using System.Net.Http.Headers;

namespace Client.Services;

/// <summary>
/// Operator-side file storage on the TeamServer: list, upload, download.
/// </summary>
/// <remarks>
/// Distinct from the agent transfer routes (<c>/api/files/{agentKey}/…</c>), which move files
/// between an implant and the server. These are the files an operator stores and retrieves.
/// </remarks>
public sealed class FilesApiClient(AuthorizedHttp http, TeamServerConnection connection)
{
    private Uri RequireBase()
        => connection.BaseAddress ?? throw new InvalidOperationException(
            "Connect to a TeamServer from the Login page before using files.");

    /// <summary>GET /api/v1/files — file names in the server's uploads directory.</summary>
    public async Task<List<string>> ListAsync(CancellationToken cancellationToken = default)
    {
        RequireBase();
        return await http.GetFromJsonAsync<List<string>>("api/v1/files", cancellationToken) ?? [];
    }

    /// <summary>POST /api/v1/files/upload — stores one file; returns the stored name.</summary>
    public async Task<string?> UploadAsync(Stream content, string fileName, CancellationToken cancellationToken = default)
    {
        RequireBase();

        using var form = new MultipartFormDataContent();
        var part = new StreamContent(content);
        part.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
        // The field name must match the action's parameter name ("file") for form binding.
        form.Add(part, "file", fileName);

        var client = await http.CreateAsync(cancellationToken);
        var response = await client.PostAsync(new Uri(RequireBase(), "api/v1/files/upload"), form, cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            return null;
        }

        return await response.Content.ReadAsStringAsync(cancellationToken);
    }

    /// <summary>GET /api/v1/files/download/{name} — raw bytes of a stored file.</summary>
    public async Task<byte[]?> DownloadAsync(string fileName, CancellationToken cancellationToken = default)
    {
        RequireBase();

        var client = await http.CreateAsync(cancellationToken);
        var response = await client.GetAsync(
            new Uri(RequireBase(), $"api/v1/files/download/{Uri.EscapeDataString(fileName)}"), cancellationToken);

        return response.IsSuccessStatusCode
            ? await response.Content.ReadAsByteArrayAsync(cancellationToken)
            : null;
    }

    /// <summary>DELETE /api/v1/files/{name} — removes one stored file. False when it was not there.</summary>
    public async Task<bool> DeleteAsync(string fileName, CancellationToken cancellationToken = default)
    {
        RequireBase();

        var client = await http.CreateAsync(cancellationToken);
        var response = await client.DeleteAsync(
            new Uri(RequireBase(), $"api/v1/files/{Uri.EscapeDataString(fileName)}"), cancellationToken);
        return response.IsSuccessStatusCode;
    }

    /// <summary>DELETE /api/files/{campaignId}/{name} — removes a file an agent uploaded.</summary>
    public async Task<bool> DeleteCampaignFileAsync(int campaignId, string fileName, CancellationToken cancellationToken = default)
    {
        RequireBase();

        var client = await http.CreateAsync(cancellationToken);
        var response = await client.DeleteAsync(
            new Uri(RequireBase(), $"api/files/{campaignId}/{Uri.EscapeDataString(fileName)}"), cancellationToken);
        return response.IsSuccessStatusCode;
    }
}
