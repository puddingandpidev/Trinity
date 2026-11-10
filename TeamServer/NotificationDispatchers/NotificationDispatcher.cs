//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Microsoft.AspNetCore.SignalR;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Interfaces;

namespace TeamServer.NotificationDispatchers
{
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    /// <summary>
    /// The server-side sender for the operator UI's live updates. It wraps
    /// <see cref="IHubContext{THub}"/> so a service can broadcast without a connection of its own.
    /// <para>
    /// The reference (<c>clean/test</c>, commit <c>5917515</c>) registered the <c>Hub</c> itself as
    /// <c>INotificationHub</c> and called it from the check-in strategies. SignalR creates hub
    /// instances per connection, so a container-resolved hub has no <c>Clients</c> and the send
    /// throws; this adapter is the working form of the same contract.
    /// </para>
    /// </summary>
    public sealed class NotificationDispatcher : INotificationHub
    {
        private readonly IHubContext<NotificationHub> _hubContext;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Default constructor.
        /// </summary>
        /// <param name="hubContext">The SignalR context used to reach the connected clients.</param>
        public NotificationDispatcher(IHubContext<NotificationHub> hubContext)
        {
            _hubContext = hubContext;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <inheritdoc />
        public System.Threading.Tasks.Task SendTaskResponseAsync(string agentID, TaskNotification notification) =>
            _hubContext.Clients.Group(agentID).SendAsync("TaskNotification", notification);

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <inheritdoc />
        public System.Threading.Tasks.Task SendEventLogAsync(string campaignID, EventLog eventLog) =>
            string.IsNullOrEmpty(campaignID)
                        ? _hubContext.Clients.All.SendAsync("EventLogNotification", eventLog)
                        : _hubContext.Clients.Group(campaignID).SendAsync("EventLogNotification", eventLog);
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
