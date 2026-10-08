#!/bin/sh
# System links and dependencies: what a desktop environment needs. A package
# links its session file, PAM config and init script into the system, the
# links follow the active version through upgrade and rollback, a file WARP
# did not create is never overwritten, a link outside the integration
# directories is refused, remove takes the links away. Dependencies come from
# the archive manifests through make-index.py into the signed index and are
# installed (or moved to the wanted version) first; a cycle stops cleanly.
# Isolated store and system root, local repo.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
T="$(mktemp -d)"
RP=18784
HTTP=""
cleanup() { [ -n "$HTTP" ] && kill "$HTTP" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }
plain() { sed 's/\x1b\[[0-9;]*m//g'; }
if command -v ss >/dev/null && ss -ltn | grep -q ":$RP "; then fail "port $RP busy"; fi

mkdir -p "$T/build" "$T/store" "$T/root"
cp -r "$ROOT/src" "$ROOT/Makefile" "$T/build/"
rm -f "$T/build/src/"*.o "$T/build/warp"
( cd "$T/build" && make -s CFLAGS="-O2 -std=c11 -D_GNU_SOURCE -w -DWARP_STORE_DIR='\"$T/store\"' -DWARP_SYSROOT='\"$T/root\"'" >/dev/null )
W="$T/build/warp"; [ -x "$W" ] || fail "build"
TAG="$("$W" archive-tag)"
R="$T/root"

# pkg <name> <version> <manifest-extra-json> <file>...   (files get their path as content)
mkdir -p "$T/repo"
pkg() {
    pn="$1"; v="$2"; extra="$3"; shift 3
    d="$T/pk-$pn-$v"; rm -rf "$d"; mkdir -p "$d/files"
    for f in "$@"; do mkdir -p "$d/files/$(dirname "$f")"; printf '%s %s %s\n' "$pn" "$v" "$f" > "$d/files/$f"; done
    printf '{"name": "%s", "version": "%s"%s}\n' "$pn" "$v" "$extra" > "$d/manifest.json"
    tar -C "$d" -cf - manifest.json files | gzip -n > "$T/repo/$pn-$v-$TAG.warp"
}
pkg lib 1.0 ', "install_links": ["share/foo/lib.txt"], "triggers": ["ldconfig"]' share/foo/lib.txt
pkg lib 2.0 ', "install_links": ["share/foo/lib.txt"], "triggers": ["ldconfig"]' share/foo/lib.txt
pkg de 1.0 ', "deps": [{"name": "lib", "version": "1.0"}], "triggers": ["dbus-reload", "no-such-trigger"], "install_links": ["share/xsessions/de.desktop", "etc/pam.d/de", {"from": "lib/de/de.initd", "to": "/etc/init.d/de"}, {"from": "bin/de", "to": "/usr/bin/de"}, {"from": "share/de/x", "to": "/usr/share/../../etc/shadow"}, "share/xsessions/other.desktop"]' \
    share/xsessions/de.desktop etc/pam.d/de lib/de/de.initd bin/de share/de/x share/xsessions/other.desktop
pkg de 2.0 ', "deps": [{"name": "lib", "version": "2.0"}], "install_links": ["share/xsessions/de.desktop", {"from": "lib/de/de.initd", "to": "/etc/init.d/de"}]' \
    share/xsessions/de.desktop lib/de/de.initd
pkg cyca 1.0 ', "deps": [{"name": "cycb", "version": "1.0"}]' share/a
pkg cycb 1.0 ', "deps": [{"name": "cyca", "version": "1.0"}]' share/b
pkg lonely 1.0 ', "deps": [{"name": "ghost", "version": "9.9"}]' share/l

"$W" keygen "$T/priv.hex" "$T/pub.hex" >/dev/null
serve() { python3 -m http.server "$RP" --bind 127.0.0.1 --directory "$T/repo" >"$T/http.log" 2>&1 & HTTP=$!; until curl -s -o /dev/null "http://127.0.0.1:$RP/"; do n=$((${n:-0}+1)); [ "$n" -gt 100 ] && { echo "FAIL: http server did not start" >&2; exit 1; }; sleep 0.2; done; n=0; }
python3 "$ROOT/tools/make-index.py" "$T/repo" --base-url "http://127.0.0.1:$RP" --key "$T/priv.hex" --warp "$W" --keep-old-versions --no-deltas >/dev/null
serve
"$W" repo add lab "http://127.0.0.1:$RP" --pubkey "$(tr -d '\n' < "$T/pub.hex")" >/dev/null
"$W" repo disable k1os >/dev/null 2>&1 || true
"$W" update >/dev/null || fail "update"

# 1. deps travel from the manifest into the signed index
python3 - "$T/repo/index.json" <<'PY' || fail "deps in index"
import json, sys
p = json.load(open(sys.argv[1]))["packages"]
assert p["de"]["deps"] == [{"name": "lib", "version": "2.0"}], p["de"].get("deps")
assert p["de@1.0"]["deps"] == [{"name": "lib", "version": "1.0"}], p["de@1.0"].get("deps")
assert "deps" not in p["lib"]
PY
ok "make-index takes deps from each archive's manifest"

