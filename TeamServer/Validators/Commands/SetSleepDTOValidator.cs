//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using FluentValidation;
using Trinity.Shared.DTOs.Command;

namespace TeamServer.Validators.Commands
{
    public class SetSleepDTOValidator : AbstractValidator<SetSleepDTO>
    {
        public SetSleepDTOValidator()
        {
            // Sleep must be a positive interval; the agent clamps below 1 s and above 24 h.
            RuleFor(x => x.Sleep).GreaterThan(0u);

            // Jitter is a percentage where 0 is the normal "no jitter" value. `NotEmpty()` treats a
            // value type's default as empty, so it rejected exactly the zero-jitter case the harness
            // sends — the rule was never able to accept the value it was meant to describe.
            RuleFor(x => x.Jitter).InclusiveBetween(0u, 100u);
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//