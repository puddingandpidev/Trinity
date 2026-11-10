using System.Net.Http.Json;
using System.Text.Json;
using Client.Components.Shell;
using Client.Services;
using Microsoft.AspNetCore.Components;
using Microsoft.JSInterop;

namespace Client.Components.Pages.Loot;

public partial class Loot
{
    // one received file, as the server sends it
    private sealed record ReceivedFile(string FileName, long Size, DateTime LastModified);

    private sealed class LootItem
    {
        // TODO(connect to backend): files are saved per campaign, not per agent
        public int? AgentId { get; init; }
        public int? CampaignId { get; init; }
        public required string Type { get; init; }
        public required string Name { get; init; }
        public required string PathOrValue { get; init; }
        public long? Size { get; init; }
        public long Received { get; set; }
        public required string Status { get; set; }
        public required DateTime Collected { get; init; }
    }

    [Inject]
    private IJSRuntime JS { get; set; } = default!;

    [Inject]
    private TeamServerConnection Connection { get; set; } = default!;

    [Inject]
    private CampaignService CampaignService { get; set; } = default!;

    [Inject]
    private IHttpClientFactory HttpFactory { get; set; } = default!;

    [Inject]
    private FilesApiClient FilesApi { get; set; } = default!;

    [Inject]
    private AuthorizedHttp ServerHttp { get; set; } = default!;

    [CascadingParameter]
    private WorkspaceState State { get; set; } = default!;

    [SupplyParameterFromQuery(Name = "agent")]
    public int? AgentQuery { get; set; }

    // Downloads are the only loot the TeamServer stores today; credentials and notes have no
    // server-side model yet, so they are not shown as if they existed.
    private readonly List<LootItem> _items = [];

    private string? _loadError;
    private bool _isLoading;
    private LootItem? _selected;
    private string _agentFilter = "";
    private string _typeFilter = "";
    private string? _copyStatus;

    private bool _noteOpen;
    private string _noteAgent = "";
    private string _noteName = "";
    private string _noteText = "";
    private string? _noteError;

    protected override async Task OnInitializedAsync()
    {
        State.OpenTab("Loot", "loot");
        await LoadDownloadsAsync();
        _selected = _items.FirstOrDefault();
    }

    /// <summary>
    /// Deletes a received file through the API, then reloads the listing.
    /// </summary>
    /// <remarks>
    /// Loot's Remove used to change only the in-memory row: the file stayed in the server's
    /// campaign storage for ever, because neither the API nor the UI had a removal path.
    /// </remarks>
    private async Task DeleteDownloadAsync(int campaignId, string fileName)
    {
        try
        {
            var removed = await FilesApi.DeleteCampaignFileAsync(campaignId, fileName);
            _loadError = removed ? null : $"'{fileName}' was already gone from the team server.";
            await LoadDownloadsAsync();
        }
        catch (Exception exception) when (exception is HttpRequestException or InvalidOperationException or TaskCanceledException)
        {
            _loadError = ServerDown.Is(exception) ? ServerDown.Message : $"Could not delete '{fileName}': {exception.Message}";
        }
    }

    // load the received files for every campaigns
    private async Task LoadDownloadsAsync()
    {
        if (Connection.BaseAddress is null)
        {
            _loadError = "Connect to a TeamServer from the Login page to load downloads.";
            return;
        }

        _isLoading = true;
        try
        {
            var http = await ServerHttp.CreateAsync();
            var downloads = new List<LootItem>();
            foreach (var campaign in await CampaignService.GetAllAsync())
            {
                var files = await http.GetFromJsonAsync<List<ReceivedFile>>(
                    new Uri(Connection.BaseAddress, $"api/files?campaignId={campaign.ID}")) ?? [];

                downloads.AddRange(files.Select(f => new LootItem
                {
                    CampaignId = campaign.ID,
                    Type = "Download",
                    Name = f.FileName,
                    PathOrValue = $"campaign: {campaign.Name}",
                    Size = f.Size,
                    Received = f.Size,
                    Status = "complete",
                    Collected = f.LastModified.ToLocalTime(),
                }));
            }

            _items.RemoveAll(i => i.Type == "Download");
            _items.InsertRange(0, downloads.OrderByDescending(d => d.Collected));
            _loadError = null;
        }
        catch (Exception e) when (e is HttpRequestException or JsonException or TaskCanceledException)
        {
            _loadError = ServerDown.Is(e) ? ServerDown.Message : $"Could not load downloads from the team server: {e.Message}";
        }
        finally
        {
            _isLoading = false;
        }
    }

