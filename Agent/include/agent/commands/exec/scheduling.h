// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// PROCESS/SLEEP: replace the check-in interval and jitter for the rest of the process.
    [[nodiscard]] CommandResult SetSleep(const models::CheckinResponse& task);

    /// SET/UPDATE_HOSTS: replace the C2 host list the agent rotates through.
    [[nodiscard]] CommandResult UpdateHosts(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
