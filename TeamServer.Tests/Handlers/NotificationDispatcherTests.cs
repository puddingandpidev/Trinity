// ================================================================= { START OF FILE } ================================================================= //
using Microsoft.AspNetCore.SignalR;
using Moq;
using TeamServer.NotificationDispatchers;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Enums;
using Trinity.Shared.Enums.Tasks;
using Xunit;
using AsyncTask = System.Threading.Tasks.Task;

namespace TeamServer.Tests.Handlers
{
    /// <summary>
    /// Ported from <c>clean/test</c> (commits <c>f5d1a94</c>, <c>f3642e2</c>, <c>5917515</c>): the
    /// notification hub is how a finished task reaches the operator UI, so the send has to land in the
    /// right group with the right message name. The reference had no test for it; this pins the two
    /// sends the check-in strategies make.
    /// </summary>
    public class NotificationDispatcherTests
    {
        private readonly Mock<IClientProxy> _clientProxy = new();
        private readonly Mock<IHubClients> _clients = new();
        private readonly Mock<IHubContext<NotificationHub>> _hubContext = new();
        private readonly NotificationDispatcher _dispatcher;

        public NotificationDispatcherTests()
        {
            this._clients.Setup(c => c.Group(It.IsAny<string>())).Returns(this._clientProxy.Object);
            this._hubContext.SetupGet(c => c.Clients).Returns(this._clients.Object);
            this._dispatcher = new NotificationDispatcher(this._hubContext.Object);
        }

        [Fact]
        public async AsyncTask SendTaskResponseAsync_SendsToTheAgentsGroupUnderTaskNotification()
        {
            // Arrange
            var notification = new TaskNotification(DateTime.UtcNow, 7, "File uploaded successfully.", Status.SUCCESS);

            // Act
            await this._dispatcher.SendTaskResponseAsync("42", notification);

            // Assert
            this._clients.Verify(c => c.Group("42"), Times.Once);

            this._clientProxy.Verify(
                p => p.SendCoreAsync(
                    "TaskNotification",
                    It.Is<object?[]>(arguments => arguments.Length == 1 && ReferenceEquals(arguments[0], notification)),
                    It.IsAny<CancellationToken>()),
                Times.Once);
        }

        [Fact]
        public async AsyncTask SendEventLogAsync_SendsToTheCampaignsGroupUnderEventLogNotification()
        {
            // Arrange
            var entry = new EventLog(DateTime.UtcNow, EventLogType.AGENT, "Agent 42 checked in.");

            // Act
            await this._dispatcher.SendEventLogAsync("3", entry);

            // Assert
            this._clients.Verify(c => c.Group("3"), Times.Once);

            this._clientProxy.Verify(
                p => p.SendCoreAsync(
                    "EventLogNotification",
                    It.Is<object?[]>(arguments => arguments.Length == 1 && ReferenceEquals(arguments[0], entry)),
                    It.IsAny<CancellationToken>()),
                Times.Once);
        }
    }
}
// ================================================================== { END OF FILE } =================================================================== //
