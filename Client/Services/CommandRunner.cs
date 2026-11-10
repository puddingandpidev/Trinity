using Trinity.Shared.DTOs.Task;

namespace Client.Services;

/// <summary>
/// Runs one command line against an agent: parses the verb, queues the matching TeamServer
/// command and waits for the agent's result.
/// </summary>
/// <remarks>
/// This replaces the Refit <c>ICommandAPI</c> layer and its sixteen strategy classes. That layer
/// duplicated the route table by hand, needed the Refit reflection builder (which was not
/// installed, so the two terminals that used it threw on render) and pointed at a hard-coded
/// <c>https://localhost:7251</c>. One parser and <see cref="CommandApiClient"/> is enough for the
/// verbs the agent implements, and the error text now says what is missing instead of silently
/// queueing an empty command.
/// </remarks>
public sealed class CommandRunner(CommandApiClient commands, TaskApiClient tasks)
{
    /// <summary>How long to wait for the agent to report the task's outcome.</summary>
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(45);

    public sealed record Outcome(bool Accepted, int TaskId, string Message, string? Output)
    {
        public bool Succeeded => Output is not null;
    }

    public async Task<Outcome> RunAsync(int agentId, string commandLine, TimeSpan? timeout = null, CancellationToken cancellationToken = default)
    {
        var parsed = Parse(commandLine);
        if (!parsed.Ok)
        {
            return new Outcome(false, 0, parsed.Error!, null);
        }

        CommandApiClient.QueueResult queued;
        try
        {
            queued = await commands.QueueCommandAsync(agentId, parsed.PathSuffix!, parsed.Body!, cancellationToken);
        }
        catch (Exception exception) when (exception is HttpRequestException or InvalidOperationException or System.Text.Json.JsonException)
        {
            return new Outcome(false, 0, ServerDown.Is(exception) ? ServerDown.Message : exception.Message, null);
        }

        if (!queued.Accepted)
        {
            return new Outcome(false, 0, queued.Error ?? "The team server refused the command.", null);
        }

        var taskId = queued.TaskId;

        var deadline = DateTime.UtcNow + (timeout ?? DefaultTimeout);
        while (DateTime.UtcNow < deadline && !cancellationToken.IsCancellationRequested)
        {
            await Task.Delay(1200, cancellationToken);

            GetTaskDetailsResponse? detail;
            try
            {
                detail = await tasks.GetTaskDetailAsync(taskId, cancellationToken);
            }
            catch (Exception exception) when (exception is HttpRequestException or System.Text.Json.JsonException)
            {
                continue;
            }

            if (detail is null)
            {
                continue;
            }

            var kind = TaskStatuses.Kind(detail.status);
            if (kind is TaskStatusKind.Success)
            {
                return new Outcome(true, taskId, $"Task {taskId} succeeded.", string.IsNullOrWhiteSpace(detail.result) ? "(no output)" : detail.result);
            }

            if (kind is TaskStatusKind.Failed)
            {
                return new Outcome(true, taskId, $"Task {taskId} failed.", detail.error ?? detail.result ?? "(no detail)");
            }
        }

        return new Outcome(true, taskId, $"Task {taskId} queued; it has not reported back yet (check Tasks).", null);
    }

    public sealed record ParseResult(bool Ok, string? PathSuffix, object? Body, string? Error);

