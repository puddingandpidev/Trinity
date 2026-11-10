#!/usr/bin/env bash
# ========================================== { START OF FILE } ========================================== #
# scripts/e2e-remote.sh
#
# One-liner entry point for the acceptance harness, meant to be called over ssh
# from a machine that has no guest keys of its own:
#
#   ssh -t testbox '~/project/scripts/e2e-remote.sh'
#   ssh -t testbox 'PAYLOAD_SOURCE=clang ~/project/scripts/e2e-remote.sh'
#
# What it does
#   1. resolves the repo root from its own path, so the caller's cwd does not matter
#   2. runs the four preflight checks the harness depends on, and STOPS before
#      creating anything if one fails (the harness would die later, after a tmux
#      session had already been spawned)
#   3. runs scripts/e2e-agent-test.sh inside tmux with the output tee'd to a log,
#      so a dropped connection does not kill the run; it attaches to that session,
#      and Ctrl-b d leaves it running
#   4. returns the harness exit code: 0 green, 1 a case failed, 2 stopped early
#
# Options
#   --no-tmux          run in this terminal instead (CI, or already inside tmux)
#   --detach           start the tmux session and return immediately
#   --preflight-only   run the checks and stop
#   --print            show what would run, execute nothing
#   --force            start even if another harness is already running
#   --session NAME     tmux session name            (default e2e-<timestamp>)
#   --log PATH         log file                     (default /tmp/e2e-<timestamp>.log)
#   -- ARGS...         everything after -- is passed to scripts/e2e-agent-test.sh
#
# Environment: anything the harness reads is inherited - PAYLOAD_SOURCE, TRACE_BUILD,
# KEEP_ARTIFACTS, CHECKIN_TIMEOUT, TASK_TIMEOUT, LISTENER_NAME, SSH_HOST, API_URL...
# E2E_HARNESS overrides the harness path (used by this script's own test to stub it).
# ========================================== { END OF FILE } ========================================== #

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HARNESS="${E2E_HARNESS:-$REPO_ROOT/scripts/e2e-agent-test.sh}"
RUN_ID="$(date +%F-%H%M%S)"
SESSION="e2e-${RUN_ID}"
LOG="/tmp/e2e-${RUN_ID}.log"
STATUS="/tmp/e2e-${RUN_ID}.status"
USE_TMUX=1
DETACH=0
PREFLIGHT_ONLY=0
PRINT_ONLY=0
FORCE=0
HARNESS_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --no-tmux)        USE_TMUX=0; shift ;;
        --detach)         DETACH=1; shift ;;
        --preflight-only) PREFLIGHT_ONLY=1; shift ;;
        --force)          FORCE=1; shift ;;
        --print)          PRINT_ONLY=1; shift ;;
        --session)        SESSION="${2:-}"; shift 2 ;;
        --log)            LOG="${2:-}"; STATUS="${2:-}.status"; shift 2 ;;
        --)               shift; HARNESS_ARGS=("$@"); break ;;
        -h|--help)        sed -n '3,33p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)                echo "e2e-remote: unknown argument: $1" >&2; exit 2 ;;
    esac
done

# --------------------------------------------------------------------------- output
if [ -t 1 ]; then
    C_OK=$'\033[32m'; C_BAD=$'\033[31m'; C_RESET=$'\033[0m'
else
    C_OK=''; C_BAD=''; C_RESET=''
fi
step() { printf '\n== %s ==\n' "$1"; }
ok()   { printf '   %sok%s    %s\n' "$C_OK" "$C_RESET" "$1"; }
fail() { printf '   %sFAIL%s  %s\n' "$C_BAD" "$C_RESET" "$1"; FAILED=1; }
FAILED=0

API_URL="${API_URL:-http://192.168.122.20:5000}"
SSH_HOST="${SSH_HOST:-win11-user}"
COMPOSE_FILE="${COMPOSE_FILE:-docker-compose.yml}"
PG_USER="${PG_USER:-postgres}"
PG_DB="${PG_DB:-postgres}"

# --------------------------------------------------------------------------- runner
# shellcheck disable=SC2016  # the body is generated: its $ and % are for the runner, not for us
write_runner() {
    local runner="$1"
    {
        printf '#!/usr/bin/env bash\n'
        printf 'cd %q || exit 2\n' "$REPO_ROOT"
        printf 'set -o pipefail\n'
        printf '%q' "$HARNESS"
        local a
        for a in "${HARNESS_ARGS[@]+"${HARNESS_ARGS[@]}"}"; do printf ' %q' "$a"; done
        printf ' 2>&1 | tee %q\n' "$LOG"
        printf 'rc=$?\n'
        printf 'printf "%%s\\n" "$rc" > %q\n' "$STATUS"
        printf 'printf "\\nlog: %%s\\n" %q\n' "$LOG"
        printf 'exit "$rc"\n'
    } >"$runner"
    chmod +x "$runner"
}

