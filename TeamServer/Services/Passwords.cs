//================================================ { START OF FILE } ================================================//
using System.Security.Cryptography;
using System.Text;

namespace TeamServer.Services
{
    /// <summary>
    /// Password storage for operator accounts.
    /// </summary>
    /// <remarks>
    /// New accounts are stored as <c>pbkdf2$iterations$salt$hash</c> (PBKDF2-SHA256).
    /// Accounts created before this existed hold the password as plain text, so
    /// <see cref="Verify"/> still accepts that form; the seeded bootstrap operator keeps
    /// working and the next password change upgrades it. Nothing about the wire format
    /// changes, so the mobile and Blazor clients are unaffected.
    /// </remarks>
    public static class Passwords
    {
        private const string Prefix = "pbkdf2";
        private const int SaltBytes = 16;
        private const int HashBytes = 32;
        private const int Iterations = 100_000;

        /// <summary>Hashes a password for storage.</summary>
        public static string Hash(string password)
        {
            ArgumentNullException.ThrowIfNull(password);

            byte[] salt = RandomNumberGenerator.GetBytes(SaltBytes);
            byte[] hash = Rfc2898DeriveBytes.Pbkdf2(password, salt, Iterations, HashAlgorithmName.SHA256, HashBytes);

            return $"{Prefix}${Iterations}${Convert.ToBase64String(salt)}${Convert.ToBase64String(hash)}";
        }

        /// <summary>
        /// Verifies a supplied password against a stored value, accepting both the hashed form
        /// and the legacy plain-text form. Comparison is constant-time in both cases.
        /// </summary>
        public static bool Verify(string? stored, string? supplied)
        {
            supplied ??= string.Empty;

            if (string.IsNullOrEmpty(stored))
            {
                return false;
            }

            if (!stored.StartsWith(Prefix + "$", StringComparison.Ordinal))
            {
                // Legacy row: compare the bytes the way the previous implementation did.
                byte[] storedBytes = Encoding.UTF8.GetBytes(stored);
                byte[] suppliedBytes = Encoding.UTF8.GetBytes(supplied);
                return storedBytes.Length == suppliedBytes.Length
                    && CryptographicOperations.FixedTimeEquals(storedBytes, suppliedBytes);
            }

            string[] parts = stored.Split('$');
            if (parts.Length != 4 || !int.TryParse(parts[1], out int iterations))
            {
                return false;
            }

            try
            {
                byte[] salt = Convert.FromBase64String(parts[2]);
                byte[] expected = Convert.FromBase64String(parts[3]);
                byte[] actual = Rfc2898DeriveBytes.Pbkdf2(supplied, salt, iterations, HashAlgorithmName.SHA256, expected.Length);
                return CryptographicOperations.FixedTimeEquals(expected, actual);
            }
            catch (FormatException)
            {
                return false;
            }
        }
    }
}
//================================================ { END OF FILE } ================================================//