    /// <summary>
    /// Maps a command line to a TeamServer route and body. Verbs mirror the agent's own
    /// dispatcher; anything unknown is rejected rather than queued as an empty command.
    /// </summary>
    public static ParseResult Parse(string commandLine)
    {
        var line = (commandLine ?? string.Empty).Trim();
        if (line.Length == 0)
        {
            return new ParseResult(false, null, null, "Enter a command.");
        }

        var space = line.IndexOf(' ');
        var verb = (space < 0 ? line : line[..space]).ToLowerInvariant();
        var rest = space < 0 ? string.Empty : line[(space + 1)..].Trim();

        string[] Split(int max = 2) => rest.Split(' ', max, StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        static bool TryInt(string value, out int number) => int.TryParse(value, out number);

        switch (verb)
        {
            case "shell":
                return rest.Length == 0
                    ? new ParseResult(false, null, null, "Usage: shell <command>")
                    : new ParseResult(true, "spawn/shell", new { Command = rest }, null);

            case "powershell":
            case "ps":
                return rest.Length == 0
                    ? new ParseResult(false, null, null, "Usage: powershell <commandlet> [arguments]")
                    : rest.Contains(' ', StringComparison.Ordinal)
                        ? new ParseResult(true, "spawn/powershell", new { Commandlet = rest[..rest.IndexOf(' ')], Arguements = rest[(rest.IndexOf(' ') + 1)..] }, null)
                        : new ParseResult(true, "spawn/powershell", new { Commandlet = rest, Arguements = string.Empty }, null);

            case "getuid":
                return new ParseResult(true, "execute/getuid", new { }, null);

            case "exit":
            case "killagent":
                return new ParseResult(true, "execute/kill", new { }, null);

            case "kill":
            case "killprocess":
                return TryInt(rest, out var killPid)
                    ? new ParseResult(true, "execute/killprocess", new { PID = killPid }, null)
                    : new ParseResult(false, null, null, "Usage: kill <pid>");

            case "sleep":
                return TryInt(rest, out var sleep)
                    ? new ParseResult(true, "set/sleep", new { Sleep = sleep }, null)
                    : new ParseResult(false, null, null, "Usage: sleep <milliseconds>");

            case "run":
            {
                var parts = Split();
                return parts.Length < 1
                    ? new ParseResult(false, null, null, "Usage: run <program> [arguments]")
                    : new ParseResult(true, "spawn/run", new { Program = parts[0], Arguements = parts.Length > 1 ? parts[1] : string.Empty }, null);
            }

            case "runas":
            {
                // runas DOMAIN\user <command>
                var parts = Split(3);
                if (parts.Length < 2 || !parts[0].Contains('\\', StringComparison.Ordinal))
                {
                    return new ParseResult(false, null, null, "Usage: runas DOMAIN\\user <command> [arguments]");
                }

                var identity = parts[0].Split('\\', 2);
                return new ParseResult(true, "spawn/runas",
                    new { Domain = identity[0], User = identity[1], Command = parts[1], Arguements = parts.Length > 2 ? parts[2] : string.Empty }, null);
            }

            case "runu":
            {
                // runu <parent pid> <command>
                var parts = Split(2);
                return parts.Length < 2 || !TryInt(parts[0], out var parentPid)
                    ? new ParseResult(false, null, null, "Usage: runu <parentPid> <command>")
                    : new ParseResult(true, "spawn/runu", new { PID = parentPid, Command = parts[1], Arguements = string.Empty }, null);
            }

            case "escalate":
            {
                var parts = Split();
                return parts.Length < 2
                    ? new ParseResult(false, null, null, "Usage: escalate <registryKey> <targetBinary>")
                    : new ParseResult(true, "execute/escalate", new { RegKey = parts[0], TargetBinary = parts[1] }, null);
            }

            case "netasm":
            case "dotnet":
            {
                var parts = Split();
                return parts.Length < 1
                    ? new ParseResult(false, null, null, "Usage: netasm <base64 assembly> [arguments]")
                    : new ParseResult(true, "execute/dotnetassembly", new { Assembly = parts[0], Arguments = parts.Length > 1 ? parts[1] : string.Empty }, null);
            }

            case "bof":
            {
                var parts = Split();
                return parts.Length < 1
                    ? new ParseResult(false, null, null, "Usage: bof <base64> [entrypoint] [arguments]")
                    : new ParseResult(true, "execute/bof",
                        new { Bof = parts[0], Entrypoint = parts.Length > 1 ? parts[1] : string.Empty, Arguements = parts.Length > 2 ? parts[2] : string.Empty }, null);
            }

            case "download":
                return rest.Length == 0
                    ? new ParseResult(false, null, null, "Usage: download <remote path>")
                    : new ParseResult(true, "execute/filedownload", new { Path = rest }, null);

            case "cancel":
                return TryInt(rest, out var cancelTask)
                    ? new ParseResult(true, "execute/cancelfiledownload", new { TaskID = cancelTask }, null)
                    : new ParseResult(false, null, null, "Usage: cancel <taskId>");

            case "upload":
            {
                var parts = Split();
                return parts.Length < 1
                    ? new ParseResult(false, null, null, "Usage: upload <local path> [remote path]")
                    : new ParseResult(true, "execute/upload", new { Path = parts[0], RemotePath = parts.Length > 1 ? parts[1] : string.Empty }, null);
            }

            case "spawnto":
                return rest.Length == 0
                    ? new ParseResult(false, null, null, "Usage: spawnto <path>")
                    : new ParseResult(true, "set/spawnto", new { Path = rest }, null);

            case "updatehosts":
                return rest.Length == 0
                    ? new ParseResult(false, null, null, "Usage: updatehosts <host[,host]>")
                    : new ParseResult(true, "set/updatehosts",
                        new { Hosts = rest.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries).ToList() }, null);

            case "help":
                return new ParseResult(false, null, null,
                    "shell | powershell | getuid | kill <pid> | sleep <ms> | run <program> | runas <domain\\user> <cmd> | " +
                    "runu <pid> <cmd> | escalate <regkey> <binary> | netasm <b64> | bof <b64> | download <path> | " +
                    "cancel <taskId> | upload <path> | spawnto <path> | updatehosts <hosts> | exit");

            default:
                return new ParseResult(false, null, null, $"Unknown command '{verb}'. Type help.");
        }
    }
}
