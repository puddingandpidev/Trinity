// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    CommandResult RunUnderProcess(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