    protected override void OnParametersSet()
    {
        if (AgentQuery is { } agent)
        {
            _agentFilter = agent.ToString();
        }
    }

    private IEnumerable<string> AgentIds => State.Agents.Select(a => a.agentID)
        .Concat(_items.Where(i => i.AgentId is not null).Select(i => i.AgentId!.Value))
        .Distinct()
        .Order()
        .Select(id => id.ToString());

    private IEnumerable<LootItem> Filtered => _items.Where(i =>
        (_agentFilter.Length == 0 || i.AgentId.ToString() == _agentFilter)
        && (_typeFilter.Length == 0 || i.Type == _typeFilter));

    private bool CanSave => _selected is { Type: "Download", Status: "complete" };

    private bool CanCancel => _selected is { Type: "Download", Status: "chunked" };

    private static string TypeIcon(string type) => type switch
    {
        "Credential" => "loot-credential.svg",
        "Note" => "loot-note.svg",
        _ => "loot-download.svg",
    };

    private static string StatusTone(string status) => status switch
    {
        "chunked" => "yellow",
        "complete" => "green",
        "cancelled" => "red",
        _ => "grey",
    };

    private static string FormatSize(long bytes) => bytes >= 1_048_576
        ? $"{bytes / 1_048_576.0:0.0} MB"
        : $"{Math.Max(1, (int)Math.Round(bytes / 1024.0))} KB";

    private static int ProgressPercent(LootItem item) =>
        item.Size is > 0 ? (int)Math.Round(100.0 * item.Received / item.Size.Value) : 0;

    private static string ReceivedLabel(LootItem item) => item.Status switch
    {
        "cancelled" => "cancelled",
        "complete" => FormatSize(item.Received),
        _ => $"{FormatSize(item.Received)} / {FormatSize(item.Size ?? 0)}",
    };

    private async Task SaveToDiskAsync()
    {
        if (_selected?.CampaignId is not { } campaignId || Connection.BaseAddress is null)
        {
            return;
        }

        // download the file from the server
        var url = new Uri(Connection.BaseAddress, $"api/files/{campaignId}/{Uri.EscapeDataString(_selected.Name)}");
        await JS.InvokeVoidAsync("open", url.ToString(), "_blank");
    }

    // TODO(connect to backend): can't cancel on the server yet, so only the screen changes
    private void CancelDownload()
    {
        if (_selected is not null)
        {
            _selected.Status = "cancelled";
            _selected.Received = 0;
        }
    }

    private async Task CopyValueAsync()
    {
        if (_selected is null)
        {
            return;
        }

        var value = _selected.Type == "Download" ? _selected.Name : _selected.PathOrValue;
        try
        {
            await JS.InvokeVoidAsync("navigator.clipboard.writeText", value);
            _copyStatus = "Copied";
        }
        catch (JSException)
        {
            // browser blocked the copy
            _copyStatus = "Copy blocked";
        }

        StateHasChanged();
        await Task.Delay(1500);
        _copyStatus = null;
    }

    private void OpenNote()
    {
        _noteAgent = _agentFilter.Length > 0 ? _agentFilter : State.SelectedAgentId?.ToString() ?? AgentIds.FirstOrDefault() ?? "";
        _noteName = "";
        _noteText = "";
        _noteError = null;
        _noteOpen = true;
    }

    // TODO(connect to backend): notes aren't saved to the server yet
    private void SaveNote()
    {
        if (!int.TryParse(_noteAgent, out var agentId) || string.IsNullOrWhiteSpace(_noteName) || string.IsNullOrWhiteSpace(_noteText))
        {
            _noteError = "Pick an agent and fill in a title and the note.";
            return;
        }

        var note = new LootItem { AgentId = agentId, Type = "Note", Name = _noteName.Trim(), PathOrValue = _noteText.Trim(), Status = "saved", Collected = DateTime.Now };
        _items.Insert(0, note);
        _selected = note;
        _noteOpen = false;
    }

    // TODO(connect to backend): cant delete on the server yet so its only removed from the screen
    private void Remove()
    {
        if (_selected is not null)
        {
            _items.Remove(_selected);
            _selected = null;
        }
    }
}
