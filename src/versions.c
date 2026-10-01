#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include "warp.h"

/* ══════════════════════════════════════════════════════════════
 *  Versions side by side: versions, switch, pin/unpin, run, gc.
 *
 *  The store keeps every installed version as store/<name>-<hash12>; one of
 *  them is `active`. Nothing here downloads or compiles: `warp install
 *  name@version` fetches a published build, the rest works on what is stored.
 * ══════════════════════════════════════════════════════════════ */

/* 1.10 > 1.9; digits compare as numbers, anything else as text. */
static int ver_cmp(const char *a, const char *b) {
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long long x = strtoull(a, &ea, 10), y = strtoull(b, &eb, 10);
            if (x != y) return x < y ? -1 : 1;
            a = ea; b = eb;
        } else {
            if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
            a++; b++;
        }
    }
    return (*a ? 1 : 0) - (*b ? 1 : 0);
}

static int vers_desc(const void *a, const void *b) {
    return -ver_cmp(((const warp_version_t *)a)->version, ((const warp_version_t *)b)->version);
}

/* ── warp versions <name> ────────────────────────────────────── */
int cmd_versions(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp versions <package>"); return 1; }
    const char *name = argv[0];
    if (!store_name_ok(name)) { warp_err("Bad package name: %s", name); return 1; }

    warp_version_t *v; int n;
    store_versions(name, &v, &n);
    if (n > 1) qsort(v, (size_t)n, sizeof(*v), vers_desc);
    char pin[WARP_MAX_NAME] = "";
    int pinned = store_pin_get(name, pin, sizeof(pin));

    printf("\n  " WARP_BOLD "%s" WARP_RESET "\n\n  Installed (in the store):\n", name);
    if (n == 0) printf("    none\n");
    for (int i = 0; i < n; i++) {
        printf("    %-14s %s", v[i].version, v[i].hash12);
        if (v[i].active) printf("  " WARP_GREEN "active" WARP_RESET);
        if (pinned && strcmp(pin, v[i].version) == 0) printf("  " WARP_YELLOW "pinned" WARP_RESET);
        printf("\n");
    }
    free(v);

    warp_index_t idx;
    if (index_load(&idx, 0) == WARP_OK) {
        printf("\n  Published (warp install %s@<version>):\n", name);
        int any = 0;
        for (int i = 0; i < idx.count; i++) {
            const warp_pkg_entry_t *e = &idx.entries[i];
            if (strcmp(e->name, name) != 0) continue;
            any = 1;
            printf("    %-14s [%s]%s%s\n", e->version, e->repo,
                   e->versioned ? "" : "  latest",
                   e->no_build ? "  (no build for " WARP_PLATFORM ")" : "");
        }
        if (!any) printf("    not in the index\n");
        index_free(&idx);
    }
    printf("\n");
    return 0;
}

/* ── warp switch <name> <version|id> ─────────────────────────── */
int cmd_switch(int argc, char **argv) {
    if (argc < 2) { warp_err("Usage: warp switch <package> <version>"); return 1; }
    if (store_switch(argv[0], argv[1]) != WARP_OK) return 1;
    warp_ok("%s: now using %s", argv[0], argv[1]);
    char pin[WARP_MAX_NAME];
    if (store_pin_get(argv[0], pin, sizeof(pin)))
        warp_info("The pin moved with it: %s stays at %s until 'warp unpin %s'", argv[0], pin, argv[0]);
    return 0;
}

/* ── warp pin <name> [version] / warp unpin <name> ───────────── */
int cmd_pin(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp pin <package> [version]"); return 1; }
    const char *name = argv[0];
    if (!store_name_ok(name) || !store_is_installed(name, NULL)) {
        warp_err("Not installed: %s", name);
        return 1;
    }
    if (argc >= 2 && store_switch(name, argv[1]) != WARP_OK) return 1;

    warp_installed_t *list; int count;
    char ver[WARP_MAX_NAME] = "?";
    if (store_list(&list, &count) == WARP_OK) {
        for (int i = 0; i < count; i++)
            if (strcmp(list[i].name, name) == 0) snprintf(ver, sizeof(ver), "%s", list[i].version);
        store_free_list(list, count);
    }
    if (store_pin_set(name, ver) != WARP_OK) { warp_err("Cannot write the pin (try with sudo)"); return 1; }
    warp_ok("%s is pinned at %s: 'warp upgrade' will leave it alone ('warp unpin %s' to release)", name, ver, name);
    return 0;
}

int cmd_unpin(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp unpin <package>"); return 1; }
    if (!store_pin_get(argv[0], NULL, 0)) { warp_warn("%s is not pinned", argv[0]); return 0; }
    store_pin_clear(argv[0]);
    warp_ok("%s is not pinned any more", argv[0]);
    return 0;
}

