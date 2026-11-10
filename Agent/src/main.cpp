// ========================================== { START OF FILE } ========================================== //
#include "agent/agent.h"
#include "agent/task/task_queue_manager.h"
#include "agent/transport/comm_manager.h"

int main() {
    agent::Agent agent{agent::transport::CommManager{}, agent::task::TaskQueueManager{}};
    agent.Start();
    return 0;
}
// ========================================== { END OF FILE } ========================================== //
