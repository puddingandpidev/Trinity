# Technical prerequisites (KAN-116)

Everything needed to build and run Trinity locally, split into what the **host** needs and what the
**Windows guest** needs. For the step-by-step happy path see [getting-started.md](getting-started.md); for the
runtime topology see [deployment.md](deployment.md).

## Host

| Requirement                | Why                                                                            | Check                                      |
| -------------------------- | ------------------------------------------------------------------------------ | ------------------------------------------ |
| .NET 10 SDK                | builds `TeamServer`, `Client`, `Trinity.Shared` and the tests                  | `dotnet --version`                         |
| Docker Engine + Compose v2 | runs the C2 stack; the payload toolchain ships **inside** the TeamServer image | `docker version`, `docker compose version` |
| `git`                      | clone and branch                                                               | `git --version`                            |
| `curl`                     | health checks and the API examples                                             | `curl --version`                           |

The .NET SDK is only needed to build or run the tests on the host. Running the stack through Docker does
not need a host .NET install for the server; the image builds it.

## Payload toolchain

The Agent is a Windows C++20 payload cross-compiled with MinGW-w64. **Nothing is needed on the host for
server-built payloads**: the `runtime-base` stage of `TeamServer/Dockerfile` installs `clang`, `lld`,
`cmake`, `ninja` and `g++-mingw-w64-x86-64`, so `docker compose ... --build` produces a TeamServer that can
compile `Agent/` at payload-generation time.

Install the local toolchain only for **developer builds** and the quality targets:

| Tool                                         | Used by                                         |
| -------------------------------------------- | ----------------------------------------------- |
| `cmake` (3.2x) + `ninja`                     | configure/build a preset                        |
| `clang` + `lld`, or `x86_64-w64-mingw32-g++` | the two payload toolchains                      |
| `clang-format`                               | `--target format`                               |
| `clang-tidy`                                 | `--target tidy` (reads `compile_commands.json`) |
| `g++` or `clang++`                           | `--target check-units` (host unit checks)       |
| `python3`                                    | `Agent/xorstr.py` string-encryption compliance  |

The two toolchains are equivalent in behaviour; **Clang/LLD is the default**
(`PayloadBuilder:Toolchains:x64 = mingw-w64-clang.cmake`, baked into `TeamServer/appsettings.json`). The GCC
toolchain (`mingw-w64-x86_64.cmake`) is the alternative. Only `x64` builds today.

## Ports

| Port      | Service                                                   |
| --------- | --------------------------------------------------------- |
| 5000      | TeamServer API                                            |
| 5001      | Client UI                                                 |
| 5002      | `teamserver-debug` (only with `docker-compose.debug.yml`) |
| 5432      | PostgreSQL                                                |
| 5069–5070 | HTTP listener bind ports used by the lab                  |

They must be free on the host before `docker compose up`.

## Windows guest

A Windows 11 VM is the test target:

- **OpenSSH server** so the harness can drive it over key-based SSH (`win11-user`; optionally
  `win11-admin` for Defender/cleanup operations).
- **`curl.exe`** for the listener/API reachability check.
- An **interactive desktop session** for anything that needs a real window: UAC escalation, `/IT` scheduled
  tasks, and the console-session token route. Session 0 cannot service a UAC prompt.
- The guest reaches the host over the libvirt network; see `.pi/notes/guest-testing.md` for the addresses and
  the ICMP trap (ping is filtered even when the guest is up — probe SSH instead).

## See also

- [deployment.md](deployment.md) — the lab topology and rollback.
- [getting-started.md](getting-started.md) — the exact happy path.
- [troubleshooting.md](troubleshooting.md) — what breaks and why.
