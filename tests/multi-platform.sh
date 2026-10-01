#!/bin/sh
# Prebuilt packages per platform: the index carries one build per <os>-<arch>,
# the client takes only the one made for its own OS and CPU, refuses a package
# that has none (nothing is compiled, nothing foreign is installed), and an
# index in the old single-build format still works on linux-x86_64.
# warp is built three times here, each told it is a different platform.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
T="$(mktemp -d)"
RP=18781
PIDS=""
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null || true; done; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
ok()   { echo "ok: $*"; }
plain() { sed 's/\x1b\[[0-9;]*m//g'; }
if command -v ss >/dev/null && ss -ltn | grep -q ":$RP "; then fail "port $RP busy"; fi

# One binary per fake platform, each with its own store.
buildfor() { # buildfor <dir> <os> <arch>
    mkdir -p "$T/$1/build" "$T/$1/store"
    cp -r "$ROOT/src" "$ROOT/Makefile" "$T/$1/build/"
    rm -f "$T/$1/build/src/"*.o "$T/$1/build/warp"
    ( cd "$T/$1/build" && make -s CFLAGS="-O2 -std=c11 -D_GNU_SOURCE -w -DWARP_OS='\"$2\"' -DWARP_ARCH='\"$3\"' \
        -DWARP_STORE_DIR='\"$T/$1/store\"'" >/dev/null )
    [ -x "$T/$1/build/warp" ] || fail "build $1"
}
buildfor lx linux x86_64
buildfor la linux aarch64
buildfor an android aarch64
WX="$T/lx/build/warp"; WA="$T/la/build/warp"; WN="$T/an/build/warp"

# Packages: `tool` for three platforms, `x86only` for linux-x86_64 only, `docs` for any platform.
mkdir -p "$T/repo"
mk() { # mk <name> <tag> <marker>
    d="$T/pk-$1-$2"; mkdir -p "$d/files/share"
    printf '{"name": "%s", "version": "1.0", "install_bins": []}\n' "$1" > "$d/manifest.json"
    printf '%s\n' "$3" > "$d/files/share/marker"
    head -c 40000 /dev/urandom > "$d/files/share/payload-$2"
    tar -C "$d" -I 'gzip -n' -cf "$T/repo/$1-1.0-$2.warp" manifest.json files
}
mk tool x86_64 linux-x86_64
mk tool aarch64 linux-aarch64
mk tool android_aarch64 android-aarch64
mk x86only x86_64 linux-x86_64
mk docs noarch any

cat > "$T/repo/descriptions.json" <<'JSON'
{"tool": {"ru": "Инструмент для проверки", "de": "Werkzeug zum Testen"}}
JSON
"$WX" keygen "$T/priv.hex" "$T/pub.hex" >/dev/null
PUB="$(tr -d '\n' < "$T/pub.hex")"
python3 "$ROOT/tools/make-index.py" "$T/repo" --base-url "http://127.0.0.1:$RP" --key "$T/priv.hex" --warp "$WX" >/dev/null
python3 -m http.server "$RP" --bind 127.0.0.1 --directory "$T/repo" >"$T/http.log" 2>&1 & PIDS="$PIDS $!"
sleep 1

# 1. the index: builds per platform, legacy top level, old format when only x86_64
python3 - "$T/repo/index.json" <<'PY' || fail "index structure"
import json, sys
p = json.load(open(sys.argv[1]))["packages"]
t = p["tool"]
assert sorted(t["builds"]) == ["android-aarch64", "linux-aarch64", "linux-x86_64"], sorted(t["builds"])
assert t["sha256"] == t["builds"]["linux-x86_64"]["sha256"], "top level must stay the linux-x86_64 build"
assert t["url"].endswith("tool-1.0-x86_64.warp")
assert t["builds"]["linux-aarch64"]["url"].endswith("tool-1.0-aarch64.warp")
assert t["builds"]["android-aarch64"]["url"].endswith("tool-1.0-android_aarch64.warp")
assert "builds" not in p["x86only"], "a linux-x86_64 only package keeps the old format"
assert list(p["docs"]["builds"]) == ["any"]
assert "url" not in p["docs"], "no legacy build -> nothing for old clients to mistake for one"
PY
ok "index: one build per platform, linux-x86_64 stays at the top level, old format kept when only x86_64"

setup() { # setup <warp>
    "$1" repo add lab "http://127.0.0.1:$RP" --pubkey "$PUB" >/dev/null
    "$1" repo disable k1os >/dev/null 2>&1 || true
    "$1" update >/dev/null || fail "update with $1"
}
setup "$WX"; setup "$WA"; setup "$WN"

marker() { cat "$T/$1/store/active/$2/files/share/marker"; }

