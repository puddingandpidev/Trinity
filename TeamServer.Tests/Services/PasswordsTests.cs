//================================================ { START OF FILE } ================================================//
using TeamServer.Services;
using Xunit;

namespace TeamServer.Tests.Services
{
    public class PasswordsTests
    {
        [Fact]
        public void Hash_ThenVerify_AcceptsTheSamePassword()
        {
            string stored = Passwords.Hash("correct horse battery staple");

            Assert.StartsWith("pbkdf2$", stored);
            Assert.True(Passwords.Verify(stored, "correct horse battery staple"));
        }

        [Fact]
        public void Hash_NeverContainsThePlainPassword()
        {
            string stored = Passwords.Hash("trinity-operator");

            Assert.DoesNotContain("trinity-operator", stored);
        }

        [Fact]
        public void Hash_OfTheSamePassword_DiffersPerCall()
        {
            // Salted: two accounts with the same password must not share a stored value.
            Assert.NotEqual(Passwords.Hash("same"), Passwords.Hash("same"));
        }

        [Fact]
        public void Verify_RejectsTheWrongPassword()
        {
            string stored = Passwords.Hash("one");

            Assert.False(Passwords.Verify(stored, "two"));
            Assert.False(Passwords.Verify(stored, ""));
            Assert.False(Passwords.Verify(stored, null));
        }

        [Fact]
        public void Verify_StillAcceptsLegacyPlainTextRows()
        {
            // Rows created before hashing existed must keep working (the bootstrap operator).
            Assert.True(Passwords.Verify("trinity-operator", "trinity-operator"));
            Assert.False(Passwords.Verify("trinity-operator", "wrong"));
        }

        [Fact]
        public void Verify_RejectsMalformedStoredValues()
        {
            Assert.False(Passwords.Verify("pbkdf2$notanumber$x$y", "whatever"));
            Assert.False(Passwords.Verify("pbkdf2$1000$%%%$%%%", "whatever"));
            Assert.False(Passwords.Verify("", "whatever"));
        }
    }
}
//================================================ { END OF FILE } ================================================//
