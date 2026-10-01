#!/bin/sh
# Build the warp package archive for the current source tree:
#   [WARP_BIN=path/to/warp] [ARCH=x86_64_static] tools/make-package.sh [out-dir]   -> <out-dir>/warp-<version>-<arch>.warp
#
# Archive layout (what store_add expects): manifest.json at the root and the
# payload under files/, extracted with --strip-components=1.
set -eu
cd "$(dirname "$0")/.."
OUT="${1:-.}"
VERSION="$(sed -n 's/#define WARP_VERSION *"\(.*\)"/\1/p' src/warp.h)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

make -s
# The archive tag (x86_64, aarch64, android_aarch64, ...) is what this very binary was built for.
ARCH="${ARCH:-$("${WARP_BIN:-./warp}" archive-tag)}"
mkdir -p "$STAGE/files/bin" "$OUT"
install -m755 "${WARP_BIN:-warp}" "$STAGE/files/bin/warp"
printf '{"name": "warp", "version": "%s", "install_bins": ["bin/warp"]}\n' "$VERSION" > "$STAGE/manifest.json"
# --rsyncable keeps unchanged regions byte-identical between releases so
# `warp delta` can reuse them; -n drops the timestamp for reproducibility.
tar -C "$STAGE" --owner=0 --group=0 --numeric-owner --mtime='2026-01-01 00:00:00' \
    -I 'gzip -n --rsyncable' -cf "$OUT/warp-$VERSION-$ARCH.warp" manifest.json files
sha256sum "$OUT/warp-$VERSION-$ARCH.warp"
