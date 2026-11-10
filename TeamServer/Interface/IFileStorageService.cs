// ============================================== { START OF FILE } ============================================== //
using Trinity.Shared.Results;

namespace TeamServer.Interface
{
    public interface IFileStorageService
    {
        Task<Result<string>> SaveFileAsync(IFormFile file);
        Task<Result<byte[]>> GetFileAsync(string fileName);
        Result<List<string>> ListFiles();

        /// <summary>Deletes one uploaded file by name. Refuses any name that resolves outside the store.</summary>
        Result DeleteFile(string fileName);
    }
}
// ============================================== { END OF FILE } ============================================== //