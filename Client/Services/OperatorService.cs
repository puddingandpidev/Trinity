using System.Net.Http.Json;
using Trinity.Shared.DTOs.Accounts;
using Trinity.Shared.Models;

namespace Client.Services;

public class OperatorService
{
    private readonly AuthorizedHttp _http;

    public OperatorService(AuthorizedHttp http)
        => _http = http;

    /// <summary>
    /// GET /api/v1/operators — returns all registered operators.
    /// </summary>
    // Response shape is OperatorSummary: the API no longer returns credential material.
    public async Task<List<OperatorSummary>> GetAllAsync()
        => await _http.GetFromJsonAsync<List<OperatorSummary>>("/api/v1/operators") ?? [];

    /// <summary>
    /// GET /api/v1/operators/{id} — returns a single operator.
    /// </summary>
    public async Task<OperatorSummary?> GetByIdAsync(int id)
        => await _http.GetFromJsonAsync<OperatorSummary>($"/api/v1/operators/{id}");

    /// <summary>
    /// POST /api/v1/operators — creates a new operator account.
    /// </summary>
    public async Task<bool> CreateAsync(Operator op)
    {
        var response = await _http.PostAsJsonAsync("/api/v1/operators", op);
        return response.IsSuccessStatusCode;
    }

    /// <summary>
    /// DELETE /api/v1/operators/{id} — removes an operator.
    /// </summary>
    public async Task<bool> DeleteAsync(int id)
    {
        var response = await _http.DeleteAsync($"/api/v1/operators/{id}");
        return response.IsSuccessStatusCode;
    }
}