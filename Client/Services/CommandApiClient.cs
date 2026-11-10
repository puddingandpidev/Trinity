using System.Net;
using System.Net.Http.Json;
using System.Text.Json;
using Trinity.Shared.DTOs.Command;

namespace Client.Services;

public sealed class CommandApiClient(AuthorizedHttp httpClient, TeamServerConnection connection)
{
    /// <summary>Outcome of queueing a command: the task id, or the server's own reason for refusing.</summary>
    public sealed record QueueResult(int TaskId, string? Error)
    {
        public bool Accepted => TaskId > 0;
    }

    /// <summary>
    /// Queues a command for an agent. On refusal, the server's message is carried back.
    /// </summary>
    /// <remarks>
    /// The body used to be discarded (`if (!ok) return 0;`), so a legitimate 400 — a capability
    /// refusal, a validation failure — reached the operator as "the team server rejected the
    /// command" with no reason. The server already names the field or the unimplemented type; the
    /// client's job is to pass that on.
    /// </remarks>
    public async Task<QueueResult> QueueCommandAsync(int agentId, string pathSuffix, object body, CancellationToken cancellationToken = default)
    {
        if (connection.BaseAddress is null)
        {
            throw new InvalidOperationException("Connect to a TeamServer from the Login page before running commands.");
        }

        var requestUri = new Uri(connection.BaseAddress, $"api/v1/commands/{agentId}/{pathSuffix}");
        var response = await httpClient.PostAsJsonAsync(requestUri, body, cancellationToken);

        if (!response.IsSuccessStatusCode)
        {
            var text = await response.Content.ReadAsStringAsync(cancellationToken);
            return new QueueResult(0, DescribeFailure(response.StatusCode, text));
        }

        var created = await response.Content.ReadFromJsonAsync<CommandCreationResponse>(cancellationToken);
        return new QueueResult(created?.taskID ?? 0, created is null ? "The team server returned no task id." : null);
    }

    /// <summary>Turns an error payload into the sentence an operator should see.</summary>
    internal static string DescribeFailure(HttpStatusCode status, string body)
    {
        var trimmed = (body ?? string.Empty).Trim();
        if (trimmed.Length == 0)
        {
            return $"The team server refused the command (HTTP {(int)status}).";
        }

        try
        {
            using var doc = JsonDocument.Parse(trimmed);
            var root = doc.RootElement;

            // Error shape: { "code": "Commands.Unsupported", "description": "..." }
            if (root.TryGetProperty("description", out var description) && description.ValueKind == JsonValueKind.String)
            {
                var code = root.TryGetProperty("code", out var c) && c.ValueKind == JsonValueKind.String ? c.GetString() : null;
                return code is null ? description.GetString()! : $"{description.GetString()} ({code})";
            }

            // Validation shape: { "errors": [ { "field": "Jitter", "message": "..." } ] }
            if (root.TryGetProperty("errors", out var errors) && errors.ValueKind == JsonValueKind.Array)
            {
                var parts = new List<string>();
                foreach (var error in errors.EnumerateArray())
                {
                    var field = error.TryGetProperty("field", out var f) ? f.GetString() : null;
                    var message = error.TryGetProperty("message", out var m) ? m.GetString() : null;
                    if (!string.IsNullOrWhiteSpace(message))
                    {
                        parts.Add(field is null ? message! : $"{field}: {message}");
                    }
                }

                if (parts.Count > 0)
                {
                    return string.Join("; ", parts);
                }
            }

            if (root.TryGetProperty("title", out var title) && title.ValueKind == JsonValueKind.String)
            {
                return title.GetString()!;
            }
        }
        catch (JsonException)
        {
            // Not JSON: fall through to the raw text.
        }

        return trimmed.Length > 300 ? trimmed[..300] + "…" : trimmed;
    }
}
