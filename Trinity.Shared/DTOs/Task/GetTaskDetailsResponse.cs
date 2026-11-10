//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System.Text.Json;

namespace Trinity.Shared.DTOs.Task
{
    /// <summary>
    /// Task detail returned by GET /api/v1/tasks/tasks/{ID}. <paramref name="status" /> is the
    /// server Status name, <paramref name="result" /> is the success output and
    /// <paramref name="error" /> is the failure message, so a task's outcome can be read through
    /// the API instead of the database (spec 12.1.4.5).
    /// </summary>
    public record GetTaskDetailsResponse(
        int taskID,
        int agentID,
        string commandType,
        JsonDocument command,
        DateTime timestamp,
        string status,
        string? result,
        string? error
    );
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//