REPO_RUNNER="$(mktemp "${TMPDIR:-/tmp}/e2e-run.XXXXXX")"
write_runner "$REPO_RUNNER"
trap 'rm -f "$REPO_RUNNER"' EXIT

# --------------------------------------------------------------------------- preflight
step "preflight"
# One suite at a time: two runs fight over the guest's agent process and the task
# rows, and killing one by pattern kills both (and every shell whose command line
# mentions the harness). Bracket keeps this pattern from matching itself.
RUNNING="$(pgrep -f '[e]2e-agent-test\.sh' | head -1)"
if [ -n "$RUNNING" ] && [ "$FORCE" = 0 ]; then
    fail "another harness is already running (pid $RUNNING) - stop it with: tmux kill-session -t <session>"
    printf '\nStopped. Use --force only if you are sure that run is dead.\n' >&2
    exit 2
fi

if [ -x "$HARNESS" ]; then ok "harness: $HARNESS"; else fail "harness not executable: $HARNESS"; fi

code="$(curl -s -o /dev/null -w '%{http_code}' -m 5 "$API_URL/api/v1/server/status" 2>/dev/null || true)"
if [ "$code" = "200" ]; then
    ok "TeamServer at $API_URL (200)"
else
    fail "TeamServer not answering at $API_URL (got ${code:-no response})"
fi

if ssh -o BatchMode=yes -o ConnectTimeout=8 -o LogLevel=ERROR "$SSH_HOST" 'cmd /c echo e2e-ok' 2>/dev/null | grep -q 'e2e-ok'; then
    ok "guest ssh: $SSH_HOST"
else
    fail "cannot ssh to $SSH_HOST (guest down, or no key/alias on this machine)"
fi

if docker compose -f "$REPO_ROOT/$COMPOSE_FILE" exec -T postgres \
        psql -U "$PG_USER" -d "$PG_DB" -c 'select 1' >/dev/null 2>&1; then
    ok "postgres via docker compose (task results live there)"
else
    fail "cannot query postgres through docker compose - is the stack up? ($REPO_ROOT/$COMPOSE_FILE)"
fi

if [ "$USE_TMUX" = 1 ]; then
    if command -v tmux >/dev/null 2>&1; then
        ok "tmux: $(command -v tmux)"
    else
        fail "tmux not installed (use --no-tmux)"
    fi
fi

if [ "$FAILED" != 0 ]; then
    printf '\nStopped before running anything. Fix the FAIL lines above and re-run.\n' >&2
    exit 2
fi

if [ "$PRINT_ONLY" = 1 ]; then
    step "would run"
    echo "repo root : $REPO_ROOT"
    echo "harness   : $HARNESS ${HARNESS_ARGS[*]+${HARNESS_ARGS[*]}}"
    echo "log       : $LOG"
    echo "session   : $SESSION (tmux=$USE_TMUX, detach=$DETACH)"
    echo "runner    :"
    sed 's/^/   /' "$REPO_RUNNER"
    exit 0
fi

[ "$PREFLIGHT_ONLY" = 1 ] && exit 0

# --------------------------------------------------------------------------- run
if [ "$USE_TMUX" = 0 ]; then
    step "running in this terminal"
    exec "$REPO_RUNNER"
fi

step "running in tmux session $SESSION"
echo "   log       : $LOG"
echo "   leave it  : Ctrl-b d        come back: tmux attach -t $SESSION"
echo
tmux new-session -d -s "$SESSION" -c "$REPO_ROOT" "$REPO_RUNNER" || {
    echo "e2e-remote: could not create tmux session $SESSION" >&2
    exit 2
}
if [ "$DETACH" = 1 ]; then
    echo "   started detached; attach with: tmux attach -t $SESSION"
    exit 0
fi

tmux attach -t "$SESSION"
if [ -f "$STATUS" ]; then
    rc="$(cat "$STATUS")"
    printf '\n   harness exited %s; log: %s\n' "$rc" "$LOG"
    exit "$rc"
fi
printf '\n   still running (you detached); log: %s\n' "$LOG"
exit 0
