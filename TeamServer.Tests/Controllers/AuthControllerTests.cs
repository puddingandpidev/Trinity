// ================================================================= { START OF FILE } ================================================================= //
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using Moq;
using TeamServer.Controllers;
using TeamServer.Interface;
using Trinity.Shared.DTOs.Authentication;
using Trinity.Shared.Errors;
using Trinity.Shared.Results;
using Xunit;
using AsyncTask = System.Threading.Tasks.Task;

namespace TeamServer.Tests.Controllers
{
    public class AuthControllerTests
    {
        private readonly Mock<IAuthService> _authServiceMock;
        private readonly AuthController _controller;

        public AuthControllerTests()
        {
            _authServiceMock = new Mock<IAuthService>();
            _controller = new AuthController(_authServiceMock.Object);
        }

        [Fact]
        public async AsyncTask Login_WithValidCredentials_ReturnsOkWithToken()
        {
            // Arrange
            var loginDTO = new LoginDTO { Username = "operator", Password = "trinity-operator" };
            var response = new LoginResponseDTO
            {
                Token = "header.payload.signature",
                ExpiresAt = DateTime.UtcNow.AddHours(1),
                Username = loginDTO.Username,
                OperatorID = 1,
            };

            _authServiceMock.Setup(x => x.LoginAsync(loginDTO)).ReturnsAsync(response);

            // Act
            var result = await _controller.Login(loginDTO);

            // Assert
            var okResult = Assert.IsType<OkObjectResult>(result);
            var payload = Assert.IsType<LoginResponseDTO>(okResult.Value);
            Assert.Equal(response.Token, payload.Token);
        }

        [Fact]
        public async AsyncTask Register_WhenAllowed_ReturnsToken()
        {
            var dto = new RegisterDTO { Username = "ghost", Password = "ghost-password" };
            var response = new LoginResponseDTO
            {
                Token = "header.payload.signature",
                ExpiresAt = DateTime.UtcNow.AddHours(1),
                Username = dto.Username,
                OperatorID = 7,
            };

            _authServiceMock.Setup(x => x.RegisterAsync(dto)).ReturnsAsync(response);

            var result = await _controller.Register(dto);

            var ok = Assert.IsType<OkObjectResult>(result);
            var payload = Assert.IsType<LoginResponseDTO>(ok.Value);
            Assert.Equal(response.Token, payload.Token);
        }

        [Fact]
        public async AsyncTask Register_WithTakenUsername_ReturnsConflict()
        {
            var dto = new RegisterDTO { Username = "operator", Password = "ghost-password" };
            _authServiceMock.Setup(x => x.RegisterAsync(dto))
                .ReturnsAsync(AuthenticationError.UsernameTaken(dto.Username));

            var result = await _controller.Register(dto);

            var conflict = Assert.IsType<ConflictObjectResult>(result);
            var error = Assert.IsType<Trinity.Shared.Errors.Error>(conflict.Value);
            Assert.Equal("Auth.UsernameTaken", error.Code);
        }

        [Fact]
        public async AsyncTask Register_WhenDisabled_ReturnsForbidden()
        {
            var dto = new RegisterDTO { Username = "ghost", Password = "ghost-password" };
            _authServiceMock.Setup(x => x.RegisterAsync(dto))
                .ReturnsAsync(AuthenticationError.RegistrationDisabled());

            var result = await _controller.Register(dto);

            var forbidden = Assert.IsType<ObjectResult>(result);
            Assert.Equal(StatusCodes.Status403Forbidden, forbidden.StatusCode);
        }

        [Fact]
        public async AsyncTask Register_WithWeakCredentials_ReturnsBadRequest()
        {
            var dto = new RegisterDTO { Username = "gh", Password = "short" };
            _authServiceMock.Setup(x => x.RegisterAsync(dto))
                .ReturnsAsync(AuthenticationError.WeakCredentials("too short"));

            var result = await _controller.Register(dto);

            Assert.IsType<BadRequestObjectResult>(result);
        }

        [Fact]
        public async AsyncTask Login_WithInvalidCredentials_ReturnsUnauthorized()
        {
            // Arrange
            var loginDTO = new LoginDTO { Username = "wrongUsername", Password = "wrongPassword" };

            _authServiceMock
                .Setup(x => x.LoginAsync(loginDTO))
                .ReturnsAsync(AuthenticationError.InvalidCredentials());

            // Act
            var result = await _controller.Login(loginDTO);

            // Assert
            Assert.IsType<UnauthorizedObjectResult>(result);
        }
    }
}
// ================================================================= { END OF FILE } ================================================================= //
