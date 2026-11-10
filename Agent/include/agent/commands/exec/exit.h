// ========================================== { START OF FILE } ========================================== //
#pragma once

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands::exec {

    /// Requests that the agent stop after it has reported this task (KILL). The result of the exit task
    /// has to reach the server, so the command only records the request: `Agent::Start` delivers the
    /// pending result with one last check-in before the process ends. Returning Ok here and exiting
    /// immediately would leave the task queued forever, which is the "reported success but did nothing"
    /// failure this tree exists to prevent.
    CommandResult RequestAgentExit(const models::CheckinResponse& task);

    /// True once RequestAgentExit has accepted a task.
    [[nodiscard]] bool AgentExitRequested();

    /// Ends the process. Platform boundary: the caller is the agent loop, the Win32 call lives here.
    void ExitAgentProcess();

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
