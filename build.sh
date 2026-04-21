#!/bin/bash
# Build warp package manager

set -e

PKG_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BIN_DIR="${PREFIX}/bin"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_info()  { echo -e "${GREEN}[warp]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[warp]${NC} $1"; }
log_error() { echo -e "${RED}[warp]${NC} $1"; }

check_deps() {
    for dep in gcc curl-config openssl; do
        if ! command -v "$dep" &>/dev/null; then
            log_error "Missing build dependency: $dep"
            log_warn  "Install: sudo dnf install gcc libcurl-devel openssl-devel"
            exit 1
        fi
    done
}

build() {
    log_info "Building warp..."
    make -C "${PKG_DIR}" clean 2>/dev/null || true
    make -C "${PKG_DIR}" -j"$(nproc)"
    log_info "Build complete: ${PKG_DIR}/warp"
}

pkg_install() {
    log_info "Installing warp to ${BIN_DIR}..."
    install -Dm755 "${PKG_DIR}/warp" "${BIN_DIR}/warp"
    log_info "warp installed: ${BIN_DIR}/warp"
}

case "${1:-all}" in
    build)   check_deps && build ;;
    install) build && pkg_install ;;
    all)     check_deps && build && pkg_install ;;
    clean)   make -C "${PKG_DIR}" clean ;;
    *)
        echo "Usage: $0 [build|install|all|clean]"
        exit 1
        ;;
esac
