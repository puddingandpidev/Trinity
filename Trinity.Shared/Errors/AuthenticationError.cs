//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Trinity.Shared.Enums;

namespace Trinity.Shared.Errors
{
    public static class AuthenticationError
    {
        public static Error InvalidCredentials()
        {
            return new Error("Auth.InvalidCredentials", "Invalid username or password.", ErrorType.Validation);
        }

        public static Error RegistrationDisabled()
        {
            return new Error(
                "Auth.RegistrationDisabled",
                "Self-registration is disabled on this deployment. Set Auth:AllowSelfRegistration to enable it.",
                ErrorType.Validation
            );
        }

        public static Error UsernameTaken(string username)
        {
            return new Error("Auth.UsernameTaken", $"The operator '{username}' already exists.", ErrorType.Conflict);
        }

        public static Error WeakCredentials(string reason)
        {
            return new Error("Auth.WeakCredentials", reason, ErrorType.Validation);
        }

        public static Error NotConfigured()
        {
            return new Error(
                "Auth.NotConfigured",
                "JWT signing is not configured. Set Jwt:Key, Jwt:Issuer and Jwt:Audience.",
                ErrorType.Failure
            );
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
