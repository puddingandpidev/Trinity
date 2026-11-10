//================================================ { START OF FILE } ================================================//
namespace TeamServer.Configurations
{
    /// <summary>
    /// Registration policy for operator accounts. Bound from the "Auth" configuration section
    /// (or <c>Auth__AllowSelfRegistration</c>). Default is <c>false</c>: a C2 server should not
    /// hand out operator accounts unless the deployment asks for it.
    /// </summary>
    public class AuthSettings
    {
        public const string SectionName = "Auth";

        /// <summary>When true, <c>POST /api/v1/auth/register</c> creates an operator.</summary>
        public bool AllowSelfRegistration { get; set; } = false;

        /// <summary>Minimum password length accepted at registration.</summary>
        public int MinimumPasswordLength { get; set; } = 8;
    }
}
//================================================ { END OF FILE } ================================================//
