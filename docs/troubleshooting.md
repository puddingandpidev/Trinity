# Troubleshooting (KAN-137)

Failures this project has actually hit, each with the symptom and the check. Start with the two rules that
explain most "it does nothing" reports.

## Read the right column first

- **A task that failed has no result.** A success stores its output in `TaskResult.Result`; a failure sets
  `Tasks.Status = 'error'` and puts the diagnostic in `Errors.Message` (not `TaskResult`). Querying only
  `TaskResult` makes a failed task look empty.
- **A 2xx is not proof.** The agent treats any 2xx as success, so a handler that answers `200` for a failed
  transfer reports a success that did not happen. For uploads, check the stored file, not the status.

Read a task's outcome through `GET /api/v1/tasks/tasks/<taskID>` — it returns `status`, `result` and
`error`. When you need the raw tables (or the endpoint itself misbehaves), query Postgres directly:

```bash
docker compose -f docker-compose.yml exec -T postgres \
  psql -U postgres -d postgres -t -A -c \
  "select t.\"Status\", r.\"Result\", e.\"Message\" from \"Tasks\" t
     left join \"TaskResult\" r on r.\"TaskID\" = t.\"ID\"
     left join \"Errors\" e on e.\"TaskID\" = t.\"ID\"
   where t.\"ID\" = <taskID>;"
```

## The entries

### The agent checks in with an empty username

`GetUserNameW` needs the two-call pattern: call it once with a `0` size to learn the length, allocate, call
again. A single fixed-size call returns a truncated or empty name. The check-in body shows `"user": ""` and the
agent still looks healthy.

### Upload/download "succeed" but nothing moved

The HTTP handler answered a 200 ACK page for a failed transfer, and the agent only checks the status code.
Confirm the artifact: a received file appears under `/app/storage/files/<campaign>/<name>` in the teamserver
container; a downloaded file appears on the guest. If the status is success and the file is absent, the handler
is lying — fix the handler, not the agent.

### `401` on every operator call

The bearer token is missing, expired, or not attached. Every operator controller is `[Authorize]`; only
`/api/v1/auth/login` and `/api/v1/server/status` are anonymous. In the Client, `AuthorizationMessageHandler`
attaches the token — a token that expired mid-session produces 401s until re-login. `Jwt:ExpiryMinutes`
defaults to 480.

### `/api/v1/tasks` answers `400 Bad Request` with a bare problem document

The server log names the cause: `Unable to resolve service for type 'ITaskService' while attempting to
activate 'TaskController'`. A controller that injects an interface needs that interface registered —
`builder.Services.AddScoped<ITaskService, TaskService>()` — and the service must declare it. Registering only
the concrete `TaskService` leaves controller activation failing for every route on that controller, and the
exception handler turns it into a `400`. This was true of all `/api/v1/tasks` routes until 2026-11-05; the
harness reads Postgres directly, so nothing caught it.

### The `migrator` exits on startup

The EF model and `ContextModelSnapshot` disagree (a pending model change). The migrator bundle fails fast
rather than half-migrating. Regenerate the migration inside the container image that has the tool, or
hand-write it and update **all three** places (migration `Up`, migration `Down`, `ContextModelSnapshot`).

### Payload generation fails after editing `Agent/`

There is no image rebuild to do — the server compiles the read-only bind mount live. The usual causes are a
save that raced the compiler (do not edit during generation), or a new build dependency that is not installed
in the `runtime-base` stage of `TeamServer/Dockerfile`. Read the captured CMake output; "build failed" alone
is not a diagnosis.

### A task never runs

The task exists and the agent checks in, but the task stays `queued`/`running`. Look at `Errors.Message`: the
dispatcher rejects an incomplete command and names the missing wire key
(`Task 136 (UPLOAD): missing required command field 'TransferID'`). Never weaken the validator to "fix" this —
the writer dropped a key.

If there is **no** `Errors` row and no `TaskResult`, check the TeamServer log for `23505` or `duplicate key`
before anything else:

```bash
docker compose -f docker-compose.yml logs teamserver | grep -E 'duplicate key|23505'
```

A constraint violation happens _inside_ the check-in, so the failure never reaches the task model: the task
looks merely queued, and it re-queues on every check-in while later tasks starve behind it. The one this project
hit was `IX_TaskResult_TaskID` — a chunked download reports once per chunk against the same task, and every
report inserted a new result row (`.pi/notes/defect-history.md`).

### `taskkill` over SSH does nothing

`taskkill` launched through an SSH session silently fails to terminate the target. Use
`Stop-Process -Force`, which the harness and the guest procedures use.

### Writes to `C:\Users\Public` are denied

`Public`'s DACL grants nothing to `Users`/`Everyone` — only `INTERACTIVE`, `SERVICE`, `BATCH`, `SYSTEM` and
`Administrators`. An SSH session is a **network logon**, so a standard-user agent's token matches none of those.
Even `icacls C:\Users\Public` returns "Access is denied", which looks like corruption. Write markers under
`%USERPROFILE%` instead.

### Escalate returns `ShellExecuteW returned 5`

`5` is `SE_ERR_ACCESSDENIED`. UAC bypass needs an **administrator** account in an **interactive** session;
a standard user cannot start an auto-elevate binary at all, and session 0 cannot service the consent prompt.
Related preconditions: `ConsentPromptBehaviorAdmin=5` (prompt for consent) and Defender tamper protection —
Defender's behaviour monitor terminates the payload when it writes the ms-settings key, so escalate needs
real-time protection off, which tamper protection may forbid.

### Listener creation fails on a bound port

The bind port is still held by the previous listener. Free it (stop the process/container that owns it) or wait
for the old listener to be removed; the harness skips listener re-creation while the port is bound.

### `Start-Process` children die with the SSH session

OpenSSH runs each session in a job object, so children are killed when the session closes. Use a scheduled task,
or keep the session open (the harness's launcher sleeps), or launch under the supervisor for a traced run.

## See also

- [getting-started.md](getting-started.md) — the sequence these failures interrupt.
- [deployment.md](deployment.md) — what a rebuild touches and where state lives.
- `.pi/notes/defect-history.md` — the full evidence for each defect above.
