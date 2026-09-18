#!/bin/sh
# Build the warp package archive for the current source tree:
#   tools/make-package.sh [out-dir]   -> <out-dir>/warp-<version>-x86_64.warp
#
# Archive layout (what store_add expects): manifest.json at the root and the
# payload under files/, extracted with --strip-components=1.
set -eu
cd "$(dirname "$0")/.."
OUT="${1:-.}"
VERSION="$(sed -n 's/#define WARP_VERSION *"\(.*\)"/\1/p' src/warp.h)"
ARCH="${ARCH:-x86_64}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

make -s
mkdir -p "$STAGE/files/bin" "$OUT"
install -m755 warp "$STAGE/files/bin/warp"
printf '{"name": "warp", "version": "%s", "install_bins": ["bin/warp"]}\n' "$VERSION" > "$STAGE/manifest.json"
# --rsyncable keeps unchanged regions byte-identical between releases so
# `warp delta` can reuse them; -n drops the timestamp for reproducibility.
tar -C "$STAGE" --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' \
    -I 'gzip -n --rsyncable' -cf "$OUT/warp-$VERSION-$ARCH.warp" manifest.json files
sha256sum "$OUT/warp-$VERSION-$ARCH.warp"
