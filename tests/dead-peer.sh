#!/bin/sh
# A peer that never answers must cost seconds, not minutes: the install falls back to the
# repository after the short peer timeouts and still verifies the archive.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
T="$(mktemp -d)"
RP=18791
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
if command -v ss >/dev/null && ss -ltn | grep -q ":$RP "; then fail "port $RP busy"; fi

mkdir -p "$T/build" "$T/store" "$T/repo" "$T/pk/files/share"
cp -r "$ROOT/src" "$ROOT/Makefile" "$T/build/"
rm -f "$T/build/src/"*.o "$T/build/warp"
( cd "$T/build" && make -s CFLAGS="-O2 -std=c11 -D_GNU_SOURCE -w -DWARP_STORE_DIR='\"$T/store\"' -DWARP_BIN_DIR='\"$T/bin\"'" >/dev/null )
W="$T/build/warp"
TAG="$("$W" archive-tag)"
printf '{"name": "tool", "version": "1.0", "install_bins": []}\n' > "$T/pk/manifest.json"
head -c 30000 /dev/urandom > "$T/pk/files/share/payload"
tar -C "$T/pk" -cf - manifest.json files | gzip -n > "$T/repo/tool-1.0-$TAG.warp"
# 10.255.255.1 is not routable here: connecting to it hangs until the client gives up
printf '{"updated": "now", "peers": ["http://10.255.255.1:7777", "http://10.255.255.2:7777"]}\n' > "$T/repo/peers.json"
"$W" keygen "$T/priv.hex" "$T/pub.hex" >/dev/null
python3 "$ROOT/tools/make-index.py" "$T/repo" --base-url "http://127.0.0.1:$RP" --key "$T/priv.hex" --warp "$W" \
    --peer-list-url "http://127.0.0.1:$RP/peers.json" >/dev/null
python3 -m http.server "$RP" --bind 127.0.0.1 --directory "$T/repo" >"$T/http.log" 2>&1 & PIDS="$PIDS $!"
n=0; until curl -s -o /dev/null "http://127.0.0.1:$RP/"; do n=$((n+1)); [ "$n" -gt 100 ] && fail "http server did not start"; sleep 0.2; done

"$W" repo add lab "http://127.0.0.1:$RP" --pubkey "$(tr -d '\n' < "$T/pub.hex")" >/dev/null
"$W" repo disable k1os >/dev/null 2>&1 || true
"$W" update >/dev/null || fail "update"

start=$(date +%s)
"$W" install tool >"$T/out.log" 2>&1 || { cat "$T/out.log"; fail "install with dead peers"; }
took=$(( $(date +%s) - start ))
[ "$took" -le 30 ] || fail "dead peers cost $took s"
[ -e "$T/store/active/tool" ] || fail "tool not installed"
grep -q "Peer 1/" "$T/out.log" || fail "the peers were never tried (the test proves nothing)"
echo "ok: two dead peers cost $took s and the install still verified and finished"
echo "PASS: dead-peer"
