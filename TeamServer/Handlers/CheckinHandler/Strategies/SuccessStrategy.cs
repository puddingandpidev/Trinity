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
    public class SuccessStrategy : ICheckinStrategy
    {
        private readonly TaskRepository _taskRepo;
        private readonly ILogger _logger;

        private readonly INotificationHub _notificationHub;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Default constructor.
        /// </summary>
        /// <param name="taskRepository"></param>
        /// <param name="notificationHub">Pushes the result to the operator clients watching the agent.</param>
        public SuccessStrategy(TaskRepository taskRepository, INotificationHub notificationHub, ILogger<SuccessStrategy> logger)
        {
            _taskRepo = taskRepository;
            _notificationHub = notificationHub;
            _logger = logger;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Processes successful checkin task responses. 
        /// </summary>
        /// <param name="element"></param>
        /// <returns></returns>
        /// <exception cref="ArgumentNullException"></exception>
        public async System.Threading.Tasks.Task ProcessResponseAsync(JsonElement element)
        {
            var taskID = element.GetProperty("TaskID").GetInt32();
            // With the result navigation loaded: a chunked transfer reports once per chunk against
            // the same task, and the row must be updated rather than inserted again.
            var task = await _taskRepo.GetTaskWithResultAsync(taskID);
            if (task is null)
            {
                // The operator removed the task (Stop Selected Task) while the agent was still
                // running it. The result has nowhere to go, but throwing here fails the whole
                // check-in: the agent would never deliver its other results and would retry this
                // one forever. Drop it, say so, and let the check-in succeed.
                _logger.LogWarning("Result for task {TaskID} arrived after the task was removed; dropped", taskID);
                return;
            }

            var response = JsonSerializer.Deserialize<TaskResp<TaskSuccessResponse>>(element);
            if (response != null)
            {
                // Records the output and decides the status: a chunked download reports once per
                // chunk against the same task and stays queued until its final chunk.
                task.Status = task.RecordSuccess(response.Data.Output);
                await _taskRepo.CommitAsync();

                // Ported from clean/test (commit f5d1a94): the UI learns the outcome from this push
                // instead of polling. The status sent is the one just derived, not a constant.
                await _notificationHub.SendTaskResponseAsync(
                    task.AgentID.ToString(),
                    new TaskNotification(DateTime.UtcNow, taskID, response.Data.Output, task.Status));
            }
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//