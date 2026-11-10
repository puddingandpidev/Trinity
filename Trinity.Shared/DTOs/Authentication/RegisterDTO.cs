//================================================ { START OF FILE } ================================================//
namespace Trinity.Shared.DTOs.Authentication
{
    /// <summary>
    /// Self-registration request for the operator account itself (not a Firebase identity).
    /// Deliberately separate from <see cref="LoginDTO"/> so the two routes can be gated
    /// independently: login stays always-on, registration is opt-in per deployment.
    /// </summary>
    public class RegisterDTO
    {
        public string Username { get; set; } = string.Empty;
        public string Password { get; set; } = string.Empty;
    }
}
//================================================ { END OF FILE } ================================================//
