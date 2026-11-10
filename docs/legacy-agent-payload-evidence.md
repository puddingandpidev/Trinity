# Evidence — `Agent.Legacy` payload builds

Recorded 2026-11-09 18:41 SAST by `dev` on the lab host, on branch
**`thrudr/agent-legacy-crossbuild`** at **`0da21ff52a3b3dcb0ed63dd7c2a85be838057d5d`** (based on
`dev` `f07e1f0`). This page is what makes the claim re-checkable without access to that host: the
commands, the log lines and the digests. The binaries themselves are not committed.

## Image

Everything ran inside the image the compose file builds from this commit:

```
docker build -t sleepydev-teamserver:legacyproof -f TeamServer/Dockerfile --target runtime .
image   sha256:99d5325c1a70126fe1cd9f0295ffd0832ffcb6224c010b6bd66b327a0d7b1ea6
created 2026-11-09T18:37:54+02:00, built from a clean checkout of 0da21ff
```

## The two runs

Two throwaway containers, same image, same database, same request — only
`PayloadBuilder__Sources__x64` differs. Each was built through the server (`POST
/api/v1/payloads/generate`), not by calling cmake directly.

```
docker run -d --name <container> --network sleepydev_default -p <port>:5000 \
  -e ConnectionStrings__Postgres='Host=postgres;Database=postgres;Username=postgres;Password=JAMZ' \
  -e ASPNETCORE_ENVIRONMENT=Development -e ASPNETCORE_URLS=http://+:5000 \
  -e Jwt__Key=… -e Jwt__Issuer=TrinityTeamServer -e Jwt__Audience=TrinityClient \
  [-e PayloadBuilder__Sources__x64=Agent.Legacy] \
  -v <repo>/Agent:/Agent:ro -v <repo>/Agent.Legacy:/Agent.Legacy:ro \
  sleepydev-teamserver:legacyproof

POST /api/v1/payloads/generate
{"name":"live-<label>","listenerID":11,"campaignID":22,"architecture":"x64",
 "type":"winexe","outputFormat":"winexe","retryStrategy":"linear","isStageless":true}
```

|                                               | legacy                           | control                          |
| --------------------------------------------- | -------------------------------- | -------------------------------- |
| container                                     | `legacy-proof-live`              | `port-proof-live`                |
| `StartedAt`                                   | `2026-11-09T16:41:32.595066964Z` | `2026-11-09T16:41:39.698611654Z` |
| port                                          | 5097                             | 5096                             |
| `PayloadBuilder__Sources__x64`                | `Agent.Legacy`                   | unset (default `Agent`)          |
| response                                      | `{"id":47}`                      | `{"id":48}`                      |
| `GET /api/v1/payloads/<id>` in that container | `200`, 1,145,344 B               | `200`, 1,310,208 B               |

Server log line, verbatim, from `docker logs <container> | grep 'payload build:'`:

```
payload build: source=Agent.Legacy arch=x64 toolchain=mingw-w64-clang.cmake build=/app/PayloadBuilds/6dc346c6-fbe7-4fcf-bdf6-075cb2e3b690
payload build: source=Agent arch=x64 toolchain=mingw-w64-clang.cmake build=/app/PayloadBuilds/0aea3d7d-ed44-4f6f-a6a8-87cf908fdd8f
```

## Artifacts

`docker exec <container> ls -l /app/Payloads/`:

```
-rwxr-xr-x 1 root root 1145344 Nov  9 16:41 live-legacy.exe
-rwxr-xr-x 1 root root 1310208 Nov  9 16:41 live-port.exe
```

`docker exec <container> objdump -f /app/Payloads/*.exe`:

```
/app/Payloads/live-legacy.exe:     file format pei-x86-64
/app/Payloads/live-port.exe:     file format pei-x86-64
```

`docker exec <container> objdump -p /app/Payloads/*.exe | grep Magic`:

```
Magic			020b	(PE32+)
Magic			020b	(PE32+)
```

`docker exec <container> sha256sum /app/Payloads/*.exe`:

```
321988ffa10f70f32de838e58c68458d55466bebcf9339247c74bfa4bcd8c0a3  /app/Payloads/live-legacy.exe
f9630e0cd01590ddc7c1316927ca0b951844cb7b532a6fff31af143a3616c206  /app/Payloads/live-port.exe
```

Host-side `file` on copies taken with `docker cp` (same digests):

```
PE32+ executable for MS Windows 6.00 (console), x86-64, 6 sections
```

## Build and tests on the same commit

```
dotnet build TeamServer/TeamServer.slnx --configuration Release   -> Build succeeded, 0 Error(s), 28 Warning(s)
dotnet test  TeamServer.Tests/TeamServer.Tests.csproj --configuration Release
                                                                 -> Passed! 159 / 0 failed / 0 skipped, 236 ms
git status --porcelain                                            -> empty
```

## State after capture

The two proof containers were stopped and their `Payloads` rows (`47`, `48`) deleted; earlier
attempts had created `43`–`46`, also deleted. The deployed stack was left with its three containers,
and no proof container or proof row remains. Because those rows pointed at files that lived only
inside the containers, deleting them is what keeps `/payloads` free of entries that 404.

## Limits

- The legacy payload was **built, not run**: no execution on the guest is claimed here.
- x64 only; there is no Debug/PDB preset for `Agent.Legacy` (`-Wl,--pdb` is wired in the shared
  toolchain, but the legacy tree has no debug configure path documented or exercised).
- The binaries are not committed; the digests above are what a rebuild should reproduce. Expect the
  digests to change if the toolchain image, the `INPUT_HOSTS`/UUID values or the listener config
  differ — the size and `pei-x86-64`/`PE32+` output are the stable comparison, not the hash.
