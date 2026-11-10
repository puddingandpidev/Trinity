# Trinity

_NB!!_
As per company request
we are not hosting this, as its a containerized system meant to be portable.

# Project Requirement

## Running the project

The stack uses a default local development database password when no override is provided. To set a private password, copy `.env.example` to `.env` and change `POSTGRES_PASSWORD`. Shell environment variables are prioritised over `.env` entries. Postgres applies this password only when initializing an empty data volume. Changing it later does not rotate the password in an existing database; update the database role explicitly, or reset the volume if its data can be discarded.

`bash` Example:

```bash
cp .env.example .env
sed -i 's/please-replace-this-postgres-password/your-own-secure-local-postgres-password/g' .env
```

### Docker

This repository tracks three compose files, and Docker Compose resolves a bare `docker compose` to
`compose.yaml` — the onion stack, not the C2 stack — so always name the file you mean:

| File                       | Stack                                                                    |
| -------------------------- | ------------------------------------------------------------------------ |
| `docker-compose.yml`       | Trinity C2 stack: postgres, migrator, teamserver (:5000), client (:5001) |
| `docker-compose.debug.yml` | debug override adding `teamserver-debug` (:5002); see the next section   |
| `compose.yaml`             | onion stack: tor, socks-proxy, weather-api, redirector                   |

1. Start the stack:  
   `docker compose -f docker-compose.yml up -d`
2. Rebuild and start:  
   `docker compose -f docker-compose.yml up -d --build`
3. Check service health:  
   `docker compose -f docker-compose.yml ps`
4. Open the client at http://localhost:5001.

`dbg-compose.sh` wraps the same commands and always names the right file pair, from any directory:  
`./dbg-compose.sh` rebuilds the C2 stack plus the debug container, `./dbg-compose.sh base ps` is the C2
stack only, and `./dbg-compose.sh help` lists the rest.

### Debugging Containerised TeamServer (VS Code)

The debug-only container runs idle until VS Code launches the API under the debugger. It leaves the regular TeamServer available on port 5000 and serves the debug instance on port 5002.

Start the debug container with:  
`docker compose -f docker-compose.yml -f docker-compose.debug.yml up -d --build teamserver-debug`

After the stack and debug container have been started once, rebuild only the debug service after a TeamServer code edit:  
`docker compose -f docker-compose.yml -f docker-compose.debug.yml up -d --no-deps --build teamserver-debug`

The `--no-deps` form assumes Postgres is already running and migrations have already completed. The Dockerfile keeps package restore, runtime system packages, and `vsdbg` in cached layers, and excludes migration-bundle generation from a debug-only build. Source changes still need to be compiled into the image, but should not reinstall those dependencies. In VS Code, use Restart after rebuilding to launch the updated API.

In VS Code, start `Launch TeamServer in debug container`, then open Swagger at http://localhost:5002. The debug override adds `vsdbg` and `SYS_PTRACE`; this should be used only for local development.

The debug image's pinned Linux x64 `vsdbg` archive was validated with C# Dev Kit (`ms-dotnettools.csdevkit`) 3.40.215 and the C# extension (`ms-dotnettools.csharp`) 2.160.4. The archive URL and SHA-256 are pinned in `TeamServer/Dockerfile` to match that C# extension. When upgrading the extension, update and verify the debugger archive and checksum together.

## Documentation

Operator and deployment guides live under [`docs/`](docs/):

| Document                                             | Covers                                                                                                                                        |
| ---------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| [`docs/getting-started.md`](docs/getting-started.md) | the happy path: configure, start, log in, create a listener and campaign, generate and deploy a payload, queue a command (KAN-117)            |
| [`docs/prerequisites.md`](docs/prerequisites.md)     | host, payload toolchain, ports and Windows guest requirements (KAN-116)                                                                       |
| [`docs/deployment.md`](docs/deployment.md)           | the compose topology, what a rebuild touches, where state lives, rollback (KAN-74)                                                            |
| [`docs/troubleshooting.md`](docs/troubleshooting.md) | the failure modes this project has hit, with the check for each (KAN-137)                                                                     |
| [`docs/security.md`](docs/security.md)               | what is and is not authenticated, secrets, payload protections (KAN-69)                                                                       |
| [`docs/net-assemblies.md`](docs/net-assemblies.md)   | execute-assembly (`NET_ASM`): build a .NET Framework 4.x assembly, stage it in `./Payloads`, queue it, and read the result honestly (KAN-131) |

