// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <string>
#include <utility>

#include "agent/models/checkin_models.h"
#include "agent/task/task_queue_manager.h"
#include "agent/transport/comm_manager.h"

namespace agent {

    class Agent {
       public:
        Agent(transport::CommManager comm_manager, task::TaskQueueManager task_queue_manager);
        [[noreturn]] void Start();

       private:
        transport::CommManager comm_manager;
        task::TaskQueueManager task_queue_manager;
    };

}  // namespace agent
// ========================================== { END OF FILE } ========================================== //
