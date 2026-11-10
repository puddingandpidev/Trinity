using System.Text.Json;
using Client.Components.Shell;
using Client.Services;
using Microsoft.AspNetCore.Components;
using Microsoft.AspNetCore.SignalR.Client;
using Trinity.Shared.DTOs.Notification;
using Trinity.Shared.DTOs.Task;
using Trinity.Shared.Enums;

namespace Client.Components.Pages.Logs;

public partial class Logs : IAsyncDisposable
{
    private sealed record EventEntry(DateTime Time, string Category, string Tone, IReadOnlyList<EventPart> Parts);

    private sealed record EventPart(string Text, string Style = "");

    [Inject]
    private TaskApiClient TaskApiClient { get; set; } = default!;

    [Inject]
    private CampaignService CampaignService { get; set; } = default!;

    [Inject]
    private IConfiguration Config { get; set; } = default!;

    [CascadingParameter]
    private WorkspaceState State { get; set; } = default!;

    private readonly CancellationTokenSource _stop = new();
    private List<EventEntry> _events = [];
    private List<EventEntry> _liveEvents = [];
    private HubConnection? _hubConnection;
    private string? _loadError;
    private bool _isLoading;

    // Live event-log entries first (most recent at the top), then the task-derived history.
    private IReadOnlyList<EventEntry> Events => [.. this._liveEvents, .. this._events];

    protected override async Task OnInitializedAsync()
    {
        State.OpenTab("Event Log", "logs");
        await LoadAsync();
        await ConnectHubAsync();
        _ = PollAsync();
    }

    // The hub pushes TeamServer events (operator sign-in, listener and payload creation, agent
    // check-in, file transfers) to campaign groups; campaign-less events are broadcast to everyone.
    // Subscribing to every campaign keeps the tab complete without knowing which one is "active".
    private async Task ConnectHubAsync()
    {
        var baseUrl = Config["TeamServer:BaseUrl"]!;
        _hubConnection = new HubConnectionBuilder()
            .WithUrl($"{baseUrl}/register")
            .WithAutomaticReconnect()
            .Build();

        _hubConnection.On<EventLog>("EventLogNotification", async entry =>
        {
            _liveEvents.Insert(0, ToEvent(entry));
            await InvokeAsync(StateHasChanged);
        });

        try
        {
            await _hubConnection.StartAsync();

            foreach (var campaign in await CampaignService.GetAllAsync())
            {
                await _hubConnection.InvokeAsync("SubscribeToEventLog", campaign.ID.ToString());
            }
        }
        catch (Exception e)
        {
            // The tab still shows the task history; a missing hub must not break the page.
            _loadError ??= $"Live event log unavailable: {e.Message}";
        }
    }

    private static EventEntry ToEvent(EventLog entry)
    {
        var time = entry.Timestamp.Kind == DateTimeKind.Utc ? entry.Timestamp.ToLocalTime() : entry.Timestamp;
        return new EventEntry(time, entry.Type.ToString().ToLowerInvariant(), "green", [new(entry.Message)]);
    }

    private async Task PollAsync()
    {
        using var timer = new PeriodicTimer(TimeSpan.FromSeconds(10));
        try
        {
            while (await timer.WaitForNextTickAsync(_stop.Token))
            {
                await LoadAsync();
                await InvokeAsync(StateHasChanged);
            }
        }
        catch (OperationCanceledException)
        {
        }
    }

    // TODO(connect to backend): no event log yet, so show tasks instead
    private async Task LoadAsync()
    {
        _isLoading = true;
        try
        {
            var tasks = await TaskApiClient.GetTasksAsync(_stop.Token);
            _events = tasks.OrderBy(t => t.timeStamp).Select(ToEvent).ToList();
            _loadError = null;
        }
        catch (Exception e) when (e is HttpRequestException or InvalidOperationException or JsonException or TaskCanceledException)
        {
            _loadError = ServerDown.Is(e) ? ServerDown.Message : $"Could not load events from the team server: {e.Message}";
        }
        finally
        {
            _isLoading = false;
        }
    }

    private static EventEntry ToEvent(TaskResponse task)
    {
        var failed = Client.Services.TaskStatuses.IsFailed(task.status);
        var time = task.timeStamp.Kind == DateTimeKind.Utc ? task.timeStamp.ToLocalTime() : task.timeStamp;
        EventPart[] parts = failed
            ?
            [
                new(task.agentID.ToString(), "em"),
                new($" {task.commandType} failed: "),
                new(task.status, "err"),
            ]
            :
            [
                new(task.agentID.ToString(), "em"),
                new($" {task.commandType} · {task.status}"),
            ];

        return new EventEntry(time, failed ? "error" : "agent", failed ? "red" : "green", parts);
    }

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _stop.Dispose();

        if (_hubConnection is not null)
        {
            await _hubConnection.DisposeAsync();
        }
    }
}
