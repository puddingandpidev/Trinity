using Microsoft.AspNetCore.Components.Server.ProtectedBrowserStorage;
using Trinity.Shared.DTOs.Authentication;

namespace Client.Services;

/// <summary>
/// Holds the JWT issued by the TeamServer for the current operator session.
/// </summary>
/// <remarks>
/// The value is kept in memory for the circuit and mirrored into protected session storage.
/// Blazor Server renders the first request in a scope that is discarded before the interactive
/// circuit starts, so an in-memory-only token is lost on navigation or refresh: every
/// authenticated call after sign-in then answered 401. <see cref="EnsureLoadedAsync"/> restores
/// it, and <see cref="AuthorizationMessageHandler"/> awaits that before attaching the header.
/// </remarks>
public sealed class AuthTokenStore(ProtectedSessionStorage storage)
{
    private const string StorageKey = "trinity.auth.token";

    public string? Token { get; private set; }

    public DateTime? ExpiresAt { get; private set; }

    public string? Username { get; private set; }

    public bool HasValidToken => !string.IsNullOrEmpty(Token) && (ExpiresAt is null || ExpiresAt > DateTime.UtcNow);

    private Task? _restore;
    public void Set(LoginResponseDTO response)
    {
        ArgumentNullException.ThrowIfNull(response);
        Set(response.Token, response.ExpiresAt, response.Username);
    }

    /// <summary>Restores the token from protected session storage once per circuit.</summary>
    /// <remarks>
    /// The restore is cached as a <see cref="Task"/>, not as a flag: several components ask for the
    /// token concurrently during a page render, and a flag set before the storage read completes
    /// let every request after the first go out anonymously (the page then answered 401).
    /// Prerendering has no JS interop, so the restore is retried on the next call instead of
    /// failing the request that triggered it.
    /// </remarks>
    public async Task EnsureLoadedAsync()
    {
        if (!string.IsNullOrEmpty(Token))
        {
            return;
        }

        _restore ??= RestoreAsync();
        await _restore;
    }

    private async Task RestoreAsync()
    {
        try
        {
            var stored = await storage.GetAsync<StoredToken>(StorageKey);
            if (stored is { Success: true, Value: { } value } && !string.IsNullOrEmpty(value.Token))
            {
                Set(value.Token, value.ExpiresAt, value.Username);
            }
        }
        catch (Exception)
        {
            // Prerender (no JS interop yet), a disconnected circuit, or a value encrypted with a
            // key ring that no longer exists (container rebuild): stay anonymous and let the
            // operator sign in again.
            _restore = null;
        }
    }

    /// <summary>Persists the token for the operator session.</summary>
    public async Task SetAsync(LoginResponseDTO response)
    {
        Set(response);
        await PersistAsync();
    }

    /// <summary>Forgets the token in memory and in protected session storage.</summary>
    public async Task ClearAsync()
    {
        Clear();

        try
        {
            await storage.DeleteAsync(StorageKey);
        }
        catch (Exception)
        {
            // Prerender or a dead circuit: the in-memory clear is enough.
        }
    }

    public void Clear()
    {
        Token = null;
        ExpiresAt = null;
        Username = null;
    }

    private void Set(string? token, DateTime? expiresAt, string? username)
    {
        Token = string.IsNullOrEmpty(token) ? null : token;
        ExpiresAt = expiresAt is { } value
            ? (value.Kind == DateTimeKind.Unspecified ? DateTime.SpecifyKind(value, DateTimeKind.Utc) : value.ToUniversalTime())
            : null;
        Username = username;
    }

    private async Task PersistAsync()
    {
        if (Token is null)
        {
            return;
        }

        try
        {
            await storage.SetAsync(StorageKey, new StoredToken(Token, ExpiresAt, Username));
        }
        catch (Exception)
        {
            // Prerender: the interactive pass persists it on the next sign-in.
        }
    }

    private sealed record StoredToken(string? Token, DateTime? ExpiresAt, string? Username);
}