# 2. a file the system owns is never overwritten
mkdir -p "$R/usr/share/xsessions"; echo system > "$R/usr/share/xsessions/other.desktop"

# 3. de 1.0: its dependency lib 1.0 comes first, links appear, bad ones are refused
out=$("$W" install de@1.0 2>&1 | plain) || fail "install de@1.0: $out"
[ "$(cat "$T/store/active/lib/files/share/foo/lib.txt")" = "lib 1.0 share/foo/lib.txt" ] || fail "lib 1.0 not active"
echo "$out" | grep -q "de needs lib 1.0" || fail "no dependency message: $out"
[ "$(cat "$R/usr/share/foo/lib.txt")" = "lib 1.0 share/foo/lib.txt" ] || fail "lib link"
[ -L "$R/usr/share/xsessions/de.desktop" ] && [ "$(cat "$R/usr/share/xsessions/de.desktop")" = "de 1.0 share/xsessions/de.desktop" ] || fail "session link"
[ "$(cat "$R/etc/pam.d/de")" = "de 1.0 etc/pam.d/de" ] || fail "etc/ shorthand link"
[ "$(cat "$R/etc/init.d/de")" = "de 1.0 lib/de/de.initd" ] || fail "from/to link"
[ ! -e "$R/usr/bin/de" ] || fail "/usr/bin is not an integration directory, link must be refused"
[ ! -e "$R/etc/shadow" ] || fail "a .. path must be refused"
echo "$out" | grep -q "outside the allowed" || fail "no refusal message: $out"
[ "$(cat "$R/usr/share/xsessions/other.desktop")" = "system" ] && [ ! -L "$R/usr/share/xsessions/other.desktop" ] || fail "system file overwritten"
echo "$out" | grep -q "other.desktop already exists and is not WARP's" || fail "no conflict message: $out"
echo "$out" | grep -q "Unknown trigger 'no-such-trigger'" || fail "unknown trigger not reported: $out"
echo "$out" | grep -q "Trigger dbus-reload skipped" || fail "triggers must not run against a scratch root: $out"
"$W" links de | plain | grep -q "/etc/init.d/de" || fail "warp links"
ok "deps first; session, PAM and init links; /usr/bin, .. and a system file refused; triggers named only"

# 4. upgrade to de 2.0 moves lib to 2.0 and replaces the link set (PAM link gone)
"$W" unpin de >/dev/null
"$W" upgrade de >/dev/null 2>&1 || fail "upgrade de"
[ "$(cat "$R/usr/share/xsessions/de.desktop")" = "de 2.0 share/xsessions/de.desktop" ] || fail "session link not on 2.0"
[ ! -e "$R/etc/pam.d/de" ] && [ ! -L "$R/etc/pam.d/de" ] || fail "link dropped by 2.0 still there"
[ "$(cat "$R/usr/share/foo/lib.txt")" = "lib 2.0 share/foo/lib.txt" ] || fail "dependency not moved to lib 2.0"
ok "upgrade: dependency moved to the wanted version, links follow the new version"

# 5. rollback brings the 1.0 links back
"$W" rollback de >/dev/null 2>&1 || fail "rollback"
[ "$(cat "$R/etc/pam.d/de")" = "de 1.0 etc/pam.d/de" ] || fail "rollback did not restore the PAM link"
[ "$(cat "$R/usr/share/xsessions/de.desktop")" = "de 1.0 share/xsessions/de.desktop" ] || fail "rollback session link"
ok "rollback restores the old version's links"

# 6. remove takes WARP's links away and nothing else
"$W" remove de >/dev/null 2>&1 || fail "remove"
for f in usr/share/xsessions/de.desktop etc/pam.d/de etc/init.d/de; do
    [ ! -e "$R/$f" ] && [ ! -L "$R/$f" ] || fail "$f left after remove"
done
[ "$(cat "$R/usr/share/xsessions/other.desktop")" = "system" ] || fail "remove touched a system file"
[ -L "$R/usr/share/foo/lib.txt" ] || fail "remove of de must not touch lib's links"
ok "remove: the package's links are gone, the system file and other packages' links stay"

# 7. a link replaced by hand is not ours any more: removing lib leaves it
rm "$R/usr/share/foo/lib.txt"; echo mine > "$R/usr/share/foo/lib.txt"
"$W" remove lib >/dev/null 2>&1 || fail "remove lib"
[ "$(cat "$R/usr/share/foo/lib.txt")" = "mine" ] || fail "remove deleted a file the user put there"
ok "a link replaced by hand survives remove"

# 8. missing dependency and a cycle fail cleanly
if "$W" install lonely >"$T/out" 2>&1; then fail "a missing dependency must fail"; fi; out=$(plain < "$T/out")
echo "$out" | grep -q "ghost" || fail "missing dependency not named: $out"
[ ! -e "$T/store/active/lonely" ] || fail "lonely installed without its dependency"
if "$W" install cyca >"$T/out" 2>&1; then fail "a cycle must fail"; fi; out=$(plain < "$T/out")
echo "$out" | grep -q "cycle" || fail "cycle not reported: $out"
ok "missing dependency and dependency cycle are refused"

echo "all links/deps tests passed"
