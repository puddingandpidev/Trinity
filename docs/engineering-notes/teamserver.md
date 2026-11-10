# TeamServer, Client and Trinity.Shared architecture

Orientation detail for work outside `Agent/`. The frozen contracts stay in `AGENTS.md`; this note is the
map of where things are and which code paths can silently reshape data.

## TeamServer

- `Program.cs` wires the full DI graph: scoped services (`ListenerService`, `CommandService`,
  `PayloadService`, `AgentService`, `TaskService`, `OperatorService`, `CampaignService`, `AdminService`,
  `DatabaseService`, `AuthService`) and singleton factories (`ListenerFactory`, `HttpListenerManager`,
  `CommandFactory`, `AgentFactory`, `TaskResultFactory`).
- **Authentication .** `AuthService` validates operators and issues HS256 JWTs; `Program.cs`
  configures `AddAuthentication().AddJwtBearer(...)` + `AddAuthorization()` from the `Jwt` section
  (`Key` ≥ 32 bytes, `Issuer`, `Audience`, `ExpiryMinutes`). Every operator/admin controller carries
  `[Authorize]`; the agent-facing surfaces are anonymous — `POST /api/v1/agents/checkin` (UUID + payload
  content) and `/api/v1/server/status` (container healthcheck). A bootstrap operator is seeded when the
  Operators table is empty.
- **Listener checkins and file transfer.** `Trinity.Shared.Modules.HttpModule` routes
  `POST /api/v1/agents/checkin` to `IAgentService.Checkin` through an `IServiceScopeFactory` supplied by
  `HttpModuleFactory`, and `/api/files/...` to the file handlers. Handlers must return an honest status
  code (500/404 on failure) — the agent treats any 2xx as success, so a 200 ACK page for an unknown path
  used to make failed uploads and downloads report success. Modules are created per listener and live only
  in memory: after a restart, re-create the listener.
- `CMakePayloadBuilder` (implements `IPayloadBuilder`) shells out to CMake at runtime to build Agent
  payloads, injecting `HOST` and `PAYLOAD_UUID` as compile definitions and selecting the MinGW toolchain
  from the requested payload architecture.
- Listeners: `Listeners/Module.cs` defines the abstract `Module` base + `ListenerType` (HTTP/HTTPS/TCP);
  `Trinity.Shared/Modules/HttpModule.cs` wraps `System.Net.HttpListener` in a polling loop and routes agent
  checkins to the agent service.
- API routes are centralized in `Controllers/Routes.cs`
  (`/api/v1/{listeners,agents,commands,tasks,payloads,auth,operators,campaigns,admins,files}`).
- EF Core: `Data/Context.cs` (pooled via `AddDbContextPool<Context>`), Npgsql provider; migrations run in a
  separate `migrator` container built from an EF migrations bundle. The migrator fails fast on pending model
  changes.
- FluentValidation: global `ValidationFilter` on controllers + one validator per command DTO in
  `Validators/Commands/`.
- SignalR `NotificationHub` is mapped at `/register`; Swagger is served at the root path in Development.
- **Docker build dependency:** when adding system dependencies for the Agent build, also update
  `TeamServer/Dockerfile` to install them in the `runtime-base` stage, or containerized TeamServer
  instances cannot compile payloads.

### Task command writers — the place data gets silently reshaped

`CommandFactory` serialises the command DTO into `Task.Command` (`jsonb`), `TaskRequestFactory` wraps it
into a `TaskRequest`, and two other paths can rewrite the command:

| Path                                            | What it does                                                        |
| ----------------------------------------------- | ------------------------------------------------------------------- |
| `FileTransferService.PrepareDownloadChunkAsync` | Produces both the stored command **and** the on-wire chunk document |
| `CommandService.QueueTaskAsync`                 | Fills `TransferID` from the agent's campaign                        |

These writers used anonymous JSON objects, which is how `TransferID` was once dropped. Also watch
`AgentService.ProcessFullyEstablishedCheckin` (which passes the override) and `Validators/Commands/`.
`TeamServer.Tests/Contracts/TaskCommandContractTests.cs` pins the required keys per `TaskType` — extend it
whenever a command DTO is added or changed, and prefer typed records over anonymous objects for anything
serialised onto the wire.

## Client

- Blazor Server (interactive rendering) + MudBlazor.
- `TeamServerConnection` holds the runtime-configured TeamServer base URL; typed API clients
  (`AgentApiClient`, `TaskApiClient`, `AuthApiClient`, `ListenerApiClient`, `CommandApiClient`,
  `ServerApiClient`) are registered via `AddHttpClient<T>`. The JWT is attached by
  `AuthorizationMessageHandler`.
- `Interpreter/CommandInterpreter.cs` uses a keyword→strategy pattern to parse operator input into command
  DTOs.

## Trinity.Shared

- `Result<T>` is the core contract type: private constructor enforcing the success/error invariant, implicit
  conversions from `T` and `Error`.
- DTOs are organized by domain (Agent, Listener, Payload, Command, …); `Errors` holds the `Error` enum.
- `Enums/Tasks` is the server-side source of truth for `TaskType` / `TaskCode`; the Agent enum must mirror
  it (see `wire-protocol.md`).
- **It carries the listener module, which is a layering smell worth knowing about (2026-11-04).** The refactor
  moved the agent-facing listener from `TeamServer/Modules/HttpModule.cs` to
  `Trinity.Shared/Modules/HttpModule.cs` and rewrote it: the old one answered every request with
  `"<HTML><BODY> Recieved </BODY></HTML>"` and never reached the API (the recorded defect), the new one routes
  `/api/v1/agents/checkin`, `/api/files/...` and — on `agent/legacy-parity` — `.../chunks/{n}`, `.../rebuild` and
  `/api/streams/...` to handlers injected by the host, so nothing in it references TeamServer. Consequence: the
  Blazor Client references `Trinity.Shared`, so it now compiles `HttpListener` plumbing it never runs. The clean
  shape is `IHttpModuleFactory`/`Result` handlers in Shared and the implementation back under `TeamServer/`; it
  was left alone because it builds, the Client is unaffected at runtime, and moving it again would churn the
  landing commit. If a stale `TeamServer/Modules/HttpModule.cs` is still present (the owner's `dev` kept it), it is
  dead code — `HttpModuleFactory` constructs the shared one.

## Tests

`TeamServer.Tests/` is xUnit + Moq, organized by area: `Controllers/`, `Contracts/`, `Services/`. The
contract tests (`TaskCommandContractTests`) and the controller tests are the ones that catch wire-shape
regressions; write theory cases with readable names rather than one test per DTO.

Two repo docs are stale — regenerate rather than trust them:

- `API-CONTROLLER-CHECKLIST.md` links to `project1-aspnet-api/Controllers/FileTransferController.cs`, which
  does not exist; the controller is `TeamServer/Controllers/FileTransferController.cs`.
- `API-ENDPOINT-UNIT-TESTS.md` claims 70 endpoint methods with 3 tested; the suite has moved far past
  that. Rebuild the gap list from the current code.
