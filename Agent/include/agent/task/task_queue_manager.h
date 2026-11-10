// ========================================== { START OF FILE } ========================================== //
#pragma once

#include <queue>
#include <string>

#include "agent/models/checkin_models.h"

namespace agent::task {

    class TaskQueueManager {
       public:
        TaskQueueManager();
        [[nodiscard]] bool HasPendingResult() const;
        [[nodiscard]] bool HasPendingTask() const;
        void StoreTaskResult(int task_id, bool is_successful, const std::string& output);
        void StoreTaskResult(int task_id, int task_type, int task_code, bool is_successful, const std::string& output);
        [[nodiscard]] std::string GetTaskResult();
        void QueueTask(const std::string& blob);
        [[nodiscard]] models::CheckinResponse GetNextTask();

       private:
        std::queue<models::ResultRequest> task_results;
        std::queue<std::string> task_queue;
    };

}  // namespace agent::task
// ========================================== { END OF FILE } ========================================== //
