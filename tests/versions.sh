#!/bin/sh
# Versions side by side: install any published version, switch between the
# installed ones without a download, pin, walk back through the history, run a
# version without switching, and garbage-collect. Isolated store, local repo.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
T="$(mktemp -d)"
RP=18783
HTTP=""
cleanup() { [ -n "$HTTP" ] && kill "$HTTP" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }
plain() { sed 's/\x1b\[[0-9;]*m//g'; }
if command -v ss >/dev/null && ss -ltn | grep -q ":$RP "; then fail "port $RP busy"; fi

mkdir -p "$T/build" "$T/store"
cp -r "$ROOT/src" "$ROOT/Makefile" "$T/build/"
rm -f "$T/build/src/"*.o "$T/build/warp"
( cd "$T/build" && make -s CFLAGS="-O2 -std=c11 -D_GNU_SOURCE -w -DWARP_STORE_DIR='\"$T/store\"'" >/dev/null )
W="$T/build/warp"; [ -x "$W" ] || fail "build"
TAG="$("$W" archive-tag)"

# tool 1.0, 1.1, 2.0: a script that prints its version, plus a payload so gc frees something
mkdir -p "$T/repo"
for v in 1.0 1.1 2.0; do
    d="$T/pk-$v"; mkdir -p "$d/files/bin" "$d/files/share"
    printf '{"name": "tool", "version": "%s", "install_bins": ["bin/warptestver"]}\n' "$v" > "$d/manifest.json"
    printf '#!/bin/sh\necho "tool %s $*"\n' "$v" > "$d/files/bin/warptestver"; chmod +x "$d/files/bin/warptestver"
    printf '%s\n' "$v" > "$d/files/share/ver"
    head -c 200000 /dev/urandom > "$d/files/share/payload"
    tar -C "$d" -I 'gzip -n' -cf "$T/repo/tool-$v-$TAG.warp" manifest.json files
done
"$W" keygen "$T/priv.hex" "$T/pub.hex" >/dev/null
serve() { python3 -m http.server "$RP" --bind 127.0.0.1 --directory "$T/repo" >"$T/http.log" 2>&1 & HTTP=$!; sleep 1; }
python3 "$ROOT/tools/make-index.py" "$T/repo" --base-url "http://127.0.0.1:$RP" --key "$T/priv.hex" --warp "$W" --keep-old-versions --no-deltas >/dev/null
serve
"$W" repo add lab "http://127.0.0.1:$RP" --pubkey "$(tr -d '\n' < "$T/pub.hex")" >/dev/null
"$W" repo disable k1os >/dev/null 2>&1 || true
"$W" update >/dev/null || fail "update"

active() { cat "$T/store/active/tool/files/share/ver"; }
stored() { ls "$T/store/store" | grep -c '^tool-' || true; }

# 1. index: plain key = latest, name@version for every older release
python3 - "$T/repo/index.json" <<'PY' || fail "index keys"
import json, sys
p = json.load(open(sys.argv[1]))["packages"]
assert sorted(p) == ["tool", "tool@1.0", "tool@1.1"], sorted(p)   # the latest is the plain name, no duplicate
assert p["tool"]["version"] == "2.0" and p["tool@1.0"]["version"] == "1.0"
PY
n=$("$W" search tool 2>&1 | plain | grep -c "^  tool ") ; [ "$n" -eq 1 ] || fail "search lists $n entries, expected 1"
ok "index: latest under the plain name, older releases as name@version; search shows one line"

# 2. latest, then an older one on purpose: it is pinned
"$W" install tool >/dev/null 2>&1 || fail "install latest"
[ "$(active)" = "2.0" ] || fail "latest should be 2.0"
out=$("$W" install tool@1.0 2>&1 | plain) || fail "install tool@1.0"
[ "$(active)" = "1.0" ] || fail "tool@1.0 not active"
echo "$out" | grep -q "Pinned at 1.0" || fail "no pin message: $out"
[ "$(stored)" -eq 2 ] || fail "both versions should be in the store, got $(stored)"
ok "install tool@1.0 puts it next to 2.0, activates it and pins it"

# 3. upgrade respects the pin
"$W" upgrade 2>&1 | plain | grep -q "pinned at 1.0" || fail "upgrade ignored the pin"
[ "$(active)" = "1.0" ] || fail "upgrade moved a pinned package"
"$W" list | plain | grep -q "\[pinned\]" || fail "list does not show the pin"
ok "upgrade leaves a pinned package alone; list shows it"

