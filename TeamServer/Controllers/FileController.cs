// ============================================== { START OF FILE } ============================================== //
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using TeamServer.Interface;
using TeamServer.Services;

namespace TeamServer.Controllers
{
    [ApiController]
    [Authorize]
    [Route(Routes.Files)]
    public class FilesController : ControllerBase
    {
        private readonly IFileStorageService _fileStorageService;
        public FilesController(IFileStorageService fileStorageService)
        {
            _fileStorageService = fileStorageService;
        }

        /// <summary>
        /// Uploads a file to the uploads directory.
        /// </summary>
        /// <remarks>
        /// The parameter is an IFormFile with no binding attribute: it binds from the multipart
        /// form by convention. It used to carry [FromBody], which never binds a multipart body
        /// (every upload answered 400), and the later [FromForm] made Swashbuckle throw, which
        /// broke the whole OpenAPI document (and therefore the generated client).
        /// </remarks>
        /// <param name="file">Multipart form field carrying the file.</param>
        /// <returns></returns>
        [HttpPost("upload")]
        public async Task<IActionResult> UploadFileAsync(IFormFile file)
        {
            var result = await _fileStorageService.SaveFileAsync(file);

            if (!result.IsSuccess)
            {
                return BadRequest(result.Error);
            }

            return Ok(result.Response);
        }

        /// <summary>
        /// Downloads a file from the uploads directory.
        /// </summary>
        /// <param name="fileName"></param>
        /// <returns></returns>
        [HttpGet("download/{fileName}")]
        public async Task<IActionResult> DownloadFileAsync(string fileName)
        {
            var result = await _fileStorageService.GetFileAsync(fileName);

            if (!result.IsSuccess)
            {
                return NotFound(result.Error);
            }
            return File(result.Response!, "application/octet-stream", fileName);
        }

        /// <summary>
        /// Deletes one uploaded file.
        /// </summary>
        /// <param name="fileName">Name as returned by the listing (a bare name, not a path).</param>
        /// <returns>200 when removed, 404 when it was not there.</returns>
        [HttpDelete("{fileName}")]
        [ProducesResponseType(StatusCodes.Status200OK)]
        public IActionResult DeleteFile(string fileName)
        {
            var result = _fileStorageService.DeleteFile(fileName);
            return result.IsSuccess ? Ok() : NotFound(result.Error);
        }

        /// <summary>
        /// Lists all files in the uploads directory.
        /// </summary>
        /// <returns></returns>
        [HttpGet]
        [ProducesResponseType(typeof(List<string>), StatusCodes.Status200OK)]
        public IActionResult ListFiles()
        {
            var result = _fileStorageService.ListFiles();

            if (!result.IsSuccess)
            {
                return BadRequest(result.Error);
            }

            return Ok(result.Response);
        }
    }
}
// ============================================== { END OF FILE } ============================================== //