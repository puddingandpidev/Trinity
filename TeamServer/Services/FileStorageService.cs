// ======================================= { START OF FILE } ======================================= //
using TeamServer.Interface;
using Trinity.Shared.Errors;
using Trinity.Shared.Results;

namespace TeamServer.Services
{
    public class FileStorageService : IFileStorageService
    {
        private readonly IWebHostEnvironment _webHostEnvironment;

        public FileStorageService(IWebHostEnvironment webHostEnvironment)
        {
            _webHostEnvironment = webHostEnvironment;
        }

        /// <summary>
        /// Retrieves a file from the uploads directory.
        /// </summary>
        /// <param name="fileName"></param>
        /// <returns></returns>
        public async Task<Result<byte[]>> GetFileAsync(string fileName)
        {
            if (string.IsNullOrWhiteSpace(fileName))
            {
                return FileError.FileNotFound(fileName);
            }

            // Check if the file exists in the uploads directory
            var storagePath = Path.Combine(
                _webHostEnvironment.ContentRootPath,
                "uploads");

            var safeFileName = Path.GetFileName(fileName);

            // Ensure the uploads directory exists
            var filePath = Path.Combine(
                storagePath,
                safeFileName);

            if (!File.Exists(filePath))
            {
                return FileError.FileNotFound(safeFileName);
            }

            var fileBytes = await File.ReadAllBytesAsync(filePath);
            return fileBytes;
        }

        /// <summary>
        /// Lists all files in the uploads directory.
        /// </summary>
        /// <returns></returns>
        public Result<List<string>> ListFiles()
        {
            // Ensure the uploads directory exists
            var storagePath = Path.Combine(
                _webHostEnvironment.ContentRootPath,
                "uploads");

            if (!Directory.Exists(storagePath))
            {
                Directory.CreateDirectory(storagePath);
            }

            // Get all files in the uploads directory and return their names. An empty directory is
            // an empty listing, not a failure: answering 400 here made a healthy endpoint look broken
            // to every caller that only checks the status code.
            var files = Directory.GetFiles(storagePath).Select(Path.GetFileName).Where(f => f != null).ToList()!;

            return files;
        }

        /// <summary>
        /// Saves a file to the uploads directory.
        /// </summary>
        /// <param name="file"></param>
        /// <returns></returns>
        public async Task<Result<string>> SaveFileAsync(IFormFile file)
        {
            if (file == null || file.Length == 0)
            {
                return FileError.EmptyFile();
            }

            // Ensure the uploads directory exists
            var storagePath = Path.Combine(
                _webHostEnvironment.ContentRootPath,
                "uploads");

            if (!Directory.Exists(storagePath))
            {
                Directory.CreateDirectory(storagePath);
            }

            // Generate a unique file name to avoid overwriting existing files
            var fileName = Path.GetFileName(file.FileName);
            var fileNameWithoutExtension =
                Path.GetFileNameWithoutExtension(fileName);

            var extension = Path.GetExtension(fileName);

            var destinationPath = Path.Combine(
                storagePath,
                fileName);

            var counter = 1;

            // If a file with the same name already exists, append a number to the file name
            while (File.Exists(destinationPath))
            {
                var newFileName =
                    $"{fileNameWithoutExtension}({counter}){extension}";

                destinationPath = Path.Combine(
                    storagePath,
                    newFileName);

                counter++;
            }

            // Save the file to the uploads directory
            await using var stream = new FileStream(
                destinationPath,
                FileMode.Create);

            await file.CopyToAsync(stream);

            return destinationPath;
        }

        //^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^//
        /// <summary>
        /// Removes one file from the uploads directory.
        /// </summary>
        /// <remarks>
        /// The store had no removal path at all — not in the API, not in the UI — so anything an
        /// operator uploaded stayed for ever. Only a bare name is accepted and the resolved path is
        /// re-checked against the store, so a crafted name cannot delete outside it.
        /// </remarks>
        public Result DeleteFile(string fileName)
        {
            if (string.IsNullOrWhiteSpace(fileName))
            {
                return FileError.FileNotFound(fileName);
            }

            var storagePath = Path.GetFullPath(Path.Combine(_webHostEnvironment.ContentRootPath, "uploads"));
            var candidate = Path.GetFullPath(Path.Combine(storagePath, Path.GetFileName(fileName)));

            if (!candidate.StartsWith(storagePath + Path.DirectorySeparatorChar, StringComparison.Ordinal) ||
                !File.Exists(candidate))
            {
                return FileError.FileNotFound(fileName);
            }

            File.Delete(candidate);
            return Result.Success();
        }
    }
}
// ======================================= { END OF FILE } ======================================= //
