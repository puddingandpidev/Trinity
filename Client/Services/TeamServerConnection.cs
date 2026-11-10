namespace Client.Services;

public sealed class TeamServerConnection
{
    /// <summary>
    /// The TeamServer address, taken from <c>TeamServer:BaseUrl</c> when the value is usable.
    /// </summary>
    /// <remarks>
    /// Three layouts and the login page each used to call <see cref="Configure"/> with the same
    /// configuration value. Configuring once, where the connection lives, keeps the address in
    /// step with the circuit without every page remembering to do it.
    /// </remarks>
    public TeamServerConnection(IConfiguration configuration)
    {
        if (configuration["TeamServer:BaseUrl"] is { Length: > 0 } address &&
            Uri.TryCreate(address.Trim(), UriKind.Absolute, out _))
        {
            TryConfigure(address);
        }
    }

    public Uri? BaseAddress { get; private set; }

    public void Configure(string address)
    {
        if (!Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri)
            || (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps)
            || !string.IsNullOrEmpty(uri.UserInfo)
            || !string.IsNullOrEmpty(uri.Query)
            || !string.IsNullOrEmpty(uri.Fragment))
        {
            throw new ArgumentException("Enter a complete TeamServer URL, such as https://server:50050.");
        }

        BaseAddress = new UriBuilder(uri)
        {
            Path = uri.AbsolutePath.TrimEnd('/') + "/"
        }.Uri;
    }

    /// <summary>Configures the address without throwing; returns false when it is not usable.</summary>
    public bool TryConfigure(string address)
    {
        try
        {
            Configure(address);
            return true;
        }
        catch (ArgumentException)
        {
            return false;
        }
    }
}
