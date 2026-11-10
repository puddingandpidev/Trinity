# Task-command shape hardening

The goal: **a task command with a missing required field must fail loudly, naming the field, and no writer
may be able to construct an incomplete command silently.** Three steps, in dependency order. All three are
committed; step 3 landed as the contained scope below.

Verification for each step: the harness (these notes) plus the TeamServer suite. Wire key
names are frozen — a typed refactor must keep them byte-identical, and the agent also accepts legacy
lower-case keys.

## Step 1 — strict agent parsing — done

When a task command is missing a field its command type requires, report `Status=ERROR` and name the
missing field (it used to say "Missing file path or campaign ID" _and_ report `SUCCESS`). Smallest change,
biggest diagnostic payoff.

Landed as: `Agent/src/task/task_validator.cpp` (`task::MissingCommandField`), `commands::CommandResult`
driving `Status`, and `Agent::Start` mapping success honestly. Host-tested via `--target check-units`.

## Step 2 — server contract test — done

Serialise a task with the real `TaskRequestFactory` and assert the required wire keys per `TaskType` are
present (spec §12.1.4.1 asks for this: "task deserialisation and construction").

Landed as `TeamServer.Tests/Contracts/TaskCommandContractTests.cs`. Extend it whenever a command DTO is
added or changed.

## Step 3 — typed command DTOs — done (contained scope)

Replace the anonymous `JsonDocument` / `JsonSerializer.Serialize(new { ... })` writers with typed payloads
so a dropped field is a compile or test error. This is the structural fix. Two scopes; pick one before
writing code.

**Contained (recommended):** replace the two anonymous objects in
`FileTransferService.PrepareDownloadChunkAsync` (the stored command _and_ the on-wire chunk document) with
typed records, and add them to `TaskCommandContractTests`. Leaves `Task.Command` as `JsonDocument`. Blast
radius: `FileTransferService`, the contract test.

Landed as `DownloadChunkState` (the stored command) and `DownloadChunkPayload` (the on-wire chunk) in
`Trinity.Shared/DTOs/Command/`, plus `DownloadChunkPayload_CarriesEveryKeyTheAgentNeeds` in the contract
tests. The full scope — typing `Task.Command` everywhere — remains open.

**Full:** type `TaskRequest.Command` / `Task.Command` everywhere. `Task.Command` is `jsonb` in Postgres and
flows through `AgentService`, `TaskRequestFactory`, `CommandService`, the controllers and the Client, so
this needs an EF mapping/migration and touches the Blazor side. Bigger, and the wire keys must stay
byte-identical.

**The discriminator question is settled, and it is not a discriminator.** The envelope already carries the tag,
outside the command object — `TaskRequestFactory` writes `{TaskID, TaskType(int), TaskCode(int), Command{...},
Status}`, and the agent picks its parser/validator from `TaskType`/`TaskCode`. So a full pass resolves the DTO
type from the enum the server already stores (`Task.CommandType`/`Code`, both columns) in a converter, and the
serialised bytes do not change: no `$type`, no `[JsonDerivedType]` write, nothing new inside `Command`. If a
discriminator property is ever wanted anyway, the only permissible name is the existing `commandType`
(`CheckinResponse` already uses it) — never `$type`. Background and the C++ side of the argument:
these notes item 3 and `.pi/notes/to-owner/06-typing-task-command.md`.

Places that can silently reshape a command today:

| Location                                              | What it does                                       |
| ----------------------------------------------------- | -------------------------------------------------- |
| `TeamServer/Services/Factories/TaskRequestFactory.cs` | Wraps the stored command into a `TaskRequest`      |
| `CommandService.QueueTaskAsync`                       | Fills `TransferID` from the agent's campaign       |
| `AgentService.ProcessFullyEstablishedCheckin`         | Passes the download override                       |
| `TeamServer/Validators/Commands/`                     | Per-command DTO validation                         |
| `FileTransferService.PrepareDownloadChunkAsync`       | Produces the stored command and the chunk document |

## Constraints

- In payloads, handle both Windows and POSIX separators: the server runs on Linux, so `Path.GetFileName`
  does not split on a backslash (see `honest-results.md`, download preparation).
- Verify with the harness after each step; it asserts on artifacts, not status strings.
- Escalate needs real-time protection off; the harness toggles it only when tamper protection allows and
  never tries to defeat AV.
- Do not commit `AGENTS.md` (local-only), and follow the box-level git workflow before pushing.

## Acceptance

The suite stays green, a task command with a missing required field fails loudly with the field named, and
no writer can construct an incomplete command without a compile or test error.
