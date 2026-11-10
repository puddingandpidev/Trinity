//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ BEGINNING OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Trinity.Shared.Models;
using TeamServer.Services;
using Trinity.Shared.DTOs.Tool;

namespace TeamServer.Controllers
{
    // Operator-facing, so it carries the same bearer requirement as the other
    // operator controllers (KAN-42). The agent never calls this route.
    [Authorize]
    [Route(Routes.Armoury)]
    [ApiController]
    public class ArmouryController : ControllerBase
    {
        private readonly ToolService _toolService;

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Default constructor.
        /// </summary>
        /// <param name="toolService"></param>
        public ArmouryController(ToolService toolService)
        {
            _toolService = toolService;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        [HttpGet("tools")]
        [ProducesResponseType(typeof(List<Tool>), StatusCodes.Status200OK)]
        public async Task<IActionResult> GetToolsAsync([FromQuery] int pagination, [FromQuery] int rows)
        {
            var result = await _toolService.GetToolsAsync(pagination, rows);
            return result.IsSuccess ? Ok(result.Response) : BadRequest(result.Error);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        [HttpPost("upload")]
        [ProducesResponseType(StatusCodes.Status201Created)]
        public async Task<IActionResult> UploadToolAsync([FromForm] ToolCreationRequest request)
        {
            var result = await _toolService.UploadAsync(request);
            return result.IsSuccess ? StatusCode(StatusCodes.Status201Created) : BadRequest(result.Error);
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        [HttpDelete("remove/{toolID}")]
        [ProducesResponseType(StatusCodes.Status200OK)]
        public async Task<IActionResult> RemoveToolAsync([FromRoute] int toolID)
        {
            var result = await _toolService.RemoveToolAsync(toolID);
            return result.IsSuccess ? Ok() : BadRequest(result.Error);
        }
    }
}
//^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^{ END OF FILE }^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//