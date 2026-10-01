#!/bin/sh
# Build a fully static `warp` (musl, libcurl and its TLS linked in): one binary that runs on
# every Linux of the same CPU, glibc or musl alike (Void musl, Alpine, Debian, K1OS ...).
#   tools/build-static.sh [out-file]      needs Docker; default out-file: ./warp-static
set -eu
cd "$(dirname "$0")/.."
OUT="${1:-warp-static}"
OUTDIR="$(cd "$(dirname "$OUT")" && pwd)"
docker run --rm -v "$PWD":/src:ro -v "$OUTDIR":/out alpine:3.21 sh -c '
    apk add --no-cache build-base curl-dev pkgconf curl-static openssl-libs-static zlib-static \
        nghttp2-static brotli-static libidn2-static libunistring-static libpsl-static zstd-static >/dev/null
    cp -r /src /build && cd /build && find . -name "*.o" -delete && rm -f warp
    make -s LDFLAGS="-static $(pkg-config --static --libs libcurl)"
    cp warp /out/'"$(basename "$OUT")"'
'
echo "built $OUT ($(wc -c < "$OUT") bytes)"
