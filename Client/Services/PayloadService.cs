using System.Net.Http.Json;
using Trinity.Shared.DTOs.Payload;

namespace Client.Services;

public class PayloadService
{
    private readonly AuthorizedHttp _http;

    public PayloadService(AuthorizedHttp http)
        => _http = http;

    /// <summary>
    /// GET /api/v1/payloads — returns a summary list of all payloads.
    /// Uses PayloadDTO (Name, Size, Type, CreateTime) rather than the full model,
    /// because that's what the API returns for list views.
    /// </summary>
    public async Task<List<PayloadDTO>> GetAllAsync()
        => await _http.GetFromJsonAsync<List<PayloadDTO>>("/api/v1/payloads") ?? [];

    /// <summary>
    /// POST /api/v1/payloads/generate — asks the server to build a new payload.
    /// PayloadCreationDTO carries the config: listener, campaign, architecture, type, etc.
    /// </summary>
    public async Task<bool> CreateAsync(PayloadCreationDTO dto)
    {
        var response = await _http.PostAsJsonAsync("/api/v1/payloads/generate", dto);
        return response.IsSuccessStatusCode;
    }
}