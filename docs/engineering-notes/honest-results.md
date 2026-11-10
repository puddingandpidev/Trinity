# Defect history — what the guest harness caught

> Every entry was observed against a live stack and a real Windows guest, not inferred from reading code. Names of people and machines have been removed.

Every entry was observed on the Windows 11 guest against the live Docker stack, not inferred from reading
code. The pattern is the point: each defect **reported success while doing nothing**, so a test that
only checks status strings stays green. When you fix one of these, add the mechanism so the next session
recognises the shape.

Context: `scripts/e2e-agent-test.sh` was being built; the three features below were the reason it now
asserts on artifacts.

## The pattern, in one table

| Feature               | Reported               | Actual                                                           |
| --------------------- | ---------------------- | ---------------------------------------------------------------- |
| Escalate (UAC bypass) | "Escalation completed" | Registry key deleted before FodHelper read it; command never ran |
| File upload           | "Upload successful"    | Listener answered with an ACK page; nothing stored               |
| File download         | "Download successful"  | Same ACK page was saved as the file content                      |

Also fixed in the same pass: `username` was **always empty** in every check-in.

## Agent defects

### `GetUserNameW` two-call pattern inverted (`Agent/src/host/host_info.cpp`)

The first call is _supposed_ to fail with `ERROR_INSUFFICIENT_BUFFER` and report the required size. The
code treated that expected failure as fatal, so it returned an empty string. **Effect:** every agent row
had `Username = ""`. **Verified:** check-in now reports `username = the test user`.

### File upload: use-after-free + wrong header length (`Agent/src/commands/exec/filetransfer.cpp`)

- `LPCWSTR headers = ToWide(...).c_str();` stored a pointer into a temporary `std::wstring` that dies at the
  end of the statement — freed before `WinHttpSendRequest` read it. Clang flags this as `-Wdangling-gsl`.
- `dwHeadersLength` was `wcslen(...) * 2`. The API documents **characters**, not bytes, so the request
  claimed a header buffer twice its real length.

### File upload used the wrong agent key (`Agent/src/commands/exec/filetransfer.cpp`)

The agent posted to `/api/files/{payloadUUID}/{name}/file`, but the server resolves that key with
`GetAgentByCheckInUUID` — it expects the **session UUID issued at check-in**, not the payload UUID baked
into the binary. Every lookup threw `KeyNotFoundException`, `HttpModule` swallowed it, and the agent saw
HTTP 200 and reported success.

**Fix:** `CommManager` records the session UUID (`agent::transport::CurrentSessionUuid()`, set on both the
initial and established check-in); the upload uses it, falling back to the payload UUID only before the
first check-in completes. **Also:** `HttpModule` now returns 500 when a file handler throws and 404 when a
download target is missing, instead of a 200 the agent reads as success.

### `WinHttpQueryStatusCode` does not exist (`Agent/src/commands/exec/filetransfer.cpp`)

The upload path resolved a function by that name and guarded on the result. `winhttp.dll` has no such
export — the status code comes from `WinHttpQueryHeaders` with
`WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER` — so the hash lookup returned `nullptr`, the guard
tripped, and `PostFile` returned false **before sending a byte**.

**Why it was hard to see:** the task reported `success`, the result said `Upload failed`, and the server
had no record of a request. Server-side logs cannot show an absent request; the tell was that a direct
`curl` from the guest worked while the agent's own call left no trace.

### Privilege escalation: two independent defects (`Agent/src/commands/exec/privesc.cpp`)

1. **Stray `@` prefix.** The registry value was written as `@cmd.exe /c <cmd>`, so ShellExecute looked for a
   binary literally named `@cmd.exe`. A/B proven: `@cmd.exe` → no marker file; `cmd.exe` → marker file.
2. **Cleanup race.** The comment said "Cleanup after delay" but the key was deleted immediately after
   `CreateProcessW`. FodHelper reads the key _asynchronously_, so the agent raced itself and usually won.
   It now waits 5 s (`kCleanupDelayMs`).

