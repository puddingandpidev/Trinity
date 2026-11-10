//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using FluentValidation;
using Trinity.Shared.DTOs.Command;

namespace TeamServer.Validators.Commands
{
    public class TokenDTOValidator : AbstractValidator<TokenDTO>
    {
        public TokenDTOValidator()
        {
            RuleFor(x => x.Operation)
                .NotEmpty()
                .Must(operation =>
                    operation.Equals("make", StringComparison.OrdinalIgnoreCase)
                    || operation.Equals("steal", StringComparison.OrdinalIgnoreCase)
                    || operation.Equals("rev2self", StringComparison.OrdinalIgnoreCase)
                )
                .WithMessage("Operation must be one of: make, steal, rev2self.");
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