# 4. unpin lets upgrade move on
"$W" unpin tool >/dev/null
"$W" upgrade >/dev/null 2>&1 || fail "upgrade after unpin"
[ "$(active)" = "2.0" ] || fail "upgrade after unpin should reach 2.0, got $(active)"
ok "unpin, then upgrade reaches 2.0"

# 5. offline: switch and install of a stored version need no network
kill "$HTTP"; HTTP=""; sleep 1
"$W" install tool@1.1 >/dev/null 2>&1 && fail "1.1 was never fetched; offline install must fail"
serve
"$W" install tool@1.1 >/dev/null 2>&1 || fail "install tool@1.1"
kill "$HTTP"; HTTP=""; sleep 1
"$W" switch tool 2.0 >/dev/null 2>&1 || fail "offline switch"
[ "$(active)" = "2.0" ] || fail "switch to 2.0"
"$W" install tool@1.0 2>&1 | plain | grep -q "already in the store" || fail "install of a stored version should not download"
[ "$(active)" = "1.0" ] || fail "offline install@1.0"
ok "switch and install of a stored version work with the network down"

# 6. rollback walks back through the history, one version per call
# history by now: 2.0, 1.0, 2.0 (upgrade), 1.1, 2.0 (switch), 1.0 (install from the store)
for want in 2.0 1.1 2.0 1.0 2.0; do
    "$W" rollback tool >/dev/null 2>&1 || fail "rollback before $want"
    [ "$(active)" = "$want" ] || fail "rollback gave $(active), expected $want"
done
"$W" rollback tool >/dev/null 2>&1 && fail "rollback past the beginning must fail"
ok "rollback goes back through the whole activation history"

# 7. run a version without switching
[ "$("$W" run tool@1.1 a b | tr -d '\n')" = "tool 1.1 a b" ] || fail "run tool@1.1"
[ "$("$W" run tool | sed 's/ *$//')" = "tool 2.0" ] || fail "run active"
[ "$(active)" = "2.0" ] || fail "run must not switch"
"$W" run tool@9.9 >/dev/null 2>&1 && fail "run of a missing version must fail"
ok "run starts any stored version without touching the active one"

# 8. versions
out=$("$W" versions tool | plain)
echo "$out" | grep -q "2.0 .* active" || fail "versions: active not marked"
echo "$out" | grep -q "1.1" && echo "$out" | grep -q "1.0" || fail "versions: installed list"
echo "$out" | grep -q "Published" || fail "versions: no published list"
ok "versions lists what is installed and what is published"

# 9. pin follows an explicit choice
"$W" pin tool 1.1 >/dev/null 2>&1 || fail "pin tool 1.1"
[ "$(active)" = "1.1" ] || fail "pin should switch"
"$W" switch tool 1.0 >/dev/null 2>&1; [ "$(cat "$T/store/pins/tool")" = "1.0" ] || fail "pin did not follow the switch"
"$W" switch tool 2.0 >/dev/null 2>&1
"$W" unpin tool >/dev/null
ok "pin <ver> switches and pins; a pin follows switch"

# 10. gc: dry run changes nothing, the real run keeps active/pinned/recent only
"$W" switch tool 2.0 >/dev/null 2>&1
"$W" pin tool >/dev/null 2>&1
before=$(stored)
"$W" gc --keep 1 --dry-run | plain | grep -q "would remove" || fail "gc --dry-run reports nothing"
[ "$(stored)" -eq "$before" ] || fail "dry run removed something"
"$W" gc --keep 1 >/dev/null
[ "$(stored)" -eq 1 ] || fail "gc should leave only the active version, got $(stored)"
[ "$(active)" = "2.0" ] || fail "gc touched the active version"
"$W" switch tool 1.0 >/dev/null 2>&1 && fail "1.0 was collected, switch must fail"
ok "gc removes versions that are not active, pinned or recent (dry run first)"

# 11. a collected version comes back with install
serve
"$W" install tool@1.0 >/dev/null 2>&1 || fail "reinstall after gc"
[ "$(active)" = "1.0" ] || fail "reinstalled 1.0"
ok "a collected version can be installed again"

# 12. remove clears the pin and history; gc then frees the leftovers
"$W" remove tool >/dev/null 2>&1
[ ! -e "$T/store/pins/tool" ] && [ ! -e "$T/store/history/tool" ] || fail "remove left a pin or history behind"
"$W" gc >/dev/null
[ "$(stored)" -eq 0 ] || fail "gc should empty the store of tool, got $(stored)"
ok "remove clears pin and history; gc then frees the store"

echo "PASS: versions"