# 2. each client installs its own build
"$WX" install tool >/dev/null 2>&1 || fail "x86_64 install"
"$WA" install tool >/dev/null 2>&1 || fail "aarch64 install"
"$WN" install tool >/dev/null 2>&1 || fail "android install"
[ "$(marker lx tool)" = "linux-x86_64" ]     || fail "linux-x86_64 got $(marker lx tool)"
[ "$(marker la tool)" = "linux-aarch64" ]    || fail "linux-aarch64 got $(marker la tool)"
[ "$(marker an tool)" = "android-aarch64" ]  || fail "android-aarch64 got $(marker an tool)"
ok "linux-x86_64, linux-aarch64 and android-aarch64 each install their own build"

# 3. a package without a build for this platform is refused, nothing is installed
"$WX" install x86only >/dev/null 2>&1 || fail "x86only must install on linux-x86_64"
for w in la an; do
    out=$("$T/$w/build/warp" install x86only 2>&1) && fail "x86only installed on $w"
    echo "$out" | grep -q "no build for" || fail "no explanation on $w: $out"
    echo "$out" | grep -q "published for: linux-x86_64" || fail "no list of available platforms on $w"
    echo "$out" | grep -q "does not compile" || fail "no 'does not compile' on $w"
    [ -e "$T/$w/store/active/x86only" ] && fail "something was installed on $w"
done
ok "a package without a build for the platform is refused with the list of platforms it has"

# 4. architecture-independent package installs everywhere
for w in lx la an; do "$T/$w/build/warp" install docs >/dev/null 2>&1 || fail "docs on $w"; done
[ "$(marker an docs)" = "any" ] || fail "docs marker"
ok "a noarch package installs on every platform"

# 5. search / info show the situation, upgrade leaves foreign-only packages alone
"$WA" search x86 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -q "no build for linux-aarch64" || fail "search does not flag it"
"$WA" info x86only 2>&1 | sed 's/\x1b\[[0-9;]*m//g' | grep -q "none for linux-aarch64" || fail "info does not say"
ok "search and info say when there is no build"

# 5b. translated descriptions: the index carries them, the client picks the user's language
python3 - "$T/repo/index.json" <<'PY' || fail "descriptions in the index"
import json, sys
t = json.load(open(sys.argv[1]))["packages"]["tool"]
assert t["descriptions"] == {"ru": "Инструмент для проверки", "de": "Werkzeug zum Testen"}, t.get("descriptions")
assert "descriptions" not in json.load(open(sys.argv[1]))["packages"]["x86only"]
PY
lang() { env -u LANGUAGE -u LC_ALL -u LC_MESSAGES -u WARP_LANG "$@"; }   # the test must not inherit the user's locale
lang LANG=ru_RU.UTF-8 "$WX" search tool 2>&1 | plain | grep -q "Инструмент для проверки" || fail "LANG=ru did not give the Russian description"
lang LANGUAGE=de:en LANG=ru_RU.UTF-8 "$WX" search tool 2>&1 | plain | grep -q "Werkzeug zum Testen" || fail "LANGUAGE=de did not give the German description"
fr="$(lang LANG=fr_FR.UTF-8 "$WX" search tool 2>&1 | plain)"
case "$fr" in *Инструмент*|*Werkzeug*) fail "an unknown language must fall back to English" ;; esac
c="$(lang LC_ALL=C LANGUAGE=ru LANG=ru_RU.UTF-8 "$WX" search tool 2>&1 | plain)"
case "$c" in *Инструмент*|*Werkzeug*) fail "the C locale must stay English" ;; esac
lang WARP_LANG=de LANG=C "$WX" search tool 2>&1 | plain | grep -q "Werkzeug zum Testen" || fail "WARP_LANG must override the locale"
ok "package descriptions follow the user's language (ru, de, WARP_LANG), English for C and unknown languages"

# 6. pack names archives by platform
mkdir -p "$T/pk/files" && printf '{"name":"q","version":"2.0"}\n' > "$T/pk/manifest.json"
( cd "$T" && "$WX" pack pk | grep -q "q-2.0-x86_64.warp" ) || fail "linux archive name changed"
( cd "$T" && "$WN" pack pk | grep -q "q-2.0-android_aarch64.warp" ) || fail "android archive name"
ok "warp pack: linux keeps name-version-arch, other OS gets name-version-os_arch"

# 7. an index in the old format (no builds) works on linux-x86_64 and is refused elsewhere
mkdir -p "$T/old" && cp "$T/repo/x86only-1.0-x86_64.warp" "$T/old/"
python3 "$ROOT/tools/make-index.py" "$T/old" --base-url "http://127.0.0.1:$RP/../old" --key "$T/priv.hex" --warp "$WX" >/dev/null
python3 - "$T/old/index.json" <<'PY' || fail "old format"
import json, sys
e = json.load(open(sys.argv[1]))["packages"]["x86only"]
assert "builds" not in e and e["sha256"] and e["url"]
PY
ok "x86_64-only index stays in the old format"

echo "PASS: multi-platform"