/* ── warp run <name>[@version] [--bin <b>] [--] [args...] ────── */
int cmd_run(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp run <package>[@version] [--bin <name>] [--] [args...]"); return 1; }
    char name[WARP_MAX_NAME];
    snprintf(name, sizeof(name), "%s", argv[0]);
    char *ver = strchr(name, '@');
    if (ver) *ver++ = '\0';
    if (!store_name_ok(name)) { warp_err("Bad package name: %s", name); return 1; }

    const char *bin = NULL;
    int first = 1;
    if (first < argc && strcmp(argv[first], "--bin") == 0 && first + 1 < argc) { bin = argv[first + 1]; first += 2; }
    if (first < argc && strcmp(argv[first], "--") == 0) first++;

    warp_version_t *v; int n;
    store_versions(name, &v, &n);
    int pick = -1;
    for (int i = 0; i < n; i++) {
        if (ver ? strcmp(v[i].version, ver) == 0 : v[i].active) { pick = i; break; }
    }
    if (pick < 0) {
        if (ver) warp_err("%s %s is not in the store (warp install %s@%s)", name, ver, name, ver);
        else     warp_err("%s is not installed", name);
        free(v);
        return 1;
    }

    char mpath[800];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", v[pick].path);
    size_t len;
    char *ms = read_file(mpath, &len);
    json_t *jm = ms ? json_parse(ms) : NULL;
    free(ms);
    json_t *bins = jm ? json_get(jm, "install_bins") : NULL;
    char exe[1100] = "";
    if (bins && bins->type == JSON_ARRAY) {
        for (int i = 0; i < bins->v.arr.count; i++) {
            json_t *b = bins->v.arr.items[i];
            if (!b || b->type != JSON_STRING) continue;
            const char *base = strrchr(b->v.s, '/');
            base = base ? base + 1 : b->v.s;
            if (!bin || strcmp(bin, base) == 0 || strcmp(bin, b->v.s) == 0) {
                snprintf(exe, sizeof(exe), "%s/files/%s", v[pick].path, b->v.s);
                break;
            }
        }
    }
    if (jm) json_free(jm);
    if (!exe[0]) {
        warp_err("%s %s has no program%s%s to run", name, v[pick].version, bin ? " called " : "", bin ? bin : "");
        free(v);
        return 1;
    }

    setenv("WARP_PACKAGE_DIR", v[pick].path, 1);
    setenv("WARP_PACKAGE_VERSION", v[pick].version, 1);
    free(v);

    char **args = calloc((size_t)(argc - first + 2), sizeof(char *));
    const char *slash = strrchr(exe, '/');
    args[0] = (char *)(slash ? slash + 1 : exe);
    for (int i = first; i < argc; i++) args[i - first + 1] = argv[i];
    fflush(stdout);
    execv(exe, args);
    warp_err("Cannot start %s", exe);
    return 127;
}

/* ── garbage collection ──────────────────────────────────────── */
static unsigned long long tree_bytes(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    unsigned long long total = (unsigned long long)st.st_size;
    if (!S_ISDIR(st.st_mode)) return total;
    DIR *d = opendir(path);
    if (!d) return total;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        total += tree_bytes(child);
    }
    closedir(d);
    return total;
}

/* Never follows symlinks, never leaves the store. */
static int remove_tree(const char *path) {
    if (strncmp(path, WARP_STORE_DIR "/store/", strlen(WARP_STORE_DIR "/store/")) != 0 || strstr(path, "..")) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *e;
        int rc = 0;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[1024];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            if (remove_tree(child) != 0) rc = -1;
        }
        closedir(d);
        return rmdir(path) == 0 ? rc : -1;
    }
    return unlink(path);
}

/* warp gc [--keep N] [--dry-run] */
int cmd_gc(int argc, char **argv) {
    int keep = 2, dry = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0 || strcmp(argv[i], "-n") == 0) dry = 1;
        else if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) {
            keep = atoi(argv[++i]);
            if (keep < 1) { warp_err("--keep must be at least 1"); return 1; }
        } else { warp_err("Usage: warp gc [--keep N] [--dry-run]"); return 1; }
    }

    DIR *d = opendir(WARP_STORE_DIR "/store");
    if (!d) { printf("  The store is empty.\n"); return 0; }

    unsigned long long freed = 0;
    int removed = 0, kept = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char name[WARP_MAX_NAME], hash[13];
        if (!store_split_dir(ent->d_name, name, sizeof(name), hash) || !store_name_ok(name)) continue;

        char dir[1024];
        snprintf(dir, sizeof(dir), WARP_STORE_DIR "/store/%s", ent->d_name);

        int keep_it = 0;
        warp_version_t *v; int n;
        char ver[WARP_MAX_NAME] = "?";
        store_versions(name, &v, &n);
        for (int i = 0; i < n; i++) if (strcmp(v[i].hash12, hash) == 0) { snprintf(ver, sizeof(ver), "%s", v[i].version); keep_it |= v[i].active; }
        free(v);

        char pin[WARP_MAX_NAME];
        if (store_pin_get(name, pin, sizeof(pin)) && strcmp(pin, ver) == 0) keep_it = 1;

        char hs[32][13];
        int hn = store_history(name, hs, 32);
        for (int i = hn - 1; i >= 0 && i >= hn - keep; i--) if (strcmp(hs[i], hash) == 0) keep_it = 1;

        if (keep_it) { kept++; continue; }
        unsigned long long sz = tree_bytes(dir);
        char human[32];
        warp_fmt_size((size_t)sz, human, sizeof(human));
        printf("  %s %-22s %-12s %s\n", dry ? "would remove" : "removing   ", name, ver, human);
        if (!dry && remove_tree(dir) != 0) { warp_warn("Could not remove %s", dir); continue; }
        freed += sz;
        removed++;
    }
    closedir(d);

    char human[32];
    warp_fmt_size((size_t)freed, human, sizeof(human));
    printf("\n  %s %d version(s), %s; kept %d (active, pinned, last %d activations)\n\n",
           dry ? "Would remove" : "Removed", removed, human, kept, keep);
    return 0;
}
