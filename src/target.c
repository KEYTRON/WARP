/*
 * target.c - where `install` puts things, and for which platform.
 *
 * By default both are this machine. `--root DIR` (or WARP_ROOT) moves the store, the program
 * directory and the system links under DIR, and `--platform <os>-<arch>` (or WARP_PLATFORM) takes
 * the builds of another platform from the index. Together they stage a system that is not the one
 * warp runs on: the services of a K1K disk image, for instance, which warp cannot run on itself.
 * A staged root is not relocatable: the store links inside it name DIR.
 */
#include "warp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char g_warp_root[512] = "";

/* Root of the system links: --root, else the build's -DWARP_SYSROOT (the tests point it at a scratch directory). */
const char *warp_sysroot(void) { return g_warp_root[0] ? g_warp_root : WARP_SYSROOT; }

static char g_os[32], g_arch[32];     /* empty = this build's own WARP_OS / WARP_ARCH */

static const char *at(const char *dir, const char *suffix) {
    static char buf[8][1024];
    static int next;
    char *b = buf[next++ & 7];
    snprintf(b, sizeof(buf[0]), "%s%s%s", g_warp_root, dir, suffix ? suffix : "");
    return b;
}

const char *warp_store(void)                  { return at(WARP_STORE_DIR, NULL); }
const char *warp_bin(void)                    { return at(WARP_BIN_DIR, NULL); }
const char *warp_store_at(const char *suffix) { return at(WARP_STORE_DIR, suffix); }

int warp_set_root(const char *dir) {
    if (dir && *dir) {
        size_t n = strlen(dir);
        if (n >= sizeof(g_warp_root) - 1) return WARP_ERR_INVAL;
        memcpy(g_warp_root, dir, n + 1);
        while (n > 1 && g_warp_root[n - 1] == '/') g_warp_root[--n] = '\0';
        if (strcmp(g_warp_root, "/") == 0) g_warp_root[0] = '\0';
    }
    snprintf(g_volunteer_dir, sizeof(g_volunteer_dir), "%s", warp_store_at("/volunteer"));
    return WARP_OK;
}

int warp_set_platform(const char *platform) {
    g_os[0] = g_arch[0] = '\0';
    if (!platform || !*platform) return WARP_OK;
    const char *dash = strchr(platform, '-');
    if (!dash || dash == platform || !dash[1] || strchr(dash + 1, '-')) return WARP_ERR_INVAL;
    if ((size_t)(dash - platform) >= sizeof(g_os) || strlen(dash + 1) >= sizeof(g_arch)) return WARP_ERR_INVAL;
    memcpy(g_os, platform, (size_t)(dash - platform));
    g_os[dash - platform] = '\0';
    strcpy(g_arch, dash + 1);
    return WARP_OK;
}

const char *warp_target_os(void)   { return g_os[0] ? g_os : WARP_OS; }
const char *warp_target_arch(void) { return g_arch[0] ? g_arch : WARP_ARCH; }
int warp_target_is_host(void)      { return !g_os[0] && !g_arch[0]; }
