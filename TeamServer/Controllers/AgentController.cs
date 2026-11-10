//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Mvc;
using System.Buffers;
using System.Text.Json;
using TeamServer.Interface;
using TeamServer.Services;
using Trinity.Shared.DTOs.Checkin;
using Trinity.Shared.DTOs.Task;
using Trinity.Shared.Models;
using Trinity.Shared.DTOs.Agent;

namespace TeamServer.Controllers
{
    [Authorize]
    [Route(Routes.Agents)]
    [ApiController]
    public class AgentController : ControllerBase
    {
        private IAgentService _agentService;
        private readonly ILogger<AgentController> _logger;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Initializes a new instance of the AgentController class with the specified AgentService.
        /// </summary>
        /// <param name="agentService">The service that provides agent-related operations.</param>
        public AgentController(IAgentService agentService, ILogger<AgentController> logger)
        {
            this._agentService = agentService;
            _logger = logger;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Retrieves all agents and returns an HTTP response containing the agents on success or a bad request on
        /// failure.
        /// </summary>
        /// <remarks>Performs an asynchronous call to the agent service to obtain the agents.</remarks>
        /// <returns>An OkObjectResult containing the agents when the operation succeeds; otherwise a BadRequestResult.</returns>
        [HttpGet()]
        [ProducesResponseType(typeof(List<AgentResponse>), StatusCodes.Status200OK)]
        public async Task<IActionResult> GetAgents()
        {
            var result = await _agentService.GetAgentsAsync();
            return result.IsSuccess ? Ok(result.Response) : BadRequest();
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        [AllowAnonymous]
        [HttpPost("checkin")]
        [ProducesResponseType(typeof(string), StatusCodes.Status200OK)]
        public async Task<IActionResult> Checkin()
        {
            using var reader = new StreamReader(Request.Body);
            string blob = await reader.ReadToEndAsync();

            // Additive headers: the agent states the interval it is running at, so the operator's
            // view cannot drift from the implant's behaviour after `set/sleep`.
            // The interval can arrive as a header or as a query parameter. Both are additive; the
            // header is preferred, and the query form keeps the observation independent of how a
            // given WinHTTP build treats a header block.
            static int FromHeader(HttpRequest request, string name) =>
                request.Headers.TryGetValue(name, out var value) && int.TryParse(value.ToString(), out var parsed)
                    ? parsed
                    : 0;

            static int FromQuery(HttpRequest request, string name) =>
                int.TryParse(request.Query[name].ToString(), out var parsed) ? parsed : 0;

            static int Pick(int header, int query) => header > 0 ? header : query;

            var fromHeader = FromHeader(Request, "X-Agent-Sleep");
            var fromQuery = FromQuery(Request, "sleep");
            if (fromHeader > 0 || fromQuery > 0)
            {
                // Which transport delivered the interval matters while both exist: "the write-back
                // works" is not the same claim as "the agent's header path works".
                _logger.LogInformation(
                    "Check-in reports interval {Sleep} ms via {Source}",
                    fromHeader > 0 ? fromHeader : fromQuery,
                    fromHeader > 0 ? "header" : "query");
            }

            var result = await _agentService.Checkin(
                blob,
                Pick(fromHeader, fromQuery),
                Pick(FromHeader(Request, "X-Agent-Jitter"), FromQuery(Request, "jitter")));

            return result.IsSuccess ? Ok(result.Response) : BadRequest();
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Generates a intial checkin request base64 encoded (testing purposes only)
        /// </summary>
        /// <param name="request"></param>
        /// <param name="uuid"></param>
        /// <returns></returns>
        [AllowAnonymous]
        [HttpPost("base64/initial/checkin")]
        public IActionResult GetIntialBase64([FromBody] IntialCheckInRequest request, [FromQuery] string uuid)
        {
            var bufferWriter = new ArrayBufferWriter<byte>();
            Span<byte> uuidSpan = bufferWriter.GetSpan(uuid.Length);
            int bytesWritten = System.Text.Encoding.UTF8.GetBytes(uuid, uuidSpan);
            bufferWriter.Advance(bytesWritten);

            using (var jsonWriter = new Utf8JsonWriter(bufferWriter))
            {
                JsonSerializer.Serialize(jsonWriter, request);
            }

            return Ok(Convert.ToBase64String(bufferWriter.WrittenSpan));
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Generates a fully established checkin request with responses base64 encoded (testing purposes only)
        /// </summary>
        /// <param name="request"></param>
        /// <param name="uuid"></param>
        /// <returns></returns>
        [AllowAnonymous]
        [HttpPost("base64/full/checkin")]
        public IActionResult GetFullBase64([FromBody] List<TaskResp<string>> request, [FromQuery] string uuid)
        {
            var bufferWriter = new ArrayBufferWriter<byte>();
            Span<byte> uuidSpan = bufferWriter.GetSpan(uuid.Length);
            int bytesWritten = System.Text.Encoding.UTF8.GetBytes(uuid, uuidSpan);
            bufferWriter.Advance(bytesWritten);

            using (var jsonWriter = new Utf8JsonWriter(bufferWriter))
            {
                JsonSerializer.Serialize(jsonWriter, request);
            }

            return Ok(Convert.ToBase64String(bufferWriter.WrittenSpan));
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        [AllowAnonymous]
        [HttpPost("base64/error/checkin")]
        public IActionResult GetFullBase64Error([FromBody] List<TaskResp<Trinity.Shared.DTOs.Task.Error>> request, [FromQuery] string uuid)
        {
            var bufferWriter = new ArrayBufferWriter<byte>();
            Span<byte> uuidSpan = bufferWriter.GetSpan(uuid.Length);
            int bytesWritten = System.Text.Encoding.UTF8.GetBytes(uuid, uuidSpan);
            bufferWriter.Advance(bytesWritten);

            using (var jsonWriter = new Utf8JsonWriter(bufferWriter))
            {
                JsonSerializer.Serialize(jsonWriter, request);
            }

            return Ok(Convert.ToBase64String(bufferWriter.WrittenSpan));
        }

    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//