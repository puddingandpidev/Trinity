#!/usr/bin/env bash
# ========================================== { START OF FILE } ========================================== #
# dbg-compose.sh — docker compose with the files you meant, in a tree where the plain command is wrong.
#
# Three compose files are tracked, and `docker compose` prefers the first name it finds:
#   compose.yaml              the onion stack (tor, socks-proxy, weather-api, redirector). Docker Compose
#                             picks this one for a bare `docker compose ...`, so the bare command builds
#                             the onion stack — never the C2 stack — and its weather-api context
#                             (./C2Server/WeatherForecastApi) is not in this repository.
#   docker-compose.yml        the Trinity C2 stack (postgres, migrator, teamserver, client).
#   docker-compose.debug.yml  the debug override: adds teamserver-debug on :5002 (vsdbg, SYS_PTRACE,
#                             `tail -f /dev/null` entrypoint so it idles until the debugger attaches).
#                             Needs docker-compose.yml alongside it; see README.
#
# Usage (from anywhere; paths resolve against this script's directory):
#   ./dbg-compose.sh                      # rebuild C2 stack + debug container (same as `rebuild`)
#   ./dbg-compose.sh rebuild              # (alias: full-reup)
#   ./dbg-compose.sh debug-rebuild        # rebuild only teamserver-debug (alias: dbg-reup)
#   ./dbg-compose.sh remove               # force-remove the trinity containers (alias: yeet)
#   ./dbg-compose.sh base <compose args>  # C2 stack only        (docker-compose.yml)
#   ./dbg-compose.sh dbg  <compose args>  # C2 stack + debug     (both files)
#   ./dbg-compose.sh up   <compose args>  # debug stack: `up -d <args>`
#   ./dbg-compose.sh <compose args>       # anything else goes to the debug stack's docker compose
#   ./dbg-compose.sh help                 # this text
#
# The exit status is the status of the docker command that ran, including the no-argument rebuild.
# ========================================== { END OF FILE } ========================================== #

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_FILE="${SCRIPT_DIR}/docker-compose.yml"
DEBUG_FILE="${SCRIPT_DIR}/docker-compose.debug.yml"

base() { docker compose -f "$BASE_FILE" "$@"; }
debug() { docker compose -f "$BASE_FILE" -f "$DEBUG_FILE" "$@"; }

usage() {
    printf '%s\n' \
        "usage: $(basename "$0") [rebuild | debug-rebuild | remove | base <args> | dbg <args> | up <args> | <compose args>]" \
        "" \
        "  (no args)      rebuild the C2 stack and the debug container" \
        "  rebuild        same as no args                (alias: full-reup)" \
        "  debug-rebuild  rebuild only teamserver-debug  (alias: dbg-reup)" \
        "  remove         force-remove the trinity containers (alias: yeet)" \
        "  base <args>    docker compose -f docker-compose.yml <args>" \
        "  dbg <args>     docker compose -f docker-compose.yml -f docker-compose.debug.yml <args>" \
        "  up <args>      the debug stack: up -d <args>" \
        "  <other>        forwarded to the debug stack's docker compose" \
        "" \
        "A bare 'docker compose' would use compose.yaml (the onion stack), not the C2 stack."
}

# No arguments means "rebuild everything", which is the common case after a code change.
if [ "$#" -eq 0 ]; then
    set -- rebuild
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        help | -h | --help)
            usage
            exit 0
            ;;
        rebuild | full-reup)
            shift
            echo "+ base up -d --build && debug up -d --no-deps --build teamserver-debug"
            base up -d --build || exit $?
            debug up -d --no-deps --build teamserver-debug
            exit $?
            ;;
        debug-rebuild | dbg-reup)
            shift
            echo "+ debug up -d --build teamserver-debug"
            debug up -d --build teamserver-debug
            exit $?
            ;;
        remove | yeet)
            shift
            containers="$(docker ps -aq --filter 'name=trinity' 2>/dev/null || true)"
            if [ -n "$containers" ]; then
                # shellcheck disable=SC2086 # the ids are docker's own output, one per line
                docker rm -f $containers || exit $?
            fi
            exit 0
            ;;
        base)
            shift
            base "$@"
            exit $?
            ;;
        dbg)
            shift
            debug "$@"
            exit $?
            ;;
        up)
            shift
            debug up -d "$@"
            exit $?
            ;;
        *)
            debug "$@"
            exit $?
            ;;
    esac
done
