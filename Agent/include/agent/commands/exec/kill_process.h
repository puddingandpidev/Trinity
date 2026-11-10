// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// Terminates the process identified by <c>Command.PID</c> (PROCESS/KILL_PROCESS).
    CommandResult KillProcess(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
