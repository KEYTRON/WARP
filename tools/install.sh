#!/bin/sh
# WARP installer for Linux, macOS (Apple Silicon) and Termux.
#
#   curl -fsSL https://keytron-prime.org/packages/install.sh | sh
#   sh install.sh [-y]        # -y: do not wait for Enter
#
# It says what it is going to do and waits for Enter, downloads the ready-made `warp`
# binary for this machine over HTTPS, checks its sha256, puts it where warp lives, and
# then lets warp itself fetch the signed package index (warp update). The key warp
# trusts is printed at the end so it can be compared with the one on the project page.
# Nothing is compiled.
set -eu

BASE="${WARP_INSTALL_BASE:-https://keytron-prime.org/packages/install}"
YES=0
[ "${1:-}" = "-y" ] && YES=1

say()  { printf '%s\n' "$*"; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 is required but not installed"; }

need curl
need tar
if command -v sha256sum >/dev/null 2>&1; then
    sha() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    die "sha256sum or shasum is required"
fi

os="$(uname -s)"
case "$(uname -m)" in
    x86_64|amd64)  arch=x86_64 ;;
    aarch64|arm64) arch=aarch64 ;;
    *) die "no ready-made warp for the CPU $(uname -m)" ;;
esac

SUDO=""
case "$os" in
    Darwin)
        [ "$arch" = aarch64 ] || die "the macOS build exists for Apple Silicon only so far"
        tag=macos_aarch64
        prefix=/opt/warp
        bindir=/opt/warp/bin
        where="/opt/warp (owned by you, like Homebrew's prefix; nothing needs root afterwards)"
        ;;
    Linux)
        case "${PREFIX:-}" in
            *com.termux*)
                [ "$arch" = aarch64 ] || die "the Termux build exists for aarch64 only"
                tag=android_aarch64
                prefix="$PREFIX/var/lib/warp"
                bindir="$PREFIX/bin"
                where="$PREFIX (Termux: no root needed)"
                ;;
            *)
                if [ "$arch" = x86_64 ]; then
                    tag=x86_64_static         # one static binary: glibc and musl alike
                else
                    tag=aarch64               # glibc build; no musl one yet
                    ls /lib/ld-musl-* >/dev/null 2>&1 && die "no musl build for aarch64 yet"
                fi
                prefix=/var/lib/warp
                bindir=/usr/local/bin
                where="/usr/local/bin/warp and /var/lib/warp (needs root)"
                [ "$(id -u)" = 0 ] || { need sudo; SUDO=sudo; }
                ;;
        esac
        ;;
    *) die "$os is not supported" ;;
esac

say ""
say "  WARP installer"
say ""
say "  Platform:   $os $(uname -m)  ->  build $tag"
say "  Installs:   the warp program into $where"
say "  Downloads:  $BASE/ (HTTPS), sha256 checked, nothing is compiled"
say "  Then:       warp update fetches the package index, signed with a key built into warp"
say ""
if [ "$YES" != 1 ]; then
    if [ -r /dev/tty ]; then
        printf '  Press Enter to continue, Ctrl+C to cancel: '
        read -r _ </dev/tty
    else
        die "no terminal to ask on; run again with -y to go ahead"
    fi
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# each platform may be one release behind (a build machine can be off), so it has its own file
version="$(curl -fsSL "$BASE/latest-$tag" 2>/dev/null || curl -fsSL "$BASE/latest")"
version="$(printf '%s' "$version" | tr -d ' \n')"
[ -n "$version" ] || die "could not read the latest version"
name="warp-$version-$tag.warp"
say "  Downloading $name ..."
curl -fsSL -o "$tmp/$name" "$BASE/$name" || die "no build $name on the server"
want="$(curl -fsSL "$BASE/$name.sha256" | cut -d' ' -f1)"
have="$(sha "$tmp/$name")"
[ -n "$want" ] && [ "$want" = "$have" ] || die "sha256 of $name does not match (wanted $want, got $have)"

tar -xzf "$tmp/$name" -C "$tmp" files/bin/warp
[ -x "$tmp/files/bin/warp" ] || chmod 755 "$tmp/files/bin/warp"

if [ "$os" = Darwin ] && [ ! -w "$prefix" ] 2>/dev/null; then
    say "  Making $prefix yours (asks for your password once) ..."
    sudo mkdir -p "$prefix" && sudo chown "$(id -un)" "$prefix"
fi
$SUDO mkdir -p "$bindir"
$SUDO install -m 755 "$tmp/files/bin/warp" "$bindir/warp"

say "  Installed warp $version to $bindir/warp"
export PATH="$bindir:$PATH"
$SUDO "$bindir/warp" update || say "  (warp update failed; run it again later)"

say ""
say "  Trusted repository key:"
"$bindir/warp" repo list 2>/dev/null | sed -n 's/^ *\(key [0-9a-f]*\.\.\.\)$/    \1/p' | head -1
say "  Compare it with the key on https://keytron-prime.org/projects/warp"
say ""
case "$os$bindir" in
    Darwin*|*com.termux*)
        say "  Put warp's programs on your PATH (add to your shell profile):"
        say "    eval \"\$($bindir/warp shellenv)\"          # bash / zsh"
        say "    $bindir/warp shellenv | source          # fish"
        ;;
esac
say "  Try:  warp search editor"
say ""
