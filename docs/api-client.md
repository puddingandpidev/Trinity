# Generated C# client (Refit + Refitter)

The Blazor client talks to the TeamServer through hand-written client classes. That worked, but
every URL was typed by hand and three of them had already drifted from the server:

| Caller | URL it used | Route the server serves | Failure |
| --- | --- | --- | --- |
| `PayloadService.CreateAsync` | `POST /api/v1/payloads` | `POST /api/v1/payloads/generate` | 404/405 on generate |
| `TaskApiService.GetAllAsync` | `GET /api/v1/tasks` | `GET /api/v1/tasks/tasks` | 404, dashboard showed zeros |
| `ListenerService.CreateAsync` | `POST /api/v1/listeners` | `POST /api/v1/listeners/http` | 404, listener never created |

Those were fixed by hand. Generating the client from the server's own OpenAPI document removes
the class of defect: the route cannot disagree with the caller because both come from the server.

## Generate

```bash
dotnet tool install --global Refitter          # once
scripts/gen-api-client.sh                      # live stack on http://localhost:5000
TRINITY_API_URL=http://192.168.122.20:5000 scripts/gen-api-client.sh
```

Output: `Client/Api/Generated/TrinityApi.cs` (one file: Refit interface + contracts), plus a
provenance entry in `scripts/.openapi/trinity-csharp.manifest.json` (spec hash, source URL,
generator, timestamp) and the fetched document in `scripts/.openapi/trinity-csharp.json`.

Measured on 2026-11-09 against the compose stack (`sleepydev`, TeamServer `10dc47b` + the
controller annotations from this review): **68 paths, 79 operations, 46 → 50 schemas, 40 operations
declare a response schema**. The file is 1511 lines and compiles inside `Client` unchanged, so the
generated contract types already agree with `Trinity.Shared`.

## Response schemas are a prerequisite

An operation without a 2xx response *schema* generates a bodiless method
(`Task GetAgents(...)`), which is useless to a caller. The mobile client hit the same wall and
solved it the same way: annotate the controllers. See
`trinity-mobile-client/docs/openapi-and-conformance.md`.

```csharp
[HttpGet]
[ProducesResponseType(typeof(List<Agent>), StatusCodes.Status200OK)]
public async Task<IActionResult> GetAgents() { … }
```

The annotations added in this review cover admins, operators, campaigns, armoury, files, server,
tasks, payloads and the agent check-in; 39 operations still declare no schema (mostly
`FileTransferController`'s agent-facing binary routes and the keylogger routes).

## Authentication: do not use `AddRefitClient` here

`AddRefitClient<T>()` builds its handler chain through `IHttpClientFactory`, whose handlers are
resolved from the **root** service provider. A Blazor Server circuit's `AuthTokenStore` is
**scoped per circuit**, so a handler cannot see it and cannot use protected session storage (JS
interop is only available inside a circuit). That is exactly the bug that made every page answer
`401` after sign-in in this review.

So the client builds its `HttpClient` from the scoped `AuthorizedHttp` helper and hands that to
Refit:

```csharp
var api = RestService.For<ITrinityApi>(await authorizedHttp.CreateAsync());
var agents = await api.GetAgents();
```

## Adopting it

Generated code is committed but not wired in: adoption means deleting a hand-written client class
per operation and calling the interface instead, which is a review-sized change, not something a
regeneration should do silently. Order that keeps the app working:

1. Annotate the remaining controllers (see above) and regenerate.
2. Replace the six typed `*ApiClient` classes first (they are thin pass-throughs).
3. Replace the older `*Service` classes next, then delete them (two client layers is worse than
   either one).
4. Re-run `scripts/api-conformance.sh` from the mobile tree against the same server; it compares
   the spec's operations with a generated client's `@GET/@POST` annotations and reports both
   directions.
