//================================================ { START OF FILE } ================================================//
using Trinity.Shared.DTOs.Authentication;
using Trinity.Shared.Results;
using Task = System.Threading.Tasks.Task;

namespace TeamServer.Interface
{
    public interface IAuthService
    {
        /// <summary>
        /// Validates operator credentials and issues a signed JWT.
        /// </summary>
        Task<Result<LoginResponseDTO>> LoginAsync(LoginDTO login);

        /// <summary>
        /// Creates an operator account when self-registration is enabled and signs it in.
        /// </summary>
        Task<Result<LoginResponseDTO>> RegisterAsync(RegisterDTO register);
    }
}
//================================================ { END OF FILE } ================================================//
