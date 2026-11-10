# Agent ⇄ TeamServer wire protocol

Read this before changing a checkin body, a task request, a task result, or a `TaskType`. The keys
themselves are frozen (see `AGENTS.md`); what follows is how they are used.

## Checkin protocol

The listener module forwards `POST /api/v1/agents/checkin` to `IAgentService.Checkin`. All bodies are
base64; the plaintext is a 36-character UUID followed by JSON.

| Direction                      | Plaintext                                                   | Notes                                                                     |
| ------------------------------ | ----------------------------------------------------------- | ------------------------------------------------------------------------- |
| Initial checkin (request)      | `<payload UUID>` + host-info JSON                           | The payload UUID identifies the payload row; the server creates the agent |
| Initial checkin (response)     | `<CheckIn UUID>`                                            | 36 chars, no prefix. The agent must store it and use it from then on      |
| Established checkin (request)  | `<CheckIn UUID>` + results array                            | Send `[]` when there is nothing to report                                 |
| Established checkin (response) | Task request JSON, or **empty body** when the queue is idle | Empty body means “no task” — not an error                                 |

Healthy protocol, from the TeamServer logs at `Debug` level:
`Checkin uuid=… payloadLen=… knownAgent=True`, then `Checkin handled: success=True length=108` (a task was
returned), then a later checkin whose body is the result array.

## Task request

`Trinity.Shared.DTOs.Task.TaskRequest`, PascalCase keys:

```json
{
  "TaskID": 7,
  "TaskType": 8,
  "TaskCode": 0,
  "Status": 4,
  "Command": { "Command": "whoami" }
}
```

`Command` is the serialized command DTO, so its keys vary per task type (`Commandlet` / `Arguements`,
`PID`, `RegKey`, `TargetBinary`, `Operation`, `Username` / `Password`, `Path`, `TransferID`). The agent
parses both the PascalCase schema and the legacy lower-case keys.

## Task result

Batched as an array; `TaskResp<TaskSuccessResponse>` on the server:

```json
[
  {
    "TaskID": 7,
    "TaskType": 8,
    "TaskCode": 0,
    "Status": 2,
    "Data": { "Output": "..." }
  }
]
```

`Status` uses the server enum: `ERROR = 1`, `SUCCESS = 2`. Omitting `TaskType`/`TaskCode` makes the server
throw “missing required properties”. Results for an unknown `TaskID` **abort the whole batch**, so always
echo back the id/type/code of the task.

**`Data` is polymorphic on the server — send the matching shape:**

| `Status`      | `Data` shape                                                         | Server handling                                                                |
| ------------- | -------------------------------------------------------------------- | ------------------------------------------------------------------------------ |
| `2` (SUCCESS) | `{"Output":"..."}` → `TaskSuccessResponse`                           | `SuccessStrategy` stores the text in `TaskResult`                              |
| `1` (ERROR)   | `{"ErrorCode":N,"Message":"..."}` → `Trinity.Shared.DTOs.Task.Error` | `ErrorStrategy` sets `Tasks.Status = error` and stores the message in `Errors` |

Sending the success shape with `Status=1` leaves `Message` null, so the failure is recorded with no reason.
`models::ResultRequest::ToJson()` picks the shape from `is_successful`.

**Never report success for something that did not happen.** Every command returns
`commands::CommandResult`; `Agent::Start` maps its `success` flag onto `Status`. The dispatcher rejects a
command missing a field its `TaskType` requires, naming the wire key (`task/task_validator.cpp`):

```text
Task 136 (UPLOAD): missing required command field 'TransferID'
```

Reading results: a successful task stores its output in `TaskResult`; a failed one sets
`Tasks.Status = 'error'` and puts the diagnostic in `Errors`. Query both before concluding a feature “does
nothing”. There is still no REST endpoint for results — `TaskResult` is DB-only.

## Command type constants

`TaskType` / `TaskCode` are `enum class` values in `include/agent/models/task_types.h` and must equal
`Trinity.Shared.Enums.Tasks.TaskType` / `TaskCode` on the server.

| TaskType                            | Value | Dispatched to                                                                     |
| ----------------------------------- | ----- | --------------------------------------------------------------------------------- |
| `POWERSHELL`                        | 3     | `RunPowershell` (`ShellExecuteA` with `powershell.exe` + `-Command` **separate**) |
| `PROCESS` (code `RUN_U` = 6)        | 5     | `RunUnderProcess` (parent-PID spoofing)                                           |
| `PROCESS` (code `KILL_PROCESS` = 7) | 5     | `KillProcess` (`OpenProcess`/`TerminateProcess`; needs `PID`)                     |
| `DOWNLOAD` / `UPLOAD`               | 6 / 7 | `HandleDownload` / `HandleUpload` (WinHTTP; need `Path` **and** `TransferID`)     |
| `SHELL`                             | 8     | `RunShell` (cmd.exe /c)                                                           |
| `ESCALATE`                          | 9     | `EscalatePrivilege` (FodHelper / cmstp; needs `RegKey` and `TargetBinary`)        |
| `NET_ASM`                           | 10    | `ExecuteNetAssembly` (CLR host: base64 `Assembly`, arguments in `Arguements`)     |
| `WIN_TASK`                          | 13    | `CreateWinTask` (self-drop + Task Scheduler 1.0 COM; needs `Path` and `TaskName`) |
| `TOKEN`                             | 15    | `TokenImpersonate` (make / steal / rev2self)                                      |
| `KEYLOGGER`                         | 16    | `Keylogger` (dispatched, but the server has no route to queue it yet)             |

`NET_ASM` carries `{ "Assembly": "<base64>", "Arguments": "..." }`. The server resolves and encodes the
image, so the agent only ever sees base64 — `AssemblyPath` never reaches the wire. The blob must be a .NET
Framework 4.x image: the agent hosts the v4.0.30319 CLR (`ICLRMetaHost` → `ICorRuntimeHost`), and a .NET 10
assembly does not load there. The agent validates `Assembly` and treats `Arguments` as optional; `Invoke_3`
does not capture the managed console, so a task's result says the entry point ran, not what it printed.

## File transfer

Upload/download tasks carry `Path` plus a `TransferID` filled from the agent's campaign; the agent posts to
`http://<hosts[0]>:<COMM_PORT>/api/files/...`. Server-side routing and the keying rules are in
`teamserver.md`; the WinHTTP half is in `.pi/notes/win32-traps.md`.
