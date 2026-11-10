# Deployment plan (KAN-74)

How the lab actually runs: host + Docker Compose, one Windows guest. For prerequisites see
[prerequisites.md](prerequisites.md); for the first-run sequence see [getting-started.md](getting-started.md).

## Topology

| Service            | Image                         | Port            | Role                                                    |
| ------------------ | ----------------------------- | --------------- | ------------------------------------------------------- |
| `postgres`         | `postgres:17-alpine`          | 5432            | the database; state lives in the `pgdata` volume        |
| `migrator`         | local build, `migrator` stage | —               | one-shot `efbundle` that applies migrations, then exits |
| `teamserver`       | local build, `runtime` stage  | 5000, 5069–5070 | the API, the HTTP listeners, and the payload compiler   |
| `client`           | local build                   | 5001            | Blazor Server UI that proxies the API                   |
| `teamserver-debug` | local build, debug override   | 5002            | idle debug container for VS Code; see the README        |

```text
Windows guest  ── checkin / listener traffic ──▶  host :5069  ──▶  teamserver
operator       ── JWT API / Client ─────────────▶  host :5000 / :5001 ──▶  teamserver / client
                                                       teamserver ──▶ postgres :5432
```

`docker-compose.yml` is the C2 stack; `docker-compose.debug.yml` adds the debug service. A bare
`docker compose` resolves to `compose.yaml` (the onion stack), so always name the file. `dbg-compose.sh` wraps
the right pair from any directory (`./dbg-compose.sh base ps`, `./dbg-compose.sh help`).

## Build and start

```bash
docker compose -f docker-compose.yml up -d --build      # first run, and after any server/Client change
docker compose -f docker-compose.yml up -d              # when only the Agent/ tree changed
./dbg-compose.sh                                        # C2 stack + debug container
```

Images are built locally; nothing is pulled from a registry except `postgres`. The `migrator` runs before
`teamserver` and must exit 0.

## What a change touches

| You changed                                                | Rebuild needed?       | Why                                                                                                                                      |
| ---------------------------------------------------------- | --------------------- | ---------------------------------------------------------------------------------------------------------------------------------------- |
| `Agent/` (C++ payload)                                     | **No**                | `Agent/` is bind-mounted read-only (`./Agent:/Agent:ro`) and compiled at payload-generation time, so a new payload uses the working tree |
| `TeamServer/`, `Trinity.Shared/`                           | **Yes**               | baked into the image                                                                                                                     |
| `Client/`                                                  | **Yes**               | baked into the image                                                                                                                     |
| `TeamServer/appsettings.json` (e.g. the payload toolchain) | **Yes**               | baked into the image — the default toolchain lives there                                                                                 |
| `.env` values                                              | **Restart** (`up -d`) | they are compose substitutions                                                                                                           |

Never edit `Agent/` while a server-built payload is generating: the compiler reads the bind mount live and a
save mid-build races it.

## State

| Data                           | Location                                                                                                      | Survives `down`?                                 |
| ------------------------------ | ------------------------------------------------------------------------------------------------------------- | ------------------------------------------------ |
| Database                       | `pgdata` named volume                                                                                         | yes (`down -v` deletes it)                       |
| Generated payloads             | host `./Payloads` → `/app/Payloads`                                                                           | yes                                              |
| File-transfer uploads/streams  | inside the container: `StoragePath`, default `<content root>/storage` (`files/<campaign>/<name>`, `streams/`) | **no** — not on a volume, so a recreate loses it |
| Legacy `/api/v1/files` uploads | inside the container: `<content root>/uploads`                                                                | **no**                                           |

If received files must survive a recreate, add a named volume and set `StoragePath` to it. The
file-transfer store is the one this lab leaves ephemeral.

## Windows guest

- Windows 11 with the OpenSSH server and `curl.exe`.
- An interactive desktop session for anything that needs a window: UAC escalation, `/IT` tasks, the
  console-session token route. Session 0 cannot service a UAC prompt.
- The guest must reach the host on the listener's bind port and on the API port; `Hosts` in the listener holds
  that address.

## Rollback

```bash
docker compose -f docker-compose.yml down          # stop + remove containers and network; volumes stay
docker compose -f docker-compose.yml down -v       # also delete the database volume
docker compose -f docker-compose.yml down --rmi all # also remove the locally built images
```

- A **payload** change needs no image rebuild; regenerate and redeploy it.
- A **server or Client** change needs `up -d --build`; the database migration runs first.
- If the migrator exits non-zero, the model and `ContextModelSnapshot` disagree — regenerate the migration
  rather than editing the container.
