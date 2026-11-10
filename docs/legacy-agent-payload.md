# Building a payload from the legacy agent

`Agent.Legacy/` is upstream's agent — `iamgred/Trinity` at `89b6440319a92e549a55cd45a6bb418360afd472`
(`test`), recorded in `Agent.Legacy/UPSTREAM` — and it now cross-compiles to an x64 Windows PE with
clang + lld + MinGW-w64 **inside the TeamServer image**, from the same build path the server uses for
`Agent/`.

## Selecting it

| Key                             | Values                                                      | Default                 |
| ------------------------------- | ----------------------------------------------------------- | ----------------------- |
| `PayloadBuilder:Sources:x64`    | `Agent` (the port), `Agent.Legacy`                          | `Agent`                 |
| `PayloadBuilder:Toolchains:x64` | `mingw-w64-clang.cmake` (default), `mingw-w64-x86_64.cmake` | `mingw-w64-clang.cmake` |

Both trees keep their toolchain in `cmake/toolchains/`, so the same `Toolchains` value applies to
either source. In a deployment the switch is an environment variable:

```bash
PayloadBuilder__Sources__x64=Agent.Legacy docker compose -f docker-compose.yml up -d teamserver
```

`POST /api/v1/payloads/generate` then builds from `Agent.Legacy` instead of `Agent`; everything else
(UUID, hosts, port, user agent, request header) is unchanged. The server logs one line per build —
`payload build: source=… arch=… toolchain=… build=…` — so a generated binary can be traced to the tree
it came from.

## Building by hand, without touching the running server

The toolchain is already in the image (`TeamServer/Dockerfile` installs `clang lld cmake ninja-build
g++-mingw-w64-x86-64`). A throwaway container gets the source from a mount and needs no compose change:

```bash
docker run --rm --entrypoint /bin/bash -v "$PWD:/src" sleepydev-teamserver:latest -lc '
  cd /src/Agent.Legacy &&
  cmake --preset mingw-clang-x86_64 \
    -DINPUT_HOSTS=192.168.122.20 -DINPUT_PAYLOAD_UUID=<uuid> -DPORT=5070 \
    -DINPUT_USER_AGENT="Trinity Agent Client/1.0" \
    -DINPUT_REQUEST_HEADER="Content-Type: application/json" &&
  cmake --build out/build/mingw-clang-x86_64'
```

The artifact is `Agent.Legacy/out/build/mingw-clang-x86_64/Agent.exe` (PE32+ x86-64, ~1.1 MB);
`Agent.Legacy/out/` is gitignored. `objdump -f` on it reports `file format pei-x86-64`.

## What had to be added, and why

The legacy sources are upstream's, byte for byte, except the block at the end of
`Agent.Legacy/CMakeLists.txt`. Everything else is additive:

- **`cmake/toolchains/mingw-w64-clang.cmake`** — adapted from `Agent/cmake/toolchains/`. Adds the
  compat include directory, `-DINITGUID` and the import libraries below.
- **`cmake/compat/mingw/metahost.h`** — MinGW-w64 ships `mscoree.h` (`ICorRuntimeHost`) but **not**
  `metahost.h`, which `Agent.cpp` includes for `ICLRMetaHost`/`ICLRRuntimeInfo`. The shim declares
  both interfaces in the SDK's method order with the documented GUIDs, and resolves
  `CLRCreateInstance` from `mscoree.dll` at runtime — MinGW has no `mscoree` import library, which is
  also why the sources' `#pragma comment(lib, "mscoree.lib")` cannot be honoured.
- **`__uuidof(mscorlib::_AppDomain)`** — `IID_PPV_ARGS(&AppDomain)` in `Agent.cpp:252` needs an IID
  that MSVC takes from the `MIDL_INTERFACE` attribute. The shim adds a `__CRT_UUID_DECL` with the
  value from `include/mscorlib.h` itself (`05F696DC-2B29-3663-AD8B-C4389CF2A713`).
- **`_Nullable`** — the generated `include/mscorlib.h` declares an interface named `_Nullable`, which
  clang treats as a nullability keyword. The shim `#define`s it to `LegacyMscorlibNullable` for the
  translation unit, which keeps the generated header unmodified.
- **`include/mscorlib.h`** — 3.5 MB generated from `mscorlib.tlb`, listed by `CMakeLists.txt`, tracked
  upstream. It used to be gitignored here because the tree was never built; it is tracked now.
- **Import libraries** — the sources pull theirs with MSVC `#pragma comment(lib, …)` (winhttp,
  iphlpapi, psapi, shlwapi, user32, ole32, ws2_32), which clang ignores, so `CMakeLists.txt` names
  them for the MinGW build only. `-fms-extensions` is deliberately not used: it would make clang act
  on those pragmas and fail on the missing `mscoree.lib`.

## Limits

- Only `x64` is configured, matching `PayloadBuilder:Toolchains` today.
- The payload **builds**; running it on the guest is not covered here. The port's `Agent/` is what the
  delivery ships and what the e2e gate exercises.
- The legacy tree keeps upstream's behaviour, including the gaps recorded in `REPORT.md` §6.1
  (`WM_KEYDOWN`-only keylogger, cmstp path). Building it does not repair them.
