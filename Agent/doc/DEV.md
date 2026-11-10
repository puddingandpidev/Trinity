# Agent Development Guide

This document covers the day-to-day development workflow for the Trinity Agent (C++20,
cross-compiled with MinGW-w64 for Windows).

## TL;DR

```bash
# Configure (first time / after cleaning)
cmake -S Agent -B Agent/out/build/mingw-x86_64 --preset mingw-x86_64

# Build → Agent/out/build/mingw-x86_64/Agent.exe
cmake --build Agent/out/build/mingw-x86_64 --config Release

# Alternative front-end/linker pair (Clang + LLD) → a structurally different PE
cmake -S Agent -B Agent/out/build/mingw-clang-x86_64 --preset mingw-clang-x86_64
cmake --build Agent/out/build/mingw-clang-x86_64 --config Release

# Quality checks
cmake --build Agent/out/build/mingw-x86_64 --target format   # clang-format
cmake --build Agent/out/build/mingw-x86_64 --target tidy     # clang-tidy
cmake --build Agent/out/build/mingw-x86_64 --target check-units  # host unit checks (tasks/, validator)
python3 Agent/xorstr.py                                       # string encryption
```

Everything else in this doc is detail for when you need it.

## General Guidelines

### Formatting (clang-format)

All C/C++ sources are formatted with `clang-format` using the repository-level `.clang-format`
config (Google base, 4-space indent, 120-column limit, no include sorting).

- Run the build target to format everything in place:
  ```bash
  cmake --build Agent/out/build/mingw-x86_64 --target format
  ```
- Or format a single file:
  ```bash
  clang-format -i path/to/file.cpp
  ```
- **Never** manually reorder `#include` blocks — Windows headers (winsock2, windows, iphlpapi,
  ...) have a required dependency order that clang-format's include sorting would break
  (`SortIncludes: false` is set for this reason).

### Static Analysis (clang-tidy)

The check set is defined in `.clang-tidy` (bugprone, clang-analyzer, cert, cppcore, misc,
performance, portability, readability, modernize, google, hicpp, llvm).

- Run the build target:
  ```bash
  cmake --build Agent/out/build/mingw-x86_64 --target tidy
  ```
- tidy reads the compile database and fails with *"No compilation database found"* when the build
directory was never configured. `CMAKE_EXPORT_COMPILE_COMMANDS` is ON in `CMakeLists.txt`, so a plain
configure is enough; after `rm -rf`ing the build directory, configure again before running tidy.
- The goal is zero diagnostics, and new code should not add new ones. Windows API code
  occasionally trips checks that can't be fixed; suppress those with
  `// NOLINTNEXTLINE(check-name) // reason` and a brief rationale. Two examples worth knowing:
  `bugprone-multi-level-implicit-pointer-conversion` (passing `&handle` as `void*` to
  `UpdateProcThreadAttribute`) and `readability-function-cognitive-complexity` (split the function
  instead of adding `NOLINT` where practical — `task_validator.cpp` does this).
- Vendored third-party code (`external/rapidjson/`, `include/base64.hpp`) is excluded via
  `ExcludeHeaderFilterRegex` and a `SYSTEM` include directory.

### Host Unit Checks (`--target check-units`)

Pure-logic code that does not touch the Windows API can be compiled with the **build host's** compiler
and run immediately, instead of shipping a payload to the guest:

```bash
cmake --build Agent/out/build/mingw-x86_64 --target check-units
# → compiles tests/*.cpp with g++/clang++ and runs the binary
```

Add a test file under `Agent/tests/` and, if it needs a translation unit, add that unit to the
`check-units` command in `CMakeLists.txt`. `tests/task_validator_test.cpp` is the model: no framework,
a tiny `ExpectEqual` helper, a non-zero exit on failure. Keep the code under test platform-neutral — that
is why command validation lives in `task/task_validator.cpp` rather than in the dispatcher.

Two host test binaries are built: `task_validator_test` (required wire keys per `TaskType`) and
`task_parsing_test` (task deserialisation and result serialisation). Both are registered with CTest, so
after `check-units` has built them, run `ctest --test-dir <build-dir>` — ctest does not build, so run
`check-units` first.

### String Encryption (xorstr_)

