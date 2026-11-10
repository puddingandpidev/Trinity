//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Microsoft.AspNetCore.SignalR;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Interfaces;

namespace TeamServer.NotificationDispatchers
{
    //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
    /// <summary>
    /// The SignalR endpoint the operator clients connect to (<c>/register</c>). The group methods are
    /// the client-callable half of the notification contract: a client joins the agent group it is
    /// watching and the campaign group whose event log it shows. Ported from <c>clean/test</c>
    /// <c>TeamServer/NotificationDispatchers/NotificationHub.cs</c> (commits <c>73dd775</c>,
    /// <c>f3642e2</c>, <c>d5cf08c</c>).
    /// </summary>
    public class NotificationHub : Hub, INotificationHub
    {
        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Subscribes the calling client to the group of the given agent, so it receives that
        /// agent's task notifications.
        /// </summary>
        /// <param name="agentID">The agent whose task notifications the client wants.</param>
        public async Task SubscribeToAgent(string agentID)
        {
            await Groups.AddToGroupAsync(Context.ConnectionId, agentID);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Removes the calling client from the agent's group.
        /// </summary>
        /// <param name="agentID">The agent group to leave.</param>
        public async Task UnsubscribeFromAgent(string agentID)
        {
            await Groups.RemoveFromGroupAsync(Context.ConnectionId, agentID);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Subscribes the calling client to the given campaign's event-log group.
        /// </summary>
        /// <param name="campaignID">The campaign whose event log the client wants.</param>
        public async Task SubscribeToEventLog(string campaignID)
        {
            await Groups.AddToGroupAsync(Context.ConnectionId, campaignID);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Removes the calling client from the campaign's event-log group.
        /// </summary>
        /// <param name="campaignID">The campaign group to leave.</param>
        public async Task UnsubscribeFromEventLog(string campaignID)
        {
            await Groups.RemoveFromGroupAsync(Context.ConnectionId, campaignID);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Pushes a finished task's outcome to a group. Server-side callers use
        /// <see cref="NotificationDispatcher"/> (it holds the <c>IHubContext</c>); this member exists so
        /// the hub satisfies the contract and can forward from inside a hub method.
        /// </summary>
        public System.Threading.Tasks.Task SendTaskResponseAsync(string agentID, TaskNotification notification) =>
            Clients.Group(agentID).SendAsync("TaskNotification", notification);

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Pushes an event-log entry to a group. See <see cref="SendTaskResponseAsync"/> for why the
        /// server-side senders go through <see cref="NotificationDispatcher"/> instead.
        /// </summary>
        /// <inheritdoc />
        public System.Threading.Tasks.Task SendEventLogAsync(string campaignID, EventLog eventLog) =>
            string.IsNullOrEmpty(campaignID)
                        ? Clients.All.SendAsync("EventLogNotification", eventLog)
                        : Clients.Group(campaignID).SendAsync("EventLogNotification", eventLog);
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
