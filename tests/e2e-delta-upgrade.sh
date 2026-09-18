#!/bin/sh
# End-to-end: a custom repository served over HTTP, signed with a throwaway
# key; install the old version, then `warp upgrade` must fetch the delta
# (not the full archive) and end up with the new version, bit-for-bit.
#
# Runs warp inside the K1OS container (needs Docker + host networking) so
# the store lives in a throwaway root filesystem. WARP=<binary> to test.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
WARP="${WARP:-$HERE/../warp}"
IMAGE="${IMAGE:-ghcr.io/keytron/k1os:latest}"
PORT="${PORT:-8765}"
# E2E_TMPDIR: on the lab runner (itself a container talking to the host's
# Docker) this must be a path that exists identically on host and runner, or
# the bind mounts below point at nothing.
T="$(mktemp -d -p "${E2E_TMPDIR:-${TMPDIR:-/tmp}}")"
# The store is written by root inside the container; clean it up the same way.
cleanup() {
    kill $HTTP 2>/dev/null || true
    docker run --rm -v "$T:$T" "$IMAGE" /bin/sh -c "rm -rf $T/store" 2>/dev/null || true
    rm -rf "$T"
}
trap cleanup EXIT
HTTP=

# Two versions of a synthetic package: 2 MiB payload, second differs a little.
mk() { # mk <version> <dir>
    mkdir -p "$2/files/bin"
    printf '{"name": "blob", "version": "%s", "install_bins": ["bin/blob"]}\n' "$1" > "$2/manifest.json"
    tar -C "$2" --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' \
        -I 'gzip -n --rsyncable' -cf "$T/repo/blob-$1-x86_64.warp" manifest.json files
}
mkdir -p "$T/repo" "$T/v1/files/bin" "$T/v2/files/bin"
head -c 2000000 /dev/urandom > "$T/payload"
{ printf '#!/bin/sh\necho blob 1.0\n'; cat "$T/payload"; } > "$T/v1/files/bin/blob"
{ printf '#!/bin/sh\necho blob 1.1\n'; head -c 1500000 "$T/payload"; head -c 50000 /dev/urandom; tail -c +1500001 "$T/payload"; } > "$T/v2/files/bin/blob"
chmod +x "$T/v1/files/bin/blob" "$T/v2/files/bin/blob"
mk 1.0 "$T/v1"
mk 1.1 "$T/v2"

# Throwaway signing key for this repository.
"$WARP" keygen "$T/priv.hex" "$T/pub.hex" > /dev/null
PUB="$(tr -d '\n' < "$T/pub.hex")"
BASE="http://127.0.0.1:$PORT"

# Stage 1: only 1.0 published.
mv "$T/repo/blob-1.1-x86_64.warp" "$T/hold.warp"
python3 "$HERE/../tools/make-index.py" "$T/repo" --base-url "$BASE" --key "$T/priv.hex" --warp "$WARP" > /dev/null
if command -v ss >/dev/null && ss -ltn | grep -q ":$PORT "; then
    echo "port $PORT is already in use (stale http.server?) — set PORT=... or kill it" >&2; exit 1
fi
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$T/repo" > "$T/http.log" 2>&1 &
HTTP=$!
sleep 1
kill -0 "$HTTP" 2>/dev/null || { echo "http.server did not start"; cat "$T/http.log"; exit 1; }

# Everything the container needs lives under $T, so one bind mount covers it.
install -m755 "$WARP" "$T/warp"
run() { docker run --rm --network host -v "$T/warp:/usr/bin/warp:ro" -v "$T:$T:ro" \
          -v "$T/store:/var/lib/warp" "$IMAGE" /bin/sh -lc "$*" 2>/dev/null; }
mkdir -p "$T/store"
# /usr/local/bin symlinks live in the throwaway container; run the active binary from the store.
BLOB=/var/lib/warp/active/blob/files/bin/blob

# repo/name form picks the repository explicitly.
run "warp repo add lab $BASE --pubkey $PUB >/dev/null && warp update >/dev/null && warp install lab/blob" | grep -E "✓|✗"
run "$BLOB" | grep -q "blob 1.0"
echo "installed 1.0 from the custom repo"

# Stage 2: publish 1.1 with a delta from 1.0.
mv "$T/hold.warp" "$T/repo/blob-1.1-x86_64.warp"
python3 "$HERE/../tools/make-index.py" "$T/repo" --base-url "$BASE" --key "$T/priv.hex" --warp "$WARP" > "$T/index.log"
grep -q '"deltas"' "$T/repo/index.json" || { echo "index has no deltas"; cat "$T/index.log"; exit 1; }
delta_size=$(stat -c %s "$T"/repo/blob-1.0-to-1.1-x86_64.warpdelta)
full_size=$(stat -c %s "$T/repo/blob-1.1-x86_64.warp")
echo "delta $delta_size B vs full $full_size B"
[ "$delta_size" -lt $((full_size / 4)) ]

: > "$T/http.log"
run "warp upgrade" > "$T/upgrade.log" 2>&1 || { cat "$T/upgrade.log"; exit 1; }
grep -q "Archive rebuilt from delta" "$T/upgrade.log" || { echo "delta path not used"; cat "$T/upgrade.log"; exit 1; }
grep -q "Upgraded: blob 1.1" "$T/upgrade.log"
# The web server must have handed out the delta and not the full 1.1 archive.
grep -q "warpdelta" "$T/http.log"
if grep -q "blob-1.1-x86_64.warp " "$T/http.log"; then echo "full archive was downloaded"; cat "$T/http.log"; exit 1; fi
run "$BLOB" | grep -q "blob 1.1"
run "warp rollback blob >/dev/null && $BLOB" | grep -q "blob 1.0"
echo "upgrade via delta ok, rollback ok"
echo "PASS"