**Verified:** the elevated command executes and writes a marker. It lands at **Medium** integrity because
the test runs in session 0 — see these notes and _Escalation evidence_ below.

### PowerShell tasks never launched anything (`Agent/src/commands/exec/shell.cpp`)

`RunPowershell` passed the whole command line as ShellExecute's `lpFile`:
`ShellExecuteA(NULL, "open", "powershell -Command \"Get-Date\"", NULL, NULL, SW_HIDE)`. `lpFile` is a
_file_, not a command line, so the shell looked for a binary literally named
`powershell -Command "Get-Date"` and returned **SE_ERR_FILE_NOT_FOUND (2)**. The result was never checked,
so the task reported `Command executed` while nothing ran.

Found by the strict result reporting below (`Status=ERROR` exposed the return value), then confirmed with
an A/B probe:

| Call                                                              | `ShellExecuteA` returns       |
| ----------------------------------------------------------------- | ----------------------------- |
| `lpFile="powershell -Command \"Get-Date\""`, `lpParameters=NULL`  | **2** (SE_ERR_FILE_NOT_FOUND) |
| `lpFile="powershell.exe"`, `lpParameters="-Command \"Get-Date\""` | **42** (success)              |

**Fix:** pass executable and arguments separately, and treat a return value ≤ 32 as failure (the error
message carries the code). **Verified:** the `powershell` test writes a marker file from PowerShell and
asserts its content.

**Lesson:** the harness passed this test for weeks because its expectation string was empty — it asserted
`status == success` only. Tests for commands whose output the agent does not capture must assert an
artifact.

### A failed task's diagnostic moved from `TaskResult` to `Errors`

The agent hardcoded `Status=SUCCESS` for every dispatched command, so a command that could not run still
stored its complaint as the task's _result_. `Agent/src/agent.cpp` now maps the dispatcher's
`CommandResult.success` onto the status, and `Agent/src/task/task_validator.cpp` rejects a command missing
a field its `TaskType` requires, naming the wire key:

```text
Task 136 (UPLOAD): missing required command field 'TransferID'
```

**Harness implications:** `Tasks.Status` becomes `error` and the message lives in `Errors.Message`
(`TaskResp<TaskResp>.Data` deserialises into `Trinity.Shared.DTOs.Task.Error { ErrorCode, Message }` for
`Status=ERROR`, _not_ `TaskSuccessResponse`). `wait_task` still returns only `TaskResult` text, so
anything matching on a diagnostic string must fall back to `task_error <id>`.

## Server defects

### File upload/download never reached the server (`Trinity.Shared/Modules/HttpModule.cs`)

`HttpModule` routed only `POST /api/v1/agents/checkin`. Every other path fell through to
`<HTML><BODY> Recieved </BODY></HTML>` with status 200. Since the agent's transfer code treats any 2xx as
success, uploads reported success and downloads saved the HTML page.

**Fix:** route `/api/files/{agentKey}/{fileName}/file` (POST) and `/api/files/{campaignId}/{fileName}`
(GET) to handlers supplied by `HttpModuleFactory`, mirroring the checkin-handler pattern.

### Download preparation keyed on the full guest path, and aborted the check-in

`FileTransferService.PrepareDownloadChunkAsync` used the task's `Path` (`C:\Users\the test user\file.txt`) as
the storage file name, but storage is keyed by the base name, so every lookup threw
`FileNotFoundException`. Worse, it is called from `AgentService.ProcessFullyEstablishedCheckin`, so the
exception aborted the entire check-in response — the task stayed `queued` forever and the agent never
learned it had work.

**Fix, three parts:**

