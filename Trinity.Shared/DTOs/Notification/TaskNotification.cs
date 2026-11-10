//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Trinity.Shared.Enums.Tasks;

namespace Trinity.Shared.DTOs.Notification
{
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    /// <summary>
    /// The outcome of a finished task, pushed to every operator client subscribed to the agent's
    /// SignalR group. Ported from <c>clean/test</c>
    /// <c>Trinity.Shared/DTOs/Notification/TaskResponseNotification.cs</c> (commit <c>f5d1a94</c>);
    /// the type is <c>TaskNotification</c> there, so this file follows the type name.
    /// </summary>
    /// <param name="Timestamp">When the server recorded the result.</param>
    /// <param name="TaskID">The task the result belongs to.</param>
    /// <param name="Message">The task's output on success, its diagnostic on failure.</param>
    /// <param name="Status">The derived task status, never the status the agent claimed.</param>
    public record TaskNotification(DateTime Timestamp, int TaskID, string Message, Status Status);
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
