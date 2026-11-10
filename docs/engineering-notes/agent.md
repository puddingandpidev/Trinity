# Agent development (C++20 payload)

> Written while porting the legacy monolithic agent to the modular tree. Names of people, machines and internal links have been removed; the engineering content is unchanged.

Detail for work inside `Agent/`. The canonical long form is `Agent/doc/DEV.md`; keep the two in sync when
either changes. Root conventions (banner headers, naming, wire keys) stay in `AGENTS.md`.

## Porting background and module map

The legacy monolithic agent (originally `Agent/src/Agent.cpp`, preserved unchanged as
`Agent.Legacy/src/Agent.cpp`) has been ported to a modular architecture following
`Agent.RefactorExample/`. `Agent.Legacy/` serves two purposes since 2026-11-04 (the owner's decision): it is the
**reference** — synced wholesale from `upstream/test` when the comparison needs a newer monolith, never
hand-edited as reference — and it is the **drop area** for code a teammate still needs ported. A drop is input to
a port, not reference: port it, then delete it. Its build output (`build/`, `out/`) plus the generated
`include/mscorlib.h` are gitignored; `Agent.RefactorExample/` is a
template and **must not be committed**.

| Module                           | Responsibility                                                                                                                                                                                                                                                                                                      |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `transport/comm_manager`         | HTTP layer (WinHTTP), checkin protocol with payload UUID prefix/suffix handling                                                                                                                                                                                                                                     |
| `commands/command_dispatcher`    | Validates then routes by `models::TaskType` / `TaskCode` (enums in `models/task_types.h`, mirroring the server enums). Returns `commands::CommandResult { success, output }`; an unknown type or code is an error result, never a silent no-op                                                                      |
| `commands/command_result`        | `CommandResult`, the success/failure contract every command returns. `Agent::Start` maps `success` onto the task `Status`                                                                                                                                                                                           |
| `commands/exec/shell.cpp`        | PowerShell / shell execution (`POWERSHELL`, `SHELL`)                                                                                                                                                                                                                                                                |
| `commands/exec/privesc.cpp`      | UAC bypass via FodHelper.exe or cmstp.exe (`ESCALATE`)                                                                                                                                                                                                                                                              |
| `commands/exec/runu.cpp`         | Parent PID spoofing via `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS` + `CreateProcessW` (**not** `NtCreateProcess`)                                                                                                                                                                                                       |
| `commands/exec/kill_process.cpp` | `OpenProcess(PROCESS_TERMINATE)` + `TerminateProcess` for `PROCESS/KILL_PROCESS`; a refused handle is reported with `GetLastError`, never as success                                                                                                                                                                |
| `commands/exec/token.cpp`        | Token impersonation (`TOKEN`)                                                                                                                                                                                                                                                                                       |
| `commands/exec/keylogger.cpp`    | `GetAsyncKeyState` polling (todo: `WH_KEYBOARD_LL`; `KEYLOGGER` = 16, dispatched but the server has no route to queue it)                                                                                                                                                                                           |
| `commands/exec/filetransfer.cpp` | File upload/download via WinHTTP (`UPLOAD`, `DOWNLOAD`)                                                                                                                                                                                                                                                             |
| `commands/exec/netasm.cpp`       | Execute-assembly (`NET_ASM`): hosts the v4.0.30319 CLR (`CLRCreateInstance` → `ICLRMetaHost` → `ICorRuntimeHost`), reads the `Load_3` / `get_EntryPoint` / `GetParameters` / `Invoke_3` slots from the runtime's own `mscorlib.tlb` via `FUNCDESC.oVft`, and validates them against the `mscorlib.h` layout indices |
| `commands/exec/wintask.cpp`      | Persistence via COM Task Scheduler 1.0 (`WIN_TASK`): self-drop the payload at `Path`, then register it as a logon-triggered task named `TaskName`; hashed `CoInitializeEx`/`CoCreateInstance`, local GUIDs, no `uuid` link                                                                                          |
| `host/host_info`                 | System info gathering (hostname, username, external IP, OS version, MAC addresses, adapter enumeration, process listing)                                                                                                                                                                                            |
| `task/task_queue_manager`        | Task queue management; parses the task request into `CheckinResponse` and sets `has_command` when a command payload was actually present                                                                                                                                                                            |
| `task/task_validator`            | Platform-neutral required-field validation per `TaskType`: returns the missing **wire key** (`Path`, `TransferID`, `Commandlet`, `PID`, …) so a malformed command is rejected with the field named. Host-testable (`--target check-units`), so keep Windows APIs out of it                                          |
| `util/api_resolver`              | Hash-based Win32 API resolution (Maldev-Academy/ApiHashing)                                                                                                                                                                                                                                                         |
| `util/strings`                   | String utilities (`ToWide`, `ToNarrow`)                                                                                                                                                                                                                                                                             |
| `util/trace`                     | Compile-time structured trace (`-DAGENT_TRACE=ON`): one flushed line per event in `%TEMP%\agent-<pid>.log`. No-op macros otherwise, so a Release payload carries neither code nor strings. Traps and the guest procedure: these notes                                                                               |
| `util/process`                   | Process utilities (current process name)                                                                                                                                                                                                                                                                            |
| `models/checkin_models`          | Checkin request/response structures, including the `Status` / `Data` shape of task results                                                                                                                                                                                                                          |
| `models/task_types`              | `enum class TaskType` / `TaskCode` mirroring `Trinity.Shared.Enums.Tasks`                                                                                                                                                                                                                                           |

## Build

`CMakePresets.json` handles toolchain selection; toolchains live in `Agent/cmake/toolchains/`.

```bash
# Default: GCC + GNU ld
cmake -S Agent -B Agent/out/build/mingw-x86_64 --preset mingw-x86_64
cmake --build Agent/out/build/mingw-x86_64 --config Release

# Alternative: Clang + LLD with whole-program LTO (same behaviour, different PE)
cmake -S Agent -B Agent/out/build/mingw-clang-x86_64 --preset mingw-clang-x86_64
cmake --build Agent/out/build/mingw-clang-x86_64 --config Release
```

Both toolchains link `-static`; Release/MinSizeRel additionally strip symbols and debug sections (`-s`).

| Preset               | Toolchain file                            | Compiler + linker               | Notes                                                                                  |
| -------------------- | ----------------------------------------- | ------------------------------- | -------------------------------------------------------------------------------------- |
| `mingw-clang-x86_64` | `cmake/toolchains/mingw-w64-clang.cmake`  | Clang + LLD (whole-program LTO) | **Default** for payload builds; emits PDBs in debug builds                             |
| `mingw-x86_64`       | `cmake/toolchains/mingw-w64-x86_64.cmake` | GCC + GNU ld                    | Alternative: different front-end codegen, linker and PE layout for identical behaviour |

**Server-side selection.** `CMakePayloadBuilder` resolves the toolchain from
`PayloadBuilder:Toolchains:<arch>` (default `mingw-w64-clang.cmake`, mirrored in
`TeamServer/appsettings.json`). Point it at `mingw-w64-x86_64.cmake` for GCC + GNU ld. The runtime image
installs `clang`, `lld` and mingw (`TeamServer/Dockerfile`, `runtime-base` stage) — new Agent build
dependencies must be added there too.

**Verification.** The Clang/LLD build was validated on the Windows 11 test VM: check-in, CIG status
(`0x0001:CODEINTEGRITY_OPTION_ENABLED`) and `shell` task execution all match the GCC build.

### MSVC-ABI cross-compilation (xwin + clang-cl) — shelved

**Status: shelved (2026-11-05).** Revisit only if MSVC-ABI payloads are wanted for their own sake. If
adopted, fetch the SDK at build time rather than baking it into a distributed image (the licence-safe
avenue), and note it does not remove the need for runtime `oVft` slot resolution — `mscorlib.h` is generated
from `mscorlib.tlb`, not shipped with any SDK.

The mingw headers lack the CLR-hosting header (`metahost.h`) and ship only a partial `mscoree.h`. A real
Windows SDK can be used on Linux without Visual Studio:

- `xwin` downloads the MS CRT + Windows SDK headers/libs (`xwin --accept-license splat --output <dir>`). It
  still means accepting Microsoft's terms for those files; what it removes is the MSVC/VS _toolset_ install.
  The compiler is `clang-cl`, which is LLVM/Apache-2.0.
- `clang-cl --target=x86_64-pc-windows-msvc` then builds against that SDK, so `metahost.h` and the full
  `mscoree.h` are available.

It is a migration, not a header fix:

- CMake sets `MSVC=1` for clang-cl, so `CMakeLists.txt` takes the `ReadGadgets.asm` (MASM) branch and needs
  `ml64`, which xwin does **not** ship — assemble `src/util/read_gadgets_gas.s` with clang for the MSVC target.
- CRT linkage moves from mingw `-static` to `/MT`, import libs come from the SDK, and the resulting PE/import
  table is a new artifact class to re-validate on the guest (check-in, CIG, shell).
- The TeamServer builds payloads in Docker, so the SDK must be fetched at build time or baked into
  `runtime-base` (check the redistribution terms first), plus a `PayloadBuilder:Toolchains:x64` option and CI.
- It does **not** provide `mscorlib.h` — that is generated from `mscorlib.tlb`, and the design reads
  the vtable offsets from that type library at runtime, so no header is needed either way.

Decision for : keep mingw and hand-declare `ICLRMetaHost`/`ICLRRuntimeInfo`. Adopt xwin + clang-cl as
its own task if MSVC-ABI payloads are wanted for other reasons.

`CMAKE_EXPORT_COMPILE_COMMANDS` is ON, so configuring writes `compile_commands.json` into the build
directory — clangd and the `tidy` target both read it.

### Debug builds (symbols for a live guest)

Release has no debug info (and clang also LTOs), so add one of these when you need to debug a running
payload. Both are for debugging only — `CMakePayloadBuilder` always configures `Release`.

| Preset                     | Debug info                                                | Read by                                 |
| -------------------------- | --------------------------------------------------------- | --------------------------------------- |
| `mingw-x86_64-debug`       | DWARF (`-g -O0`, frame pointers kept)                     | gdb on the guest                        |
| `mingw-clang-x86_64-debug` | CodeView + `Agent.pdb` (`-gcodeview -Wl,--pdb=Agent.pdb`) | `cdb`/WinDbg; PDB also symbolises dumps |

Both debug presets also set `AGENT_TRACE=ON` and `AGENT_SUPERVISOR=ON`, so a debug payload writes its
own breadcrumbs to `%TEMP%\agent-<pid>.log` and the build emits `AgentSupervisor.exe` for exit-code
logging. Both options are off by default (Release payload unchanged); use them individually on a Release
build when you want the trace without symbols:

```bash
cmake -S Agent -B Agent/out/build/traced -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=Agent/cmake/toolchains/mingw-w64-clang.cmake \
      -DCMAKE_BUILD_TYPE=Release -DAGENT_TRACE=ON -DAGENT_SUPERVISOR=ON
```

Trap: `CMakePayloadBuilder` copies `Agent.exe` by name, so a helper target can never become the payload;
if you add another executable to `Agent/CMakeLists.txt`, keep its target name distinct from `Agent` and
guard it behind an option.

```bash
cmake -S Agent -B Agent/out/build/mingw-clang-x86_64-debug --preset mingw-clang-x86_64-debug
cmake --build Agent/out/build/mingw-clang-x86_64-debug
# → Agent.exe + Agent.pdb in that build dir
```

`RelWithDebInfo` is also wired (`-g -O1`, no LTO). Keep the PDB that belongs to the exact exe you send
to the guest — a stale one gives plausible but wrong stacks. See these notes for the
debugger side.

Required compile definitions are set in `CMakeLists.txt`:

- `COMM_HOSTS_LIST` — wide string list of listener hosts
- `COMM_PAYLOAD_UUID` — payload unique identifier
- `COMM_PORT` — connection port
- `COMM_USER_AGENT` — HTTP User-Agent header
- `COMM_REQUEST_HEADER` — HTTP Content-Type header

## Quality checks

```bash
cmake --build Agent/out/build/mingw-x86_64 --target format       # clang-format all sources in place
cmake --build Agent/out/build/mingw-x86_64 --target tidy         # clang-tidy all translation units
cmake --build Agent/out/build/mingw-x86_64 --target check-units  # host unit checks
python3 Agent/xorstr.py                                          # xorstr compliance
```

### Formatting

`clang-format` with the repository `.clang-format` (Google base, 4-space indent, 120 columns,
`SortIncludes: false`). Format one file with `clang-format -i path/to/file.cpp`. **Never** manually
reorder `#include` blocks — Windows headers (winsock2, windows, iphlpapi, …) have a required dependency
order that include sorting would break.

### clang-tidy

Check set in `.clang-tidy` (bugprone, clang-analyzer, cert, cppcore, misc, performance, portability,
readability, modernize, google, hicpp, llvm). tidy reads the compile database and fails with _"No
compilation database found"_ when the build directory was never configured — after `rm -rf`ing the build
dir, configure again first. Aim for zero diagnostics; suppress genuinely unfixable ones with
`// NOLINTNEXTLINE(check-name) // reason`. Two recurring cases:
`bugprone-multi-level-implicit-pointer-conversion` (passing `&handle` as `void*` to
`UpdateProcThreadAttribute`) and `readability-function-cognitive-complexity` (split the function rather
than adding a `NOLINT` where practical — `task_validator.cpp` does this). Vendored third-party code
(`external/rapidjson/`, `include/base64.hpp`) is excluded via `ExcludeHeaderFilterRegex` and a `SYSTEM`
include directory.

### Host unit checks (`--target check-units`)

Pure-logic code that does not touch the Windows API is compiled with the **build host's** compiler and run
immediately instead of shipping a payload to the guest:

```bash
cmake --build Agent/out/build/mingw-x86_64 --target check-units
# → compiles tests/*.cpp with g++/clang++ and runs the binary
```

Add a test file under `Agent/tests/` and, if it needs a translation unit, add that unit to the
`check-units` command in `CMakeLists.txt`. `tests/task_validator_test.cpp` is the model: no framework, a
tiny `ExpectEqual` helper, non-zero exit on failure. Keep the code under test platform-neutral — that is
why command validation lives in `task/task_validator.cpp` rather than in the dispatcher. If a piece of
logic is hard to test this way, that is a hint it belongs behind the platform boundary in the `.cpp`.

Two host test binaries exist: `task_validator_test` (required wire keys per `TaskType`) and
`task_parsing_test` (task deserialisation and result serialisation). Both are registered with CTest
(`enable_testing()` + `add_test`), so after the `check-units` target has built them,
`ctest --test-dir <build-dir>` runs them — ctest does not build, so run `check-units` first.

### String encryption (`xorstr_`)

Every string literal longer than 6 characters that appears in the compiled binary must be encrypted at
compile time with `xorstr_()` from `external/xorstr/xorstr.hpp` (Justas Masiulis, Apache-2.0,
SIMD-based). Plaintext API names, URLs, commands and registry paths are trivially greppable otherwise.

**Dangling pointer warning:** `xorstr_()` expands to `xorstr(str).crypt_get()` — it returns a
`char*`/`wchar_t*` into a temporary `xor_string` destroyed at the end of the current full-expression.
Passing it straight into a call is safe; storing it is not:

```cpp
// Good — consumed within the same full-expression
uint32_t hash = HASH_STRING_A(xorstr_("CreateFileA"));

// Good — copied into std::string before the temporary dies
std::string name{xorstr_("CreateFileA")};

// BAD — dangling pointer
const char* p = xorstr_("CreateFileA");
```

`Agent/xorstr.py` enforces this: it walks `src/` and flags any literal > 7 characters not wrapped in
`xorstr_()`. Short strings, format specifiers and strings inside `#include` directives are exempt.

**rapidjson gotcha:** `rapidjson::StringRef` is non-owning (`kConstStringFlag`). `StringRef(xorstr_("Key"))`
makes the document reference the xorstr temporary, which is re-encrypted when the full-expression ends, so
the serialised JSON contains ciphertext. Copy keys into a `std::string` that outlives the writer (see
`models::ResultRequest::ToJson()`):

```cpp
const std::string k_task_id{xorstr_("TaskID")};
doc.AddMember(rapidjson::StringRef(k_task_id.c_str()), task_id, alloc);
```

## The language server (clangd)

clangd gets its flags from `compile_commands.json`, which CMake writes to
`Agent/out/build/mingw-clang-x86_64/` on configure. `Agent/.clangd` points clangd at that directory —
auto-detection cannot find it because the build dir is not an ancestor of `src/`. Clang/LLD is the
default payload toolchain, so this is the database clangd should use; the other presets' databases are
only refreshed by configuring them.

- If the LSP logs c++17 indexing or missing Windows headers, the compile database is missing: re-run the
  configure command.
- Check a file parses with `clangd --check=Agent/src/main.cpp`.
- After cleaning the build, reconfigure so clangd picks the database back up.

## How do I …

### … add a new command type?

1. Add the execution logic under `src/commands/exec/` (header in `include/agent/commands/exec/`) and expose
   one entry point per command. Return `commands::CommandResult` (`Ok(...)` / `Error(...)`), never a bare
   string — the task's `Status` is derived from it.
2. Add the value to `include/agent/models/task_types.h` (`enum class TaskType` / `TaskCode`); it must equal
   `Trinity.Shared.Enums.Tasks.TaskType` on the server (e.g. `Shell = 8`, `Escalate = 9`, `Token = 15`).
3. Route it in `CommandDispatcher::Dispatch()` and add a `HandleXxx` helper on the class.
4. Declare the fields it requires in `task::MissingCommandField()` (`src/task/task_validator.cpp`), using
   the server's wire key names, and add cases to `Agent/tests/task_validator_test.cpp`. Without this an
   incomplete command is executed with empty fields.
5. Parse any new fields in `TaskQueueManager::GetNextTask()` and store them on `models::CheckinResponse`
   (which is also where `PID`, `Path`, `TransferID`, `Username`/`Password` live).
6. Server-side: add the DTO in `Trinity.Shared/DTOs/Command/`, a validator in
   `TeamServer/Validators/Commands/`, an endpoint on `CommandController`, and a message case in
   `CommandService.FormatCreationResponse` (the switch is exhaustive — a missing case throws at runtime).
7. Add the command to `TeamServer.Tests/Contracts/TaskCommandContractTests.cs` so the required wire keys
   are pinned by a test.
8. If the new `TaskType` member changes the PostgreSQL enum, run `dotnet ef migrations add <name>` inside
   `TeamServer/` — the migrator container fails fast on pending model changes.

### … add a new API function (hashed import)?

No precomputed hash constants — names are passed xorstr-encrypted and hashed at runtime
(`include/agent/util/api_resolver.h`). Resolve at the call site:

```cpp
const HMODULE module = agent::util::get_module(xorstr_("newmodule.dll"));
const auto new_api =
    agent::util::resolve_api<decltype(&NewApiFunctionA)>(module, xorstr_("NewApiFunctionA"));
```

`decltype(&NewApiFunctionA)` gives the function-pointer type without emitting a static import; the module
loads on demand and its handle is kept for the process lifetime.

### … add a new platform implementation?

The current design uses `#ifdef _WIN32` guards in `.cpp` files. When a second platform target is needed:

1. Create `src/platform/<name>_platform.cpp` with the platform-specific implementations.
2. Create `include/agent/platform/<name>_platform.h` with the interface.
3. Update CMake to select the implementation from the target platform.
4. The `IPlatform` interface (`Username`, `Pid`, `MacAddress`, `RamTotalBytes`, `CpuCount`, `OsVersion`,
   `DiskSpace`, `InternalIp`) is the planned abstraction.

### … cross-compile for a different architecture?

The CMake presets include `x86-debug`/`x86-release` (32-bit MSVC) and `x64-debug`/`x64-release` (64-bit
MSVC) for native Windows builds. For MinGW cross-compilation from Linux only the `x86_64` toolchain is
configured. To add 32-bit:

1. Install `i686-w64-mingw32-g++`.
2. Create `Agent/cmake/toolchains/mingw-w64-i686.cmake` (mirror the x86_64 one).
3. Add a `mingw-i686` preset in `CMakePresets.json`.

### … use the optional obfuscation pass plugin?

The Clang toolchain accepts an optional LLVM New-PM **pass plugin** for IR-level obfuscation:

```bash
cmake -S Agent -B Agent/out/build/mingw-clang-x86_64 --preset mingw-clang-x86_64 \
      -DAGENT_PASS_PLUGIN=/path/to/libObfuscator.so
```

Off by default and rejected for non-Clang toolchains, so GCC builds are unaffected. The plugin's ABI must
match the LLVM version of the `clang` that loads it — plugin and compiler must agree on
`llvm/Plugins/PassPlugin.h` (API v2/v3). Candidates:

- **Morok** (https://github.com/19h/morok) — MIT, modular C++23 LLVM New-PM IR obfuscator (control-flow
  flattening, MBA/substitution, string/constant encoding, vtable and VM bytecode rewriting, optional
  runtime self-protection). HEAD tracks **LLVM 23**; builds as a loadable `libMorok.so`, so it needs no
  compiler fork once LLVM 23 is available.
- **IRvana** (https://github.com/m3rcer/IRvana) — MIT, OLLVM integration and LLVM IR tooling for Windows.

> **Status on this machine:** `llvm-22` is the newest obtainable (Debian trixie and apt.llvm.org stable
> both stop at 22) and Morok HEAD requires LLVM 23 APIs. Porting it would mean patching ~60 pass headers
> plus the control-flow flattener — a fork-level change whose correctness could not be validated — so it
> is deliberately not vendored. `AGENT_PASS_PLUGIN` makes it a one-flag opt-in once LLVM 23 is installed.

### … debug the Agent?

Off-target (Linux):

- **Static analysis:** `clang-tidy` (above) catches most logic errors.
- **Host unit checks:** pure-logic code (wire JSON, task validation) runs on the build host with no Windows
  target involved — the cheapest way to verify a parsing or validation change.
- **Windows target:** build with an MSVC preset (`x64-debug`) on a Windows machine and attach Visual Studio
  or WinDbg.

On the Windows 11 guest: `scripts/e2e-agent-test.sh` automates the whole loop and asserts on artifacts — see
these notes (harness, environment), these notes (symbols, debugger,
launching outside the SSH job) and `honest-results.md` (what has already broken, and how) before
debugging by hand.

### … clean the build?

```bash
rm -rf Agent/out/build/mingw-x86_64
```

Then reconfigure and rebuild — reconfiguring also regenerates `compile_commands.json`, which clangd needs.
There is no `--target clean` equivalent that fully removes generated files for the custom commands.

## Vendored code

Third-party code lives in `Agent/external/` and is **not modified**:

- `external/rapidjson/` — JSON parsing (SYSTEM include, excluded from tidy)
- `external/base64.hpp` — base64 encode/decode (excluded from tidy)
- `external/api_hashing/` — Maldev Academy ApiHashing (MIT)
- `external/Windows.h` — case-sensitivity shim for Linux builds
- `external/xorstr/` — Justas Masiulis xorstr (Apache-2.0), SIMD string encryption

If vendored code must be patched, fork it into `src/` and patch there.

## Project layout

```text
Agent/
├── .clangd
├── .clang-format
├── .clang-tidy
├── cmake/toolchains
│   ├── mingw-w64-clang.cmake       # Clang + LLD + LTO
│   └── mingw-w64-x86_64.cmake
├── CMakeLists.txt
├── CMakePresets.json
├── doc/DEV.md                      # Canonical development guide
├── external
│   ├── api_hashing                 # Maldev-Academy/ApiHashing
│   ├── base64.hpp
│   ├── rapidjson
│   ├── Windows.h                   # Case-sensitivity shim
│   └── xorstr
├── include/agent
│   ├── agent.h
│   ├── commands
│   │   ├── command_dispatcher.h
│   │   ├── command_result.h        # CommandResult { success, output }
│   │   └── exec/{filetransfer,keylogger,privesc,runu,shell,token}.h
│   ├── host/host_info.h
│   ├── models
│   │   ├── checkin_models.h
│   │   └── task_types.h            # enum class TaskType / TaskCode
│   ├── task
│   │   ├── task_queue_manager.h
│   │   └── task_validator.h        # required wire keys per TaskType
│   ├── transport/comm_manager.h
│   └── util/{api_resolver,process,strings,trace}.h
├── src
│   ├── agent.cpp
│   ├── commands
│   │   ├── command_dispatcher.cpp
│   │   └── exec/{filetransfer,keylogger,privesc,runu,shell,token}.cpp
│   ├── host/host_info.cpp
│   ├── main.cpp
│   ├── task
│   │   ├── task_queue_manager.cpp
│   │   └── task_validator.cpp      # platform-neutral, host-tested
│   ├── transport/comm_manager.cpp
│   └── util
│       ├── process.cpp
│       ├── read_gadgets_gas.s
│       ├── strings.cpp
│       └── trace.cpp               # -DAGENT_TRACE=ON only
├── tests                           # host unit checks (--target check-units)
│   └── task_validator_test.cpp
├── tools
│   └── supervisor/supervisor.cpp   # AgentSupervisor.exe, -DAGENT_SUPERVISOR=ON only
└── xorstr.py
```