1. Keep the full path in the task command but look up storage by the base name.
2. **Do not use `Path.GetFileName` for this.** The server runs on **Linux**, where a backslash is not a
   separator, so it returns the whole string. `ExtractFileName` splits on `\` and `/` explicitly. This
   class of bug only appears with a Windows-agent/Linux-server pair and looks like "the file is missing"
   from the server's side.
3. Wrap chunk preparation in try/catch that falls back to a plain task, because an exception here aborts
   the whole check-in.

**Also:** the chunk rewrite _replaced_ the whole command with an anonymous JSON object, silently dropping
`TransferID` (the agent uses it as the campaign id in the download URL). The stored command and the
returned chunk document had to be fixed separately — two writers for one contract, neither type-checked.
Anything that rewrites a command should merge into a typed DTO rather than serialise a fresh anonymous
object; `TeamServer.Tests/Contracts/TaskCommandContractTests.cs` now pins the keys.

**Also:** `AdvanceDownloadAsync` re-queued any download result whose command lacked `Final`, so a task
delivered via the fallback path was re-queued forever after the agent had already succeeded. A command
with no `Final` is now terminal.

## Payload/build hardening (separate work, already committed)

- New `mingw-clang-x86_64` preset + `Agent/cmake/toolchains/mingw-w64-clang.cmake` (Clang + LLD +
  whole-program LTO). Verified: check-in, CIG status and shell tasks match the GCC build.
- Both toolchains link `-s`: drops the symbol table and ~350 KB of DWARF (2.7 MB → 1.2 MB).
- The `-D`-injected config strings (`COMM_HOSTS_LIST`, `COMM_USER_AGENT`, `COMM_PAYLOAD_UUID`) were
  **plaintext in `.rdata`** — `xorstr.py` cannot see them because they never appear as literals in source.
  Now wrapped in `xorstr_()`.
- TeamServer: toolchain selectable via `PayloadBuilder:Toolchains:<arch>` (GCC remains default).
- Optional `-DAGENT_PASS_PLUGIN=` hook for LLVM IR obfuscation (off by default).
- **Not vendored:** Morok (MIT, https://github.com/19h/morok) needs LLVM 23; the newest obtainable here is
  22 and porting it touches ~60 headers. Documented in `Agent/doc/DEV.md`.

## Escalation evidence — resolved 2026-11-04

Two defects hid behind one symptom ("escalate reports success but nothing elevates"):

1. **Environmental (2026-11-03):** from session 0 there is no AppInfo desktop, so an autoElevate binary
   simply runs un-elevated. The plumbing (registry values written, FodHelper reads them, command runs)
   was provable there; the transition never was.
2. **Code (2026-11-04):** `privesc.cpp` launched FodHelper/`cmstp` with **`CreateProcessW`**, which cannot
   elevate at all. From a filtered admin token it fails with `ERROR_ELEVATION_REQUIRED` (740) — and the
   agent reported that as an opaque "could not start elevated process" with the code discarded, so from
   session 0 nobody could tell the launch had never been capable of elevation. The reference technique
   hands the binary to the **shell**, whose AppInfo service performs the silent auto-elevation.

Measured from an interactive console session (Medium integrity, filtered admin token), same registry payload:

| Launch                          | Result                   | Marker                          |
| ------------------------------- | ------------------------ | ------------------------------- |
| `CreateProcessW(FodHelper.exe)` | `ok=False lastError=740` | absent                          |
| `ShellExecuteW(FodHelper.exe)`  | `ok=True`                | present, `High Mandatory Level` |

Fixed: `ShellExecuteW` (verb `open`) with the `<= 32` failure check from
`.pi/notes/win32-traps.md`; failures now name the returned code. Verified end to end on the guest:
task success, marker at High integrity, and `consent.exe` never appeared (200 ms watcher, 180 s).
The harness now writes a marker from the elevated command and asserts its integrity level, so the task's
success string cannot produce a false green.

The harness now writes a marker from the elevated command and asserts its integrity level, so the task's
success string cannot produce a false green. A non-admin agent reports `ERROR_ELEVATION_REQUIRED`, which is
classified SKIP (the documented precondition) rather than failure — the skip match reads `Errors.Message`,
because a failed task no longer stores its diagnostic in `TaskResult`.
