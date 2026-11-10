//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System.Text.Json;
using Microsoft.Extensions.Logging;
using TeamServer.Repositories;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.DTOs.Task;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Models;

namespace TeamServer.Handlers.CheckinHandler.Strategies
{
    public class ErrorStrategy : ICheckinStrategy
    {
        private readonly TaskRepository _taskRepo;
        private readonly ILogger _logger;

        private readonly INotificationHub _notificationHub;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Default constructor.
        /// </summary>
        /// <param name="taskRepository"></param>
        /// <param name="notificationHub">Pushes the failure to the operator clients watching the agent.</param>
        public ErrorStrategy(TaskRepository taskRepository, INotificationHub notificationHub, ILogger<ErrorStrategy> logger)
        {
            _taskRepo = taskRepository;
            _notificationHub = notificationHub;
            _logger = logger;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        public async System.Threading.Tasks.Task ProcessResponseAsync(JsonElement element)
        {
            var taskID = element.GetProperty("TaskID").GetInt32();
            var task = await _taskRepo.GetByIdAsync(taskID);
            if (task is null)
            {
                _logger.LogWarning("Failure result for task {TaskID} arrived after the task was removed; dropped", taskID);
                return;
            }

            var response = JsonSerializer.Deserialize<TaskResp<Trinity.Shared.DTOs.Task.Error>>(element);

            if (response != null)
            {
                task.Status = Status.ERROR;
                task.Error = new Trinity.Shared.Models.Error { TaskID = response.TaskID, ErrorCode = response.Data.ErrorCode, Message = response.Data.Message };
                await _taskRepo.CommitAsync();

                // Ported from clean/test (commit f5d1a94), with its defect fixed: the reference sent
                // Status.SUCCESS for a failed task, so a client could only render it as a success.
                await _notificationHub.SendTaskResponseAsync(
                    task.AgentID.ToString(),
                    new TaskNotification(DateTime.UtcNow, taskID, response.Data.Message, task.Status));
            }
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//