## Clean Up

When done, stop and remove the containers and network. To remove the local database too, include `-v`; this deletes the database volume.

For the **normal** stack run:  
`docker compose -f docker-compose.yml down`  
If the **debug** override was used, to remove both TeamServer containers run:  
`docker compose -f docker-compose.yml -f docker-compose.debug.yml down`  
Add `-v` to either command to delete the database volume; add `--rmi all` to also remove project images.

# Video Links Used In Presentation for Code Walkthrough

- Presentation: https://www.youtube.com/watch?v=M2PHoHGGW1Y
- System Demo: https://www.youtube.com/watch?v=vw6UiIC5VX4
- James Marchall code walkthrough: https://www.youtube.com/watch?v=nw_U1A0IO_c
- Lyle Richardson code walkthrough: https://www.youtube.com/watch?v=DpOx4p39GXM&feature=youtu.be
- Connaire Myall code walkthrough: https://www.youtube.com/watch?v=krvivaSBQH0
- Simon Slingerland code walkthrough: https://www.youtube.com/watch?v=UXqz9NEM7fI

---

## Which branch to work on (delivered build — 2026-11-09)

`dev` is the integration branch and the branch to clone. It contains the delivered
server, agent and Blazor client:

| content | branch | sha |
| --- | --- | --- |
| server + agent | `ghostwrite/backend` | `a903b9d` |
| Blazor client | `ghostwrite/frontend` | `31a2135` |
| merged into `dev` | PR #2 then PR #1 | `52cb197`, `c568413` |

### Evidence, with the commands that produced it

All three lines below were run on this repository; the runner and date are part of the
claim, because a number without one is a rumour.

| what | command | result | runner · when |
| --- | --- | --- | --- |
| client build | `dotnet build Client/Client.slnx --configuration Release` | **0 errors**, 28 warnings | manager · 2026-11-09 18:11 SAST · at `c568413` |
| server tests | `dotnet test TeamServer.Tests/TeamServer.Tests.csproj --configuration Release` | **159 passed, 0 failed, 0 skipped** (336 ms) | manager · 2026-11-09 18:11 SAST · at `c568413` |
| live end-to-end gate | `AGENT_USER=admin AGENT_LAUNCH=task ./scripts/e2e-agent-test.sh` | **64 passed, 0 failed, 0 skipped** (7m21s, task mode) | manager · 2026-11-09 09:51–09:59 SAST · at `31a2135` / `a903b9d` |

### What does **not** build

- The previous `dev` / `test` tip `89b6440` (also upstream `iamgred/Trinity` `test`): the client
  does **not** compile — 7 errors, and the test project 7 errors. Measured twice, by two people,
  on 2026-11-09. If you have an old clone of `dev`, pull: the merges above replace it.
- Upstream `master` `92bb12f` builds but contains none of this deliverable.

### Upstream tracking and why a clean merge is not available

- `test` **tracks upstream**: fast-forwarded to upstream `iamgred/Trinity` `test` `9d51428` on
  2026-11-09 (was `89b6440`; zero local commits, ancestor check passed, read back from the API).
- `master` is identical to upstream `master` `92bb12f`.
- **`dev` cannot be merged cleanly into upstream `test`.** Measured with
  `git merge-tree --write-tree --name-only origin/dev upstream/test` → exit 1, **128 conflicted
  paths**, on top of incompatible EF migration histories (upstream regenerated `20261109114828`;
  this tree carries a 6-migration chain from `20261025211917_init`). That is a reconciliation
  project, not a setting: merging it needs a decision about which migration chain survives.
  Runners: human-interface and manager, 2026-11-09, same result.
- Each `.slnx` builds the whole tree (each lists the other's `.csproj`), so a server-side failure
  can surface as a client-build failure and vice versa.
- `master` is intentionally left as the upstream skeleton; the work is here.