All string literals longer than 6 characters that appear in the compiled binary must be
encrypted at compile time using `xorstr_()` from `external/xorstr/xorstr.hpp` (Justas
Masiulis, Apache-2.0, https://github.com/JustasMasiulis/xorstr).

- **Why:** The Agent is a red-team payload. Plaintext strings (API names, URLs, commands,
  registry paths) are trivially greppable in the binary. `xorstr_()` encrypts the string
  in-place at compile time and decrypts it at runtime, leaving no plaintext in the binary.
- **Dangling pointer warning:** `xorstr_()` is a macro expanding to
  `xorstr(str).crypt_get()` — it returns a `char*`/`wchar_t*` pointing into a temporary
  `xor_string` object that is destroyed at the end of the current full-expression
  (statement). It is safe to pass the result directly into a function call (e.g.
  `HASH_STRING_A(xorstr_("CreateFileA"))`), but **never** store the pointer in a variable
  for later use. If you need to keep the string, copy it into a `std::string` immediately:
  ```cpp
  // Good — consumed within the same full-expression
  uint32_t hash = HASH_STRING_A(xorstr_("CreateFileA"));

  // Good — copied into std::string before the temporary is destroyed
  std::string name{xorstr_("CreateFileA")};

  // BAD — dangling pointer; the temporary is destroyed at the end of this statement
  const char* p = xorstr_("CreateFileA");
  // ... use p later ...
  ```
- The CI check script `Agent/xorstr.py` enforces this: it walks `src/` and flags any string
  literal > 7 characters that is not wrapped in `xorstr_()`. Run it before committing:
  ```bash
  python3 Agent/xorstr.py
  ```
- Short strings (≤ 7 chars), format specifiers, and strings inside `#include` directives are
  exempt.

### Modern C++ Conventions

- **C++20** — the minimum standard. Use `std::string_view` for non-owning string parameters,
  `constexpr` for compile-time constants, `[[nodiscard]]` on functions whose return value
  matters.
- **No raw memory** — no `new`/`delete`, no raw owning pointers. Use `std::unique_ptr`,
  `std::vector`, `std::string`, or stack allocation.
- **No `NULL`** — use `nullptr`.
- **No `#define` for constants** — use `constexpr` or `const inline`.
- **No by-value `std::string` parameters** — pass as `const std::string&` or
  `std::string_view`.
- **No `goto`** for flow control.
- **No `#pragma comment`** — link libraries via CMake.

### Naming Conventions

- **C++ identifiers:** `snake_case` for variables, functions, and file names.
  `PascalCase` for types (classes, structs, enums).
- **C identifiers** (in vendored `.c` files): follow the original library's convention
  (typically `CamelCase`).
- **Constants:** `kPascalCase` (e.g. `kModule_kernel32`, `kFn_CreateFileA`).
- **Namespaces:** `snake_case` (e.g. `agent::transport`, `agent::util`).

### File Header Convention

New `.h`/`.cpp`/`.hpp`/`.s` files should begin and end with a banner comment (convention,
not enforced):

```cpp
// ========================================== { START OF FILE } ========================================== //
// ... file content ...
// ========================================== { END OF FILE } ========================================== //
```

For assembly files the delimiters use `/* */` style.

### Platform Boundary

- **Headers are the contract** — they must be platform-neutral. No Windows types
  (`HMODULE`, `SOCKET`, etc.) in public header signatures unless guarded by `#ifdef _WIN32`.
- **`.cpp` files are the platform boundary** — all Windows API calls live in the `.cpp`
  behind `#ifdef _WIN32` guards. There is no separate `Platform.h` abstraction layer.
- The current target is Windows only. Portable `#else` fallbacks exist but are minimal.

### JSON Wire Keys Are Fixed

The JSON keys on the wire (e.g. `"internalIP"`, `"externalIP"`, `"OS"`, `"commandType"`,
`"TaskID"`, `"Commandlet"`, `"Arguements"`) map to the server-side C# schema and **must never
be changed**. C++ identifiers use `snake_case`; wire keys keep their original casing.

**rapidjson gotcha:** `rapidjson::StringRef` is non-owning (`kConstStringFlag`). Passing
`StringRef(xorstr_("Key"))` makes the document reference the xorstr temporary, which is
re-encrypted when the full-expression ends — the serialised JSON then contains ciphertext.
Copy keys into a `std::string` that outlives the writer (see
`models::ResultRequest::ToJson()`):

```cpp
const std::string k_task_id{xorstr_("TaskID")};
doc.AddMember(rapidjson::StringRef(k_task_id.c_str()), task_id, alloc);
```

## Checkin Protocol (agent ⇄ TeamServer)

The listener module forwards `POST /api/v1/agents/checkin` to `IAgentService.Checkin`. All
bodies are base64; the plaintext is a 36-character UUID followed by JSON.

| Direction | Plaintext | Notes |
|---|---|---|
| Initial checkin (request) | `<payload UUID>` + host-info JSON | The payload UUID identifies the payload row; the server creates the agent. |
| Initial checkin (response) | `<CheckIn UUID>` | 36 chars, no prefix. The agent must store it and use it from then on. |
| Established checkin (request) | `<CheckIn UUID>` + results array | Send `[]` when there is nothing to report. |
| Established checkin (response) | Task request JSON, or **empty body** when the queue is idle | Empty body means “no task” — do not confuse it with an error. |

Task request (`Trinity.Shared.DTOs.Task.TaskRequest`, PascalCase keys):

```json
{"TaskID":7,"TaskType":8,"TaskCode":0,"Status":4,
 "Command":{"Command":"whoami"}}
```

`Command` is the serialized command DTO, so its keys vary per command type (`Commandlet` /
`Arguements`, `PID`, `RegKey`, `TargetBinary`, `Operation`, `Username` / `Password`, `Path`,
`TransferID`). The agent parses both the PascalCase schema and legacy lower-case keys.

Task result (batched as an array, `TaskResp<TaskSuccessResponse>` on the server):

```json
[{"TaskID":7,"TaskType":8,"TaskCode":0,"Status":2,"Data":{"Output":"..."}}]
```

`Status` uses the server enum: `ERROR = 1`, `SUCCESS = 2`. Omitting `TaskType`/`TaskCode`
makes the server throw “missing required properties”. Results for an unknown `TaskID` abort
the whole batch, so always echo back the id/type/code of the task.

**`Data` is polymorphic on the server — send the matching shape:**

| `Status` | `Data` shape | Server handling |
|---|---|---|
| `2` (SUCCESS) | `{"Output":"..."}` → `TaskSuccessResponse` | `SuccessStrategy` stores the text in `TaskResult` |
| `1` (ERROR) | `{"ErrorCode":N,"Message":"..."}` → `Trinity.Shared.DTOs.Task.Error` | `ErrorStrategy` sets `Tasks.Status = error` and stores the message in `Errors` |

Sending the success shape with `Status=1` leaves `Message` null, so the failure is recorded with no
reason. `models::ResultRequest::ToJson()` picks the shape from `is_successful`.

**Never report success for something that did not happen.** Every command returns
`commands::CommandResult`; `Agent::Start` maps its `success` flag onto `Status`. The dispatcher
rejects a command that is missing a field its `TaskType` requires, naming the wire key (see
`task/task_validator.cpp`):

```
Task 136 (UPLOAD): missing required command field 'TransferID'
```

### Command type constants

`TaskType` / `TaskCode` are `enum class` values in `include/agent/models/task_types.h` and must equal
`Trinity.Shared.Enums.Tasks.TaskType` / `TaskCode` on the server.

| TaskType | Value | Dispatched to |
|---|---|---|
| `POWERSHELL` | 3 | `RunPowershell` (`ShellExecuteA` with `powershell.exe` + `-Command` **separate**) |
| `PROCESS` (code `RUN_U` = 6) | 5 | `RunUnderProcess` (parent-PID spoofing) |
| `PROCESS` (code `KILL_PROCESS` = 7) | 5 | `KillProcess` (`OpenProcess`/`TerminateProcess`; needs `PID`) |
| `DOWNLOAD` / `UPLOAD` | 6 / 7 | `HandleDownload` / `HandleUpload` (WinHTTP; need `Path` **and** `TransferID`) |
| `SHELL` | 8 | `RunShell` (cmd.exe /c) |
| `ESCALATE` | 9 | `EscalatePrivilege` (FodHelper / cmstp; needs `RegKey` and `TargetBinary`) |
| `TOKEN` | 15 | `TokenImpersonate` (make / steal / rev2self) |
| `KEYLOGGER` | 16 | `Keylogger` (dispatched, but the server has no route to queue it yet) |

## Win32 & WinHTTP traps

These each cost a debugging session because the failure was silent.

- **`ShellExecuteA` takes a file and its arguments separately.** `lpFile` is a *file*, not a command
  line: passing `"powershell -Command \"Get-Date\""` makes the shell look for a binary literally
  named `powershell -Command "Get-Date"` and return `SE_ERR_FILE_NOT_FOUND` (2). Pass
  `lpFile="powershell.exe"`, `lpParameters="-Command \"...\""`. **Always check the return value**:
  anything **<= 32** is an error (0 OOM, 2 file not found, 3 path not found, 5 access denied, 31 no
  association, 32 DLL not found); > 32 means launched. Verified on the guest: whole line → 2, split →
  42.
- **`WinHttpQueryStatusCode` does not exist.** The status code comes from `WinHttpQueryHeaders` with
  `WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER`. A hash lookup for the nonexistent export
  returns null and the guard trips *before sending anything*.
- **`WinHttpCrackUrl` only fills the components whose `dw*Length` member is set to `(DWORD)-1`**
  on input. Always set `dwHostNameLength` — otherwise `lpszHostName` is `NULL` and
  `WinHttpConnect` fails silently (no request ever leaves the process).
- The returned `lpszHostName` / `lpszUrlPath` are substrings of the URL and are **not**
  null-terminated. Copy them with their reported length before passing them on.
- **`WinHttpSendRequest` takes the header length in characters, not bytes** — `wcslen(...) * 2` makes
  the request claim a buffer twice its real size.
- Never store the result of `ToWide(...).c_str()`: the temporary `std::wstring` dies at the end of the
  statement and the API reads freed memory (clang's `-Wdangling-gsl` catches this).
- Modules such as `winhttp.dll`, `iphlpapi.dll` and `shell32.dll` are not always mapped.
  `agent::util::get_module` walks the PEB and falls back to `LoadLibraryA` (resolved by
  hash) — never call `GetModuleHandleH` directly.

### Vendored Code

Third-party code lives in `Agent/external/` and is **not modified**:
- `external/rapidjson/` — JSON parsing (SYSTEM include, excluded from tidy)
- `external/base64.hpp` — base64 encode/decode (excluded from tidy)
- `external/api_hashing/` — Maldev Academy ApiHashing library (MIT)
- `external/Windows.h` — case-sensitivity shim for Linux builds
- `external/xorstr/` — Justas Masiulis xorstr (Apache-2.0, https://github.com/JustasMasiulis/xorstr), SIMD-based string encryption

If you need to patch vendored code, fork it into `src/` and apply the patch there.

---

## How Do I …

### … build the Agent?

```bash
# Configure (from repo root)
cmake -S Agent -B Agent/out/build/mingw-x86_64 --preset mingw-x86_64

# Build
cmake --build Agent/out/build/mingw-x86_64 --config Release

# Output: Agent/out/build/mingw-x86_64/Agent.exe
```

#### Toolchains

Two toolchains produce the same behaviour from the same sources:

| Preset | Toolchain file | Compiler + linker | Notes |
|---|---|---|---|
| `mingw-clang-x86_64` | `cmake/toolchains/mingw-w64-clang.cmake` | Clang + LLD (whole-program LTO) | **Default**; what the TeamServer's payload builder uses. Emits PDBs for debug builds. |
| `mingw-x86_64` | `cmake/toolchains/mingw-w64-x86_64.cmake` | GCC + GNU ld | Alternative: different front-end codegen, linker and PE layout for identical behaviour. |

Both link `-static`. Release/MinSizeRel additionally strip (`-s`) the symbol table and the debug
sections the mingw CRT startup objects would otherwise add; the debug presets below keep them.

**Server-side selection.** `CMakePayloadBuilder` resolves the toolchain from
`PayloadBuilder:Toolchains:<arch>` (default `mingw-w64-clang.cmake`, also set in
`TeamServer/appsettings.json`). Point it at `mingw-w64-x86_64.cmake` to build payloads with GCC +
GNU ld instead. The runtime image installs `clang`, `lld` and the mingw toolchain
(`TeamServer/Dockerfile`, `runtime-base` stage) — **any new Agent build dependency must be added
there too**, or containerised payload builds fail.

**Verification.** The Clang/LLD build was validated on the Win11 test VM: check-in,
CIG status (`0x0001:CODEINTEGRITY_OPTION_ENABLED`), and `shell` task execution all match
the GCC build.

#### Debug builds (symbols for a live guest)

Release strips symbols and, with Clang, applies whole-program LTO — there is nothing left to debug
with. Two extra presets produce symbol-bearing binaries:

| Preset | Debug info | Debugger that reads it |
|---|---|---|
| `mingw-x86_64-debug` | DWARF (`-g -O0`, frame pointers kept) | gdb on the guest |
| `mingw-clang-x86_64-debug` | CodeView + `Agent.pdb` (`-gcodeview -Wl,--pdb=Agent.pdb`) | `cdb`/WinDbg; the PDB also symbolises crash dumps |

```bash
cmake -S Agent -B Agent/out/build/mingw-clang-x86_64-debug --preset mingw-clang-x86_64-debug
cmake --build Agent/out/build/mingw-clang-x86_64-debug
# → Agent/out/build/mingw-clang-x86_64-debug/Agent.exe (+ Agent.pdb next to it)
```

- **Debug builds are for debugging only.** `CMakePayloadBuilder` always configures `Release`, so
  payloads built by the TeamServer — and everything the harness runs — are unchanged.
- **Ship the `.pdb` with the `.exe`, from the same build.** A stale PDB points at the wrong code
  and silently produces nonsense stacks; copy both to the guest together.
- `RelWithDebInfo` works too (`-g -O1`, no LTO, symbols kept) — use it when `-O0` changes the
  behaviour you are chasing.
- GCC can only emit DWARF, which is why `cdb` is an option for the Clang build and not the GCC one.
  Clang emits CodeView/PDB instead; both `-g` and `-gcodeview` are set in the debug presets.

The debug presets also enable the two post-mortem aids, so a failing run explains itself without a
debugger (see `.pi/notes/live-debugging.md`):

| Option | What it builds |
|---|---|
| `-DAGENT_TRACE=ON` | A structured trace in the payload: one flushed line per loop step in `%TEMP%\agent-<pid>.log`, with `GetLastError` and WinHTTP status on failure |
| `-DAGENT_SUPERVISOR=ON` | `AgentSupervisor.exe`, which launches a payload, waits for it and logs the exit code (`0xC0000005` crash vs `0xC000013A` console close vs `1` ambiguous) |

Both are off by default, so a `Release` payload built by the TeamServer carries neither the code nor
the strings. The supervisor is a separate executable on purpose: `CMakePayloadBuilder` copies
`Agent.exe` by name, so a helper target can never be shipped as the payload.

#### Obfuscation (`AGENT_PASS_PLUGIN`)

The Clang toolchain accepts an optional LLVM New-PM **pass plugin** for IR-level obfuscation:

```bash
cmake -S Agent -B Agent/out/build/mingw-clang-x86_64 --preset mingw-clang-x86_64 \
      -DAGENT_PASS_PLUGIN=/path/to/libObfuscator.so
```

The flag is off by default and rejected for non-Clang toolchains, so existing GCC builds are
unaffected. The plugin's ABI must match the LLVM version of the `clang` that loads it — the
plugin and the compiler must agree on `llvm/Plugins/PassPlugin.h` (API v2/v3).

Actively maintained candidates (both require a newer LLVM than Debian trixie's `llvm-22`):

- **[Morok](https://github.com/19h/morok)** — MIT, modular C++23 LLVM New-PM IR obfuscator
  (control-flow flattening, MBA/substitution, string/constant encoding, vtable and VM
  bytecode rewriting, optional runtime self-protection). HEAD tracks **LLVM 23**
  (`UncondBrInst`/`CondBrInst`, `RequiredPassInfoMixin`). It builds as a loadable
  `libMorok.so`, so once LLVM 23 is available it needs no fork of the compiler.
- **[IRvana](https://github.com/m3rcer/IRvana)** — MIT, OLLVM integration and LLVM IR
tooling focused on Windows.

> **Status on this machine:** `llvm-22` is the newest obtainable (Debian trixie and
> apt.llvm.org stable both stop at 22), and Morok HEAD requires LLVM 23 APIs. Porting it
> would mean patching ~60 pass headers plus the control-flow flattener — a fork-level change
> whose correctness could not be validated — so it is deliberately not vendored here. The
> `AGENT_PASS_PLUGIN` hook makes it a one-flag opt-in once LLVM 23 is installed.

### … run the quality checks?

```bash
cmake --build Agent/out/build/mingw-x86_64 --target format       # clang-format all sources
cmake --build Agent/out/build/mingw-x86_64 --target tidy         # clang-tidy all translation units
cmake --build Agent/out/build/mingw-x86_64 --target check-units  # host unit checks
python3 Agent/xorstr.py                                           # xorstr compliance
```

Aim for zero tidy diagnostics; suppress genuinely unfixable ones with `NOLINTNEXTLINE`. tidy needs a
configured build directory (it reads `compile_commands.json`), and `check-units` needs a host compiler
(`g++` or `clang++`) — both are found automatically.

### … use the language server (clangd)?

clangd gets its flags from `compile_commands.json`, which CMake generates in
`Agent/out/build/mingw-clang-x86_64/` on configure (`CMAKE_EXPORT_COMPILE_COMMANDS ON`).
`Agent/.clangd` points clangd at that directory — auto-detection can't find it because
the build dir is not an ancestor of `src/`. Clang/LLD is the default payload toolchain, so this
is the database clangd should read.

- If the LSP logs c++17 indexing or missing Windows headers, the compile database is
  missing: re-run the configure command (see "… build the Agent?").
- Verify a file parses cleanly with:
  ```bash
  clangd --check=Agent/src/main.cpp
  ```
- After cleaning the build (see below), reconfigure so clangd picks the database back up.

### … add a new command type?

1. Add the execution logic under `src/commands/exec/` (header in
   `include/agent/commands/exec/`) and expose one entry point per command. Return
   `commands::CommandResult` (`Ok(...)` / `Error(...)`), never a bare string — the task's `Status` is
   derived from it.
2. Add the value to `include/agent/models/task_types.h` (`enum class TaskType` / `TaskCode`); it must
   equal `Trinity.Shared.Enums.Tasks.TaskType` on the server (e.g. `Shell = 8`, `Escalate = 9`,
   `Token = 15`).
3. Route it in `CommandDispatcher::Dispatch()` and add a `HandleXxx` helper on the class.
4. Declare the fields it requires in `task::MissingCommandField()` (`src/task/task_validator.cpp`),
   using the server's wire key names, and add cases to `Agent/tests/task_validator_test.cpp`. Without
   this an incomplete command is executed with empty fields.
5. Parse any new fields in `TaskQueueManager::GetNextTask()` and store them on
   `models::CheckinResponse` (remember `CheckinResponse` is also where `PID`, `Path`,
   `TransferID`, `Username`/`Password` live).
6. Server-side: add the DTO in `Trinity.Shared/DTOs/Command/`, a validator in
   `TeamServer/Validators/Commands/`, an endpoint on `CommandController`, and a message
   case in `CommandService.FormatCreationResponse` (the switch is exhaustive — a missing
   case throws at runtime).
7. Add the command to `TeamServer.Tests/Contracts/TaskCommandContractTests.cs` so the required wire
   keys are pinned by a test.
8. If the new `TaskType` enum member changes the PostgreSQL enum, run `dotnet ef migrations add <name>`
   inside `TeamServer/` — the migrator container fails fast on pending model changes.

### … add a new API function (hashed import)?

No precomputed hash constants — names are passed xorstr-encrypted and hashed at
runtime (`include/agent/util/api_resolver.h`). Just resolve at the call site:

```cpp
const HMODULE module = agent::util::get_module(xorstr_("newmodule.dll"));
const auto new_api =
    agent::util::resolve_api<decltype(&NewApiFunctionA)>(module, xorstr_("NewApiFunctionA"));
```

- `decltype(&NewApiFunctionA)` gives the function-pointer type without emitting a static
  import for the function.
- The module loads on demand; its handle is kept for the process lifetime.

### … add a new platform implementation?

The current design uses `#ifdef _WIN32` guards in `.cpp` files. When a second platform
target is needed:

1. Create `src/platform/<name>_platform.cpp` with the platform-specific implementations.
2. Create `include/agent/platform/<name>_platform.h` with the interface.
3. Update CMake to select the correct implementation based on the target platform.
4. The `IPlatform` interface (`Username`, `Pid`, `MacAddress`, `RamTotalBytes`, `CpuCount`,
   `OsVersion`, `DiskSpace`, `InternalIp`) is the planned abstraction; nothing implements it yet.

### … cross-compile for a different architecture?

The CMake presets include `x86-debug`/`x86-release` (32-bit MSVC) and `x64-debug`/
`x64-release` (64-bit MSVC) for native Windows builds. For MinGW cross-compilation from
Linux, only the `x86_64` toolchain is currently configured. To add 32-bit:

1. Install `i686-w64-mingw32-g++`.
2. Create `Agent/cmake/toolchains/mingw-w64-i686.cmake` (mirror the x86_64 one).
3. Add a `mingw-i686` preset in `CMakePresets.json`.

### … debug the Agent?

#### Off-target (Linux)

- **Static analysis:** `clang-tidy` (see above) catches most logic errors.
- **Host unit checks:** pure-logic code (wire JSON, task validation) is compiled and run on the build
  host with no Windows target involved — that is what `--target check-units` does, and it is the
  cheapest way to verify a parsing or validation change:
  ```bash
  cmake --build Agent/out/build/mingw-x86_64 --target check-units
  ```
  Add a file under `Agent/tests/` for anything that does not need the Win32 API. If a piece of logic is
  hard to test, that is a hint it belongs behind the platform boundary (`#ifdef _WIN32` in the `.cpp`)
  rather than in a platform-neutral header.
- **Windows target:** Build with an MSVC preset (`x64-debug`) on a Windows machine and
  attach Visual Studio or WinDbg.

#### On the Win11 test VM

The end-to-end harness (`scripts/e2e-agent-test.sh`) automates everything below and asserts on
artifacts. The bullets that follow are the traps that have cost the most time — read them before
debugging by hand.

- **Keep the process alive.** OpenSSH on Windows runs each session in a job object, so
  `Start-Process` children are killed when the SSH session closes. Launch **with**
  redirection and keep the session open, or the agent disappears after a few seconds:
  ```powershell
  $p = Start-Process -FilePath (Join-Path $env:USERPROFILE 'agent_v3.exe') -PassThru `
        -WindowStyle Hidden -RedirectStandardError "$env:USERPROFILE\a.err" `
        -RedirectStandardOutput "$env:USERPROFILE\a.out"
  Start-Sleep -Seconds 200   # keep the session (and therefore the process) alive
  ```
- **Put markers in `%USERPROFILE%`, not `C:\Users\Public`.** Public's DACL grants nothing to
  `Users`/`Everyone` — only `INTERACTIVE`, `SERVICE`, `BATCH`, `SYSTEM` and `Administrators` — and an
  SSH session is a *network logon*, so the agent's token matches none of those. The symptom is
  misleading: `icacls C:\Users\Public` itself returns *Access is denied*, which looks like corruption
  or an integrity-level problem. An admin token *can* write there (service/network logons bypass UAC
  filtering and leave `Administrators` enabled), which is why this trap only appears for a
  standard-user agent.
- **Defender.** Real-time protection quarantines freshly built payloads
  (`Trojan:Win32/...!ml`) and removes scheduled tasks created for them. Tamper protection
  reverts `Set-MpPreference` and registry exclusions, and AMSI blocks exclusion scripts.
  Add the folder (`C:\Users\pi-user`) as an exclusion **from the Defender UI** before
  testing escalate / file-transfer features, otherwise you get unobvious failures such as
  “The file contains a virus or potentially unwanted software”. Defender's behaviour monitor also
  terminates the payload when it writes the ms-settings registry key, so escalate needs real-time
  protection off; `Get-MpThreatDetection` names the detection and the pid.
- **Session 0 has no interactive desktop.** UAC consent cannot be serviced there, so an auto-elevating
  binary runs at Medium and `runas /trustlevel` exits 1. Verify the Medium→High transition from an
  interactive session (RDP or the hypervisor console).
- **Diagnose crashes** with the trace, not stderr: an uncaught exception or an access violation
  produces no console output when the process is started hidden. Build with `-DAGENT_TRACE=ON` and read
  `%TEMP%\agent-<pid>.log` (the last line is the last thing that happened — every line is flushed), and
  launch it under `AgentSupervisor.exe` (`-DAGENT_SUPERVISOR=ON`) so the exit code is recorded
  (`0xC0000005` is a real crash; `0xC000013A`/`1` is usually the OpenSSH job-object teardown, which
  produces no dump). The harness does both: `TRACE_BUILD=1 ./scripts/e2e-agent-test.sh` builds and
  launches that way, and dumps the logs when a run fails.
- **Sign that the protocol is healthy** (TeamServer logs, `Debug` level):
  `Checkin uuid=… payloadLen=… knownAgent=True` followed by
  `Checkin handled: success=True length=108` (a task was returned) and a later checkin whose
  body is the result array.
- **Reading results:** a successful task stores its output in `TaskResult`; a failed one sets
  `Tasks.Status = 'error'` and puts the diagnostic in `Errors`. Query both before concluding a
  feature “does nothing”.

### … clean the build?

```bash
rm -rf Agent/out/build/mingw-x86_64
```

Then reconfigure and rebuild. Reconfiguring also regenerates `compile_commands.json`,
which clangd depends on (see "… use the language server (clangd)?"). There is no
`cmake --build ... --target clean` equivalent that fully removes generated files for
custom commands.

---

## Project Layout
Current Agent project layout:
```bash tree
Agent/
├── .clangd
├── .clang-format
├── .clang-tidy
├── cmake
│   └── toolchains
│       ├── mingw-w64-clang.cmake       # Clang + LLD + LTO
│       └── mingw-w64-x86_64.cmake
├── CMakeLists.txt
├── CMakePresets.json
├── doc
│   └── DEV.md                          # This file
├── external
│   ├── api_hashing                     # https://github.com/Maldev-Academy/ApiHashing/tree/8f0b5d240c82f82187398e52f2b1e182fba90afe
│   ├── base64.hpp
│   ├── rapidjson
│   ├── Windows.h                       # Case-sensitivity shim
│   └── xorstr                          # https://github.com/JustasMasiulis/xorstr/tree/066c64eea5104f4e3cfbc49e39031400e086425a
├── include
│   ├── agent
│   │   ├── agent.h
│   │   ├── commands
│   │   │   ├── command_dispatcher.h
│   │   │   ├── command_result.h        # CommandResult { success, output }
│   │   │   └── exec
│   │   │       ├── filetransfer.h
│   │   │       ├── keylogger.h
│   │   │       ├── kill_process.h
│   │   │       ├── privesc.h
│   │   │       ├── runu.h
│   │   │       ├── shell.h
│   │   │       └── token.h
│   │   ├── host
│   │   │   └── host_info.h
│   │   ├── models
│   │   │   ├── checkin_models.h
│   │   │   └── task_types.h            # enum class TaskType / TaskCode
│   │   ├── task
│   │   │   ├── task_queue_manager.h
│   │   │   └── task_validator.h        # required wire keys per TaskType
│   │   ├── transport
│   │   │   └── comm_manager.h
│   │   └── util
│   │       ├── api_resolver.h
│   │       ├── process.h
│   │       └── strings.h
├── src
│   ├── agent.cpp
│   ├── commands
│   │   ├── command_dispatcher.cpp
│   │   └── exec
│   │       ├── filetransfer.cpp # File upload/download via WinHTTP
│   │       ├── keylogger.cpp
│   │       ├── kill_process.cpp # Kill by PID: OpenProcess + TerminateProcess
│   │       ├── privesc.cpp
│   │       ├── runu.cpp
│   │       ├── shell.cpp
│   │       └── token.cpp
│   ├── host
│   │   └── host_info.cpp
│   ├── main.cpp
│   ├── task
│   │   ├── task_queue_manager.cpp
│   │   └── task_validator.cpp          # platform-neutral, host-tested
│   ├── transport
│   │   └── comm_manager.cpp
│   ├── util
│   │   ├── process.cpp
│   │   ├── read_gadgets_gas.s
│   │   └── strings.cpp
├── tests                              # host unit checks (--target check-units)
│   └── task_validator_test.cpp
└── xorstr.py
```
