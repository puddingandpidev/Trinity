# .NET assemblies and binaries (KAN-131)

Trinity runs a managed assembly **in memory** on the target: the server reads the file, base64-encodes it
into the task command, and the agent decodes it and invokes its entry point inside the CLR it hosts
(`NET_ASM`, task type 10). Nothing is written to the target's disk. The same route also covers "execute a
binary": stage the executable as a .NET assembly and let its `Main` do the work.

- **Prerequisites and the happy path:** [prerequisites.md](prerequisites.md), [getting-started.md](getting-started.md)
- **What the wire carries:** `.pi/notes/agent-wire-protocol.md`
- **How the CLR host is built:** `.pi/notes/agent-development.md` (module map) and `.pi/notes/open-items.md`

## 1. Build the assembly for .NET Framework 4.x

The agent hosts the **v4.0.30319** CLR, so the image must target .NET Framework 4.x. A .NET 6/8/10 image does
**not** load there — the task fails with a load error, not with a mysterious success. On Windows the
framework's own compiler is enough:

```powershell
& C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe /nologo /out:probe.exe probe.cs
```

The entry point must be `static void Main(string[] args)` or `static int Main(string[] args)` if you want an
argument vector. `Main` with no parameters is fine too — in that case the task must not carry `Arguments`.

## 2. Stage it where the server can read it

`AssemblyPath` is resolved **server-side**, inside the `teamserver` container, so the file has to be visible
there. `./Payloads` on the host is mounted at `/app/Payloads`:

```bash
cp probe.exe ./Payloads/probe.exe          # host side
# inside the container the server now sees /app/Payloads/probe.exe
```

A path that does not exist is rejected when the task is queued (`404`), not when the agent runs it.

## 3. Queue the task

```bash
curl -s -X POST http://localhost:5000/api/v1/commands/$AGENT_ID/execute/dotnetassembly \
  -H "$AUTH" -H 'Content-Type: application/json' \
  -d '{"AssemblyPath":"/app/Payloads/probe.exe","Arguments":"--flag \"quoted value\""}'
```

`Arguments` is split the way a Windows command line is split (`CommandLineToArgvW`), so quoting works. It is
passed only when the entry point declares parameters; surplus arguments on a parameterless `Main` fail the
task instead of being dropped.

## 4. Read the result honestly

`Invoke_3` does **not** capture the managed console, so the task's success message says only that the
assembly loaded and its entry point returned. To assert that the assembly did its work, have it write a file
and read that file back — the harness does exactly this (`.pi/notes/guest-testing.md`).

```bash
# success output
"Assembly loaded into the v4.0.30319 default AppDomain and its entry point invoked; managed console output is not captured"
# a failure names the stage and the HRESULT
"NET_ASM failed: _AppDomain::Load_3 returned HRESULT 0x80131040"
```

A failed task stores its reason in `Errors`, not `TaskResult` (`.pi/notes/agent-wire-protocol.md`).

## Limits worth knowing

- The CLR stays loaded in the agent process after the first `NET_ASM` task; there is no unload.
- Only the default AppDomain is used, and the assembly cannot be unloaded from it.
- The assembly image is passed as a `SAFEARRAY(VT_UI1)`, so very large tools cost memory twice (base64 on
  the wire, decoded copy in the agent).
- Defender's script/assembly heuristics can kill the payload when a known tool is loaded — this is a lab
  framework, see [security.md](security.md).
