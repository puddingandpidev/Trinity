//================================================ { START OF FILE } ================================================//
using System.Globalization;
using System.IdentityModel.Tokens.Jwt;
using System.Security.Claims;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Options;
using Microsoft.IdentityModel.Tokens;
using TeamServer.Configurations;
using TeamServer.Interface;
using TeamServer.Repositories;
using Trinity.Shared.DTOs.Authentication;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Enums;
using Trinity.Shared.Errors;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Models;
using Trinity.Shared.Results;
using Task = System.Threading.Tasks.Task;

namespace TeamServer.Services
{
    /// <summary>
    /// Issues JWT bearer tokens for operators of the Client web app.
    /// Ref: Configure JWT bearer authentication in ASP.NET Core
    /// https://learn.microsoft.com/en-us/aspnet/core/security/authentication/configure-jwt-bearer-authentication
    /// </summary>
    public class AuthService : IAuthService
    {
        private readonly OperatorRepository _operatorRepository;
        private readonly JwtSettings _settings;
        private readonly INotificationHub _notificationHub;
        private readonly AuthSettings _authSettings;

        public AuthService(
            OperatorRepository operatorRepository,
            IOptions<JwtSettings> settings,
            INotificationHub notificationHub,
            IOptions<AuthSettings> authSettings)
        {
            _operatorRepository = operatorRepository;
            _settings = settings.Value;
            _notificationHub = notificationHub;
            _authSettings = authSettings.Value;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Validates the supplied credentials against the Operators table and, on success,
        /// returns a signed JWT containing the operator identity and role claims.
        /// </summary>
        public async Task<Result<LoginResponseDTO>> LoginAsync(LoginDTO login)
        {
            if (login is null || string.IsNullOrWhiteSpace(login.Username) || string.IsNullOrEmpty(login.Password))
            {
                return AuthenticationError.InvalidCredentials();
            }

            var operators = await _operatorRepository.GetOperatorsAsync();

            var operatorEntity = operators.FirstOrDefault(o =>
                string.Equals(o.Username, login.Username, StringComparison.OrdinalIgnoreCase)
            );

            if (operatorEntity is null || !PasswordsMatch(operatorEntity.Password, login.Password))
            {
                // Same error for unknown user and bad password so the endpoint does not leak
                // which usernames exist.
                return AuthenticationError.InvalidCredentials();
            }

            var token = CreateToken(operatorEntity);

            // Event log: who signed in. A sign-in has no campaign, so it is broadcast to every
            // operator client (the dispatcher treats an empty campaign id as "all").
            await _notificationHub.SendEventLogAsync(
                string.Empty,
                new EventLog(DateTime.UtcNow, EventLogType.OPERATOR, $"Operator '{operatorEntity.Username}' signed in"));

            return token;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Builds the signed JWT. Claims: sub/nameidentifier (operator ID), name/unique_name
        /// (username) and role (Operator).
        /// </summary>
        private Result<LoginResponseDTO> CreateToken(Operator operatorEntity)
        {
            if (string.IsNullOrWhiteSpace(_settings.Key) || Encoding.UTF8.GetByteCount(_settings.Key) < 32)
            {
                return AuthenticationError.NotConfigured();
            }

            var signingKey = new SymmetricSecurityKey(Encoding.UTF8.GetBytes(_settings.Key));
            var credentials = new SigningCredentials(signingKey, SecurityAlgorithms.HmacSha256);
            var issuedAt = DateTime.UtcNow;
            var expiresAt = issuedAt.AddMinutes(_settings.ExpiryMinutes);
            var operatorID = operatorEntity.ID.ToString(CultureInfo.InvariantCulture);

            var claims = new[]
            {
                new Claim(JwtRegisteredClaimNames.Sub, operatorID),
                new Claim(JwtRegisteredClaimNames.UniqueName, operatorEntity.Username),
                new Claim(JwtRegisteredClaimNames.Jti, Guid.NewGuid().ToString()),
                new Claim(ClaimTypes.NameIdentifier, operatorID),
                new Claim(ClaimTypes.Name, operatorEntity.Username),
                new Claim(ClaimTypes.Role, "Operator"),
            };

            var token = new JwtSecurityToken(
                issuer: _settings.Issuer,
                audience: _settings.Audience,
                claims: claims,
                notBefore: issuedAt,
                expires: expiresAt,
                signingCredentials: credentials
            );

            var response = new LoginResponseDTO
            {
                Token = new JwtSecurityTokenHandler().WriteToken(token),
                TokenType = "Bearer",
                ExpiresAt = expiresAt,
                Username = operatorEntity.Username,
                OperatorID = operatorEntity.ID,
            };

            return response;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Creates an operator account when self-registration is enabled, then signs it in.
        /// </summary>
        /// <remarks>
        /// Off unless <c>Auth:AllowSelfRegistration</c> is true. The account is always a plain
        /// operator (no admin role exists on the model), the password is stored as a PBKDF2
        /// hash, and the response is the same token shape <c>login</c> returns so a client can
        /// continue straight to the API. This exists so an Android build can be exercised
        /// without a Firebase identity; it is not a general user-management surface.
        /// </remarks>
        public async Task<Result<LoginResponseDTO>> RegisterAsync(RegisterDTO register)
        {
            if (!_authSettings.AllowSelfRegistration)
            {
                return AuthenticationError.RegistrationDisabled();
            }

            if (register is null || string.IsNullOrWhiteSpace(register.Username))
            {
                return AuthenticationError.WeakCredentials("A username is required.");
            }

            string username = register.Username.Trim();
            if (username.Length < 3)
            {
                return AuthenticationError.WeakCredentials("The username must be at least 3 characters.");
            }

            if (string.IsNullOrEmpty(register.Password) || register.Password.Length < _authSettings.MinimumPasswordLength)
            {
                return AuthenticationError.WeakCredentials(
                    $"The password must be at least {_authSettings.MinimumPasswordLength} characters.");
            }

            var operators = await _operatorRepository.GetOperatorsAsync();
            if (operators.Any(o => string.Equals(o.Username, username, StringComparison.OrdinalIgnoreCase)))
            {
                return AuthenticationError.UsernameTaken(username);
            }

            var created = new Operator
            {
                Username = username,
                Password = TeamServer.Services.Passwords.Hash(register.Password),
                LastLogin = DateTime.UtcNow,
            };

            await _operatorRepository.InsertOperatorAsync(created);
            await _operatorRepository.CommitAsync();

            await _notificationHub.SendEventLogAsync(
                string.Empty,
                new EventLog(DateTime.UtcNow, EventLogType.OPERATOR, $"Operator '{username}' registered"));

            var login = await LoginAsync(new LoginDTO { Username = username, Password = register.Password });
            return login;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Constant-time credential comparison (operator passwords are stored as supplied by
        /// the admin API; hashing is tracked separately).
        /// </summary>
        private static bool PasswordsMatch(string storedPassword, string suppliedPassword)
        {
            return Passwords.Verify(storedPassword, suppliedPassword);
        }
    }
}
//================================================ { END OF FILE } ================================================//
