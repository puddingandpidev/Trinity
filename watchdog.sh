#!/usr/bin/env bash
#
# watchdog.sh — Watches dbg-compose.req for commands and executes them via dbg-compose.sh
#
# Usage: ./watchdog.sh
#   - Monitors dbg-compose.req for content changes
#   - When a valid dbg-compose.sh command is detected, executes it
#   - Logs activity to watchdog.log
#   - Handles concurrent writes by checking file modification time
#
# Valid commands (written to dbg-compose.req):
#   full-reup, dbg-reup, yeet, base [args], dbg [args], or any docker compose command
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REQ_FILE="${SCRIPT_DIR}/dbg-compose.req"
LOG_FILE="${SCRIPT_DIR}/watchdog.log"
DBG_COMPOSE="${SCRIPT_DIR}/dbg-compose.sh"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$LOG_FILE"
}

# Validate command against known valid dbg-compose.sh commands
is_valid_command() {
    local cmd="$1"
    # Known top-level commands
    if [[ "$cmd" == "full-reup" || "$cmd" == "dbg-reup" || "$cmd" == "yeet" ]]; then
        return 0
    fi
    # Pass-through commands (base, dbg, or any other docker compose command)
    # Allow alphanumeric, hyphens, slashes, and spaces for docker compose subcommands
    if [[ "$cmd" =~ ^[a-zA-Z0-9 _/-]+$ ]]; then
        return 0
    fi
    return 1
}

log "Watchdog started. Monitoring ${REQ_FILE}"
log "dbg-compose.sh: ${DBG_COMPOSE}"

# Track last modification time to detect changes
last_mtime=0

while true; do
    if [[ -f "$REQ_FILE" ]]; then
        current_mtime=$(stat -c %Y "$REQ_FILE" 2>/dev/null || stat -f %m "$REQ_FILE" 2>/dev/null)

        if [[ "$current_mtime" -ne "$last_mtime" ]]; then
            log "Detected change in dbg-compose.req (mtime: $current_mtime)"

            # Read the command
            cmd=$(cat "$REQ_FILE" 2>/dev/null | tr -d '\n\r' | xargs)

            if [[ -n "$cmd" ]]; then
                log "Command received: '$cmd'"

                if is_valid_command "$cmd"; then
                    log "Executing: ${DBG_COMPOSE} $cmd"

                    # Execute the command, capture output and exit code
                    output=$(bash "$DBG_COMPOSE" $cmd 2>&1)
                    exit_code=$?

                    if [[ $exit_code -eq 0 ]]; then
                        log "Command succeeded"
                    else
                        log "Command failed with exit code $exit_code"
                    fi

                    # Log output (truncate if too long)
                    if [[ ${#output} -gt 10000 ]]; then
                        log "Output (truncated): ${output:0:10000}..."
                    else
                        log "Output: $output"
                    fi

                    # Clear the request file after processing
                    echo "" > "$REQ_FILE"
                else
                    log "Invalid command: '$cmd'"
                fi
            else
                log "Empty command in dbg-compose.req"
            fi

            last_mtime="$current_mtime"
        fi
    fi

    sleep 1
done
