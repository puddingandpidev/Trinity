# Security (KAN-69)

What Trinity does and does not protect, stated plainly. This is a red-team framework built for an isolated
lab; treat it as hostile software everywhere else.

## What is authenticated

- **The operator API is JWT bearer.** `AuthService` issues an HS256 token, and every operator controller
  carries `[Authorize]`; an anonymous call to an operator route returns `401`. The anonymous routes are the
  login, the liveness checks (`GET /health` and `GET /api/v1/server/status`), and the agent check-in
  endpoints (below).
- **The Client** attaches the token through `AuthorizationMessageHandler`. It is a UI proxy, not a second
  trust boundary — it holds the same bearer token the API accepts.

## What is not authenticated

- **Agent check-in is deliberately unauthenticated.** A payload identifies itself with the payload UUID and
  the content of its check-in body; there is no per-agent secret and no mutual authentication. Anyone who can
  reach the check-in endpoint and present a known UUID can impersonate an agent. Do not expose a listener to an
  untrusted network.
- **File-transfer endpoints** are keyed by campaign and name, not by a signed capability. Treat them as
  internal to the lab.

## Secrets

| Secret                     | Where it lives                                                                                                | Default                                 |
| -------------------------- | ------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| Operator bootstrap account | `TeamServer/appsettings.json` → `Bootstrap`, or `Bootstrap__OperatorUsername` / `Bootstrap__OperatorPassword` | `operator` / `trinity-operator`         |
| JWT signing key            | `.env` → `JWT_KEY`, or `Jwt:Key`                                                                              | a development key in `appsettings.json` |
| Postgres password          | `.env` → `POSTGRES_PASSWORD`, or the compose fallback                                                         | `JAMZ` when no override is set          |

The lab defaults must never ship. Set `POSTGRES_PASSWORD` and a `JWT_KEY` of at least 32 bytes before the
stack leaves a workstation; changing the Postgres password later does not rotate it in an existing volume.
`GET /api/v1/server/status` and Swagger are unauthenticated/visible in Development — keep the environment out
of a shared network.

## Payload protections

- **Strings are encrypted at compile time.** Every literal longer than a few characters goes through
  `xorstr_()` so API names, URLs and registry paths are not greppable in the binary; `Agent/xorstr.py` enforces
  it in CI.
- **Imports are resolved by hash.** `util/api_resolver.h` loads a module and looks up an export by CRC32b name
  hash, so the sensitive Win32/ole32 calls do not appear in the import table.
- These are anti-analysis measures, not a security boundary. The payload is **lab-use only**.

## Virtualisation and Defender

- **Defender will kill it.** Real-time protection quarantines freshly built payloads and removes scheduled tasks
  created for them; its behaviour monitor terminates the payload the moment it writes the ms-settings key that
  the UAC bypass needs. Tamper protection may prevent disabling either. Add the test folder as an exclusion from
  the Defender UI, and expect escalation to need real-time protection off.
- **UAC bypass is conditional.** It needs an administrator account in an interactive session; the value of
  `ConsentPromptBehaviorAdmin` decides whether the prompt is auto-denied, consented or credential-prompted, and
  a standard user cannot start an auto-elevate binary at all.

## The integrity lesson

The most damaging defects in this project were not crashes: they were commands that **reported success and did
nothing**. A missing wire key executed with an empty field and a success-shaped result; a failed transfer
returned an ACK page and the agent believed it. The rules that came out of that — every command returns
`commands::CommandResult`, a failed task's reason goes to `Errors`, status is derived not assumed, and the
contract tests pin the required keys per `TaskType` — are security work. `.pi/notes/defect-history.md` records
each case and how it was found. Never weaken a validator or a status derivation to make a symptom disappear.

## See also

- [deployment.md](deployment.md) — where state and secrets live in the stack.
- [troubleshooting.md](troubleshooting.md) — the failure modes above, as symptoms.
