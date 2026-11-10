//================================================ { START OF FILE } ================================================//
namespace TeamServer.Configurations
{
    /// <summary>
    /// JWT bearer settings bound from the "Jwt" configuration section.
    /// Ref: https://learn.microsoft.com/en-us/aspnet/core/security/authentication/configure-jwt-bearer-authentication
    /// </summary>
    public sealed class JwtSettings
    {
        public const string SectionName = "Jwt";

        /// <summary>Symmetric signing key (HS256). Must be at least 32 bytes.</summary>
        public string Key { get; set; } = string.Empty;

        public string Issuer { get; set; } = "TrinityTeamServer";

        public string Audience { get; set; } = "TrinityClient";

        public int ExpiryMinutes { get; set; } = 480;
    }
}
//================================================ { END OF FILE } ================================================//
