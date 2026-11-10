#!/usr/bin/env bash
# Regenerate the C# Refit client from a running TeamServer's OpenAPI document.
#
# Why this exists
# ---------------
# The Blazor client used to talk to the TeamServer through ten hand-written client
# classes. Their URLs were written by hand, so a moved route was only discovered at
# runtime: `/api/v1/payloads` (gone; the route is `/generate`), `/api/v1/tasks`
# (gone; the route is `/tasks/tasks`) and `/api/v1/listeners` (gone; the create
# route is `/listeners/http`) all answered 404 from a page that "worked". Refitter
# generates the interface and contracts from the document the server actually
# serves, so a route cannot drift from its caller.
#
# Prerequisites the document must satisfy
# ---------------------------------------
# An operation without a 2xx response *schema* generates a bodiless method, which
# is useless to a caller. The controllers therefore carry
# `[ProducesResponseType(typeof(Dto), StatusCodes.Status200OK)]`; keep adding them
# when you add an action (the mobile client needed the same patch — see
# trinity-mobile-client/docs/openapi-and-conformance.md).
#
#   scripts/gen-api-client.sh                              # live stack on :5000
#   TRINITY_API_URL=http://192.168.122.20:5000 scripts/gen-api-client.sh
#
# Install the generator once: dotnet tool install --global Refitter
#
# The output is written to Client/Api/Generated and is not wired into the build by
# default: adopting it is a review step (see docs/api-client.md), not something a
# regeneration should do behind an operator's back.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
BASE_URL="${1:-${TRINITY_API_URL:-http://localhost:5000}}"
BASE_URL="${BASE_URL%/}"
SPEC_URL="$BASE_URL/swagger/v1/swagger.json"

GEN_DIR="$REPO_ROOT/Client/Api/Generated"
SPEC_FILE="$REPO_ROOT/scripts/.openapi/trinity-csharp.json"
MANIFEST="$REPO_ROOT/scripts/.openapi/trinity-csharp.manifest.json"
NAMESPACE="${TRINITY_API_NAMESPACE:-Trinity.Client.Api}"
OUTPUT="$GEN_DIR/TrinityApi.cs"

log() { printf '[gen-api-client] %s\n' "$*"; }
die() { printf '[gen-api-client] error: %s\n' "$*" >&2; exit 1; }

command -v curl >/dev/null || die "curl is required"
command -v python3 >/dev/null || die "python3 is required"
command -v refitter >/dev/null || die "refitter is required: dotnet tool install --global Refitter"

log "fetching $SPEC_URL"
mkdir -p "$(dirname "$SPEC_FILE")"
attempt=0
until curl -fsS -o "$SPEC_FILE.tmp" "$SPEC_URL"; do
    attempt=$((attempt + 1))
    [ "$attempt" -ge 10 ] && die "no OpenAPI document at $SPEC_URL.
       Is the stack up?  docker compose -f docker-compose.yml up -d
       The TeamServer serves swagger only in Development (ASPNETCORE_ENVIRONMENT=Development),
       and the URL must be the API port (5000), not an agent listener port (5069)."
    log "waiting for the server ($attempt/10)"
    sleep 2
done
mv "$SPEC_FILE.tmp" "$SPEC_FILE"

python3 - "$SPEC_FILE" <<'PY' || die "the document at $SPEC_URL is not valid OpenAPI JSON"
import json, sys
doc = json.load(open(sys.argv[1]))
paths = doc.get("paths") or {}
if not paths:
    raise SystemExit("document has no paths")
ops = sum(len([v for v in item if v in ("get", "post", "put", "delete", "patch")]) for item in paths.values())
with_schema = 0
for item in paths.values():
    for verb, op in item.items():
        if verb not in ("get", "post", "put", "delete", "patch"):
            continue
        if any(str(code).startswith("2") and resp.get("content", {}).get("application/json", {}).get("schema")
               for code, resp in op.get("responses", {}).items()):
            with_schema += 1
print(f"[gen-api-client] spec ok: {len(paths)} paths, {ops} operations, "
      f"{with_schema} declare a response schema")
if with_schema * 2 < ops:
    print("[gen-api-client] warning: most operations declare no response schema; "
          "generated methods for them will be bodiless (add [ProducesResponseType])")
PY

SPEC_HASH="$(sha256sum "$SPEC_FILE" | cut -c1-16)"
log "spec $SPEC_HASH; generating $OUTPUT (namespace $NAMESPACE)"
mkdir -p "$GEN_DIR"
refitter "$SPEC_FILE" \
    --namespace "$NAMESPACE" \
    --output "$OUTPUT" \
    --cancellation-tokens \
    --no-operation-headers

WORDS="$(wc -l < "$OUTPUT")"
python3 - "$MANIFEST" "$SPEC_HASH" "$BASE_URL" "$WORDS" <<'PY'
import datetime, json, sys
manifest_path, spec_hash, base_url, lines = sys.argv[1:5]
try:
    manifest = json.load(open(manifest_path))
except Exception:
    manifest = {"runs": []}
manifest["runs"].append({
    "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    "specSha256_16": spec_hash,
    "sourceUrl": base_url,
    "generator": "refitter",
    "outputLines": int(lines),
})
manifest["runs"] = manifest["runs"][-10:]
json.dump(manifest, open(manifest_path, "w"), indent=2)
print(f"[gen-api-client] recorded run in {manifest_path}")
PY

log "done: $OUTPUT ($WORDS lines). Adopting it is a review step: see docs/api-client.md."
