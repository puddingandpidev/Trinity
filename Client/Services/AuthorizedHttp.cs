using System.Diagnostics.CodeAnalysis;
using System.Net.Http.Headers;
using System.Net.Http.Json;

namespace Client.Services;

/// <summary>
/// Builds TeamServer HTTP calls that carry the operator's bearer token.
/// </summary>
/// <remarks>
/// This exists instead of a <c>DelegatingHandler</c> on purpose. <see cref="IHttpClientFactory"/>
/// builds message handlers from the root provider, so a handler cannot see a Blazor Server
/// circuit's scoped <see cref="AuthTokenStore"/> (and cannot use protected session storage, whose
/// JS interop is only available inside a circuit). Every authenticated call is therefore built
/// here, where the scoped token store and the circuit's JS interop are available.
/// </remarks>
public sealed class AuthorizedHttp(IHttpClientFactory factory, AuthTokenStore tokens)
{
    public const string ClientName = "TeamServer";

    /// <summary>Creates a client with the current operator's bearer token attached, if any.</summary>
    public async Task<HttpClient> CreateAsync(CancellationToken cancellationToken = default)
    {
        await tokens.EnsureLoadedAsync();

        var client = factory.CreateClient(ClientName);

        if (tokens.HasValidToken)
        {
            client.DefaultRequestHeaders.Authorization =
                new AuthenticationHeaderValue("Bearer", tokens.Token);
        }

        return client;
    }

    public async Task<T?> GetFromJsonAsync<T>(string path, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).GetFromJsonAsync<T>(path, cancellationToken);

    public async Task<T?> GetFromJsonAsync<T>(Uri uri, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).GetFromJsonAsync<T>(uri, cancellationToken);

    public async Task<HttpResponseMessage> GetAsync(Uri uri, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).GetAsync(uri, cancellationToken);

    public async Task<HttpResponseMessage> GetAsync(string path, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).GetAsync(path, cancellationToken);

    public async Task<HttpResponseMessage> PostAsJsonAsync(string path, object value, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).PostAsJsonAsync(path, value, cancellationToken);

    public async Task<HttpResponseMessage> PostAsJsonAsync(Uri uri, object value, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).PostAsJsonAsync(uri, value, cancellationToken);

    public async Task<HttpResponseMessage> PostAsync(string path, HttpContent? content, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).PostAsync(path, content, cancellationToken);

    public async Task<HttpResponseMessage> PostAsync(Uri uri, HttpContent? content, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).PostAsync(uri, content, cancellationToken);

    public async Task<HttpResponseMessage> DeleteAsync(Uri uri, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).DeleteAsync(uri, cancellationToken);

    public async Task<HttpResponseMessage> PutAsJsonAsync<[DynamicallyAccessedMembers(DynamicallyAccessedMemberTypes.PublicProperties)] T>(string path, T value, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).PutAsJsonAsync(path, value, cancellationToken);

    public async Task<HttpResponseMessage> DeleteAsync(string path, CancellationToken cancellationToken = default)
        => await (await CreateAsync(cancellationToken)).DeleteAsync(path, cancellationToken);
}
