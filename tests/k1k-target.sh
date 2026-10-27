#!/bin/sh
# K1OS / K1K as a target: `warp --root DIR --platform k1k-x86_64 install` stages the services of a
# K1K disk. The archive tag is k1k_x86_64, make-index.py files the build under k1k-x86_64, the
# package's install_links land in DIR/SVC, DIR/SVC/MANIFEST.TXT is written from the
# k1k_service entries, a Linux-only build is refused, a link outside /SVC or with a name that is
# not FAT 8.3 is refused, remove takes everything back, and nothing is announced to a tracker.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
T="$(mktemp -d)"
RP=18785
HTTP=""
cleanup() { [ -n "$HTTP" ] && kill "$HTTP" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }
plain() { sed 's/\x1b\[[0-9;]*m//g'; }
if command -v ss >/dev/null && ss -ltn | grep -q ":$RP "; then fail "port $RP busy"; fi

mkdir -p "$T/build" "$T/store" "$T/repo" "$T/disk"
cp -r "$ROOT/src" "$ROOT/Makefile" "$T/build/"
rm -f "$T/build/src/"*.o "$T/build/warp"
( cd "$T/build" && make -s CFLAGS="-O2 -std=c11 -D_GNU_SOURCE -w -DWARP_STORE_DIR='\"$T/store\"'" >/dev/null )
W="$T/build/warp"; [ -x "$W" ] || fail "build"

[ "$("$W" --platform k1k-x86_64 platform)" = "k1k-x86_64" ] || fail "platform"
[ "$("$W" --platform=k1k-x86_64 archive-tag)" = "k1k_x86_64" ] || fail "archive tag"
ok "k1k-x86_64 is a platform of its own, tag k1k_x86_64"

# pkg <name> <version> <tag> <manifest-extra> <file>
pkg() {
    d="$T/pk-$1-$2"; rm -rf "$d"; mkdir -p "$d/files/SVC"
    printf 'ELF stand-in %s\n' "$1" > "$d/files/SVC/$5"
    printf '{"name": "%s", "version": "%s"%s}\n' "$1" "$2" "$4" > "$d/manifest.json"
    tar -C "$d" -cf - manifest.json files | gzip -n > "$T/repo/$1-$2-$3.warp"
}
pkg k1k-hello 0.1.0 k1k_x86_64 ',"install_links": [{"from": "SVC/HELLO.ELF", "to": "/SVC/HELLO.ELF"}], "k1k_service": {"name": "hello", "image": "HELLO.ELF", "caps": "fs"}' HELLO.ELF
pkg k1k-long  0.1.0 k1k_x86_64 ',"install_links": [{"from": "SVC/TOOLONGNAME.ELF", "to": "/SVC/TOOLONGNAME.ELF"}]' TOOLONGNAME.ELF
pkg k1k-etc   0.1.0 k1k_x86_64 ',"install_links": [{"from": "SVC/ETC.ELF", "to": "/etc/ETC.ELF"}]' ETC.ELF
pkg linux-only 1.0 x86_64 '' LIN.ELF

"$W" keygen "$T/priv.hex" "$T/pub.hex" >/dev/null
python3 "$ROOT/tools/make-index.py" "$T/repo" --base-url "http://127.0.0.1:$RP" --key "$T/priv.hex" --warp "$W" >/dev/null
grep -q '"k1k-x86_64"' "$T/repo/index.json" || fail "make-index did not file the build under k1k-x86_64"
ok "make-index files the build under k1k-x86_64"

( cd "$T/repo" && exec python3 -m http.server "$RP" --bind 127.0.0.1 >/dev/null 2>&1 ) & HTTP=$!
sleep 1
K="$W --root $T/disk --platform k1k-x86_64"
$K repo add lab "http://127.0.0.1:$RP" --pubkey-file "$T/pub.hex" >/dev/null
$K update >/dev/null 2>&1

$K install lab/k1k-hello 2>&1 | plain | tee "$T/out" | grep -q 'Installed: k1k-hello' || fail "install"
grep -q 'Announced' "$T/out" && fail "a staged install announced itself to a tracker"
[ -f "$T/disk/SVC/HELLO.ELF" ] || fail "HELLO.ELF not in DIR/SVC"
[ "$(cat "$T/disk/SVC/MANIFEST.TXT" | grep -v '^#')" = "hello HELLO.ELF fs" ] || fail "MANIFEST.TXT"
[ -d "$T/disk$T/store/store" ] || fail "store is not under the root"
[ ! -e "$T/store/store" ] || fail "the host store was touched"
ok "install: DIR/SVC/HELLO.ELF and a MANIFEST.TXT line, host store untouched, no announce"

$K install lab/k1k-long 2>&1 | plain | grep -q 'refusing link' || fail "a name that is not 8.3 was accepted"
$K install lab/k1k-etc  2>&1 | plain | grep -q 'refusing link' || fail "a link outside /SVC was accepted"
[ ! -e "$T/disk/etc" ] || fail "something was written outside /SVC"
ok "links outside /SVC and names that are not FAT 8.3 are refused"

$K install lab/linux-only 2>&1 | plain | grep -qi 'no build\|not available\|platform' || fail "a Linux-only package installed on k1k"
ok "a package without a k1k build is not installed"

$K remove k1k-hello >/dev/null 2>&1
[ ! -e "$T/disk/SVC/HELLO.ELF" ] || fail "remove left the image"
[ -z "$(grep -v '^#' "$T/disk/SVC/MANIFEST.TXT")" ] || fail "remove left the manifest line"
ok "remove takes the image and the manifest line away"
echo "k1k-target: all passed"
