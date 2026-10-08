#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "warp.h"

/* ── system links ────────────────────────────────────────────────
 * A desktop or a daemon is more than programs on the PATH: D-Bus services,
 * polkit actions, session files, PAM and init scripts have to sit where the
 * system looks for them. The manifest lists them in `install_links`; the
 * active version gets a symlink at each place, so switch/rollback move them
 * together with the package and remove takes them away.
 *
 *   "install_links": ["share/xsessions/dde.desktop",          -> /usr/share/xsessions/dde.desktop
 *                     "etc/pam.d/ddm",                         -> /etc/pam.d/ddm
 *                     {"from": "lib/ddm/ddm.initd", "to": "/etc/init.d/ddm"}]
 *   "triggers": ["ldconfig", "glib-schemas", "dbus-reload"]
 *
 * WARP never overwrites a file it did not create: a target that exists and is
 * not a link into this package's store is left alone and reported. Targets
 * must be under one of the integration directories below — a package cannot
 * drop a link onto /sbin/init or /etc/shadow, and since nothing is ever
 * overwritten, /usr/bin/sudo stays the system's. /usr/bin is allowed because
 * desktops write absolute /usr/bin/... paths into their D-Bus and .desktop files. Every link made is recorded in
 * <store>/links/<name>, and only recorded links that still point into the
 * package are removed. Triggers are a fixed list of commands compiled in here;
 * a manifest only names them, it never carries a command line. */

#if defined(__linux__) && !defined(__ANDROID__)
#define LINKS_SUPPORTED 1
#else
#define LINKS_SUPPORTED 0
#endif

#define LINKS_DIR WARP_STORE_DIR "/links"
#define LINKS_MAX 200000      /* a desktop links every file of the package: icons, translations */

static const char *const allowed[] = {
    "/usr/bin/", "/usr/share/", "/usr/lib/", "/usr/lib64/", "/usr/libexec/",
    "/etc/xdg/", "/etc/dbus-1/", "/etc/pam.d/", "/etc/polkit-1/",
    "/etc/init.d/", "/etc/conf.d/", "/etc/X11/", "/etc/systemd/",
    "/etc/ld.so.conf.d/", "/etc/profile.d/", "/etc/fonts/", "/etc/environment.d/",
    NULL
};

static const struct { const char *name; const char *tool; const char *cmd; } triggers[] = {
    { "ldconfig",         "ldconfig",               "ldconfig" },
    { "glib-schemas",     "glib-compile-schemas",   "glib-compile-schemas /usr/share/glib-2.0/schemas" },
    { "desktop-database", "update-desktop-database","update-desktop-database -q /usr/share/applications" },
    { "icon-cache",       "gtk-update-icon-cache",  "gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor" },
    { "mime-database",    "update-mime-database",   "update-mime-database /usr/share/mime" },
    { "dbus-reload",      "dbus-send",              "dbus-send --system --type=method_call --dest=org.freedesktop.DBus "
                                                    "/org/freedesktop/DBus org.freedesktop.DBus.ReloadConfig" },
};
#define TRIGGER_COUNT ((int)(sizeof(triggers) / sizeof(triggers[0])))

static int has_dotdot(const char *p) {
    for (const char *s = p; (s = strstr(s, "..")) != NULL; s += 2)
        if ((s == p || s[-1] == '/') && (s[2] == '\0' || s[2] == '/')) return 1;
    return 0;
}

static int target_allowed(const char *to) {
    if (to[0] != '/' || has_dotdot(to) || strstr(to, "//")) return 0;
    for (int i = 0; allowed[i]; i++) {
        size_t n = strlen(allowed[i]);
        if (strncmp(to, allowed[i], n) == 0 && to[n]) return 1;
    }
    return 0;
}

/* "share/x" -> "/usr/share/x", "etc/x" -> "/etc/x": the files/ tree mirrors a DESTDIR install. */
static int resolve_link(const json_t *item, char *from, size_t fsz, char *to, size_t tsz) {
    if (item->type == JSON_STRING) {
        snprintf(from, fsz, "%s", item->v.s);
        if (strncmp(item->v.s, "etc/", 4) == 0) snprintf(to, tsz, "/%s", item->v.s);
        else                                    snprintf(to, tsz, "/usr/%s", item->v.s);
    } else if (item->type == JSON_OBJECT) {
        snprintf(from, fsz, "%s", json_str((json_t *)item, "from", ""));
        snprintf(to,   tsz, "%s", json_str((json_t *)item, "to",   ""));
    } else {
        return 0;
    }
    return from[0] && from[0] != '/' && !has_dotdot(from) && to[0];
}

/* Which package's store entry a link points into, or "" when it is not WARP's. */
static void link_owner(const char *path, char *owner, size_t sz) {
    owner[0] = '\0';
    char target[1024];
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0) return;
    target[n] = '\0';
    const char *pre = WARP_STORE_DIR "/store/";
    size_t pl = strlen(pre);
    if (strncmp(target, pre, pl) != 0) return;
    char dir[WARP_MAX_NAME + 16];
    size_t dl = strcspn(target + pl, "/");
    if (dl == 0 || dl >= sizeof(dir)) return;
    memcpy(dir, target + pl, dl);
    dir[dl] = '\0';
    char hash[13];
    store_split_dir(dir, owner, sz, hash);
}

static json_t *load_manifest(const char *store_pkg) {
    char path[768];
    snprintf(path, sizeof(path), "%s/manifest.json", store_pkg);
    size_t len;
    char *s = read_file(path, &len);
    if (!s) return NULL;
    json_t *jm = json_parse(s);
    free(s);
    return jm;
}

static unsigned trigger_mask(const json_t *jm) {
    unsigned mask = 0;
    json_t *jt = jm ? json_get((json_t *)jm, "triggers") : NULL;
    if (!jt || jt->type != JSON_ARRAY) return 0;
    for (int i = 0; i < jt->v.arr.count; i++) {
        json_t *t = jt->v.arr.items[i];
        if (!t || t->type != JSON_STRING) continue;
        int k;
        for (k = 0; k < TRIGGER_COUNT; k++)
            if (strcmp(t->v.s, triggers[k].name) == 0) { mask |= 1u << k; break; }
        if (k == TRIGGER_COUNT) warp_warn("Unknown trigger '%s' ignored (a newer warp may know it)", t->v.s);
    }
    return mask;
}

/* Remove the recorded links of the active version of `name`; returns its triggers. */
unsigned links_clear(const char *name) {
    if (!store_name_ok(name)) return 0;
    char active[512], store_pkg[512] = {0};
    snprintf(active, sizeof(active), WARP_STORE_DIR "/active/%s", name);
    ssize_t n = readlink(active, store_pkg, sizeof(store_pkg) - 1);
    unsigned mask = 0;
    if (n > 0) {
        json_t *jm = load_manifest(store_pkg);
        mask = trigger_mask(jm);
        if (jm) json_free(jm);
    }

    char rec[512];
    snprintf(rec, sizeof(rec), LINKS_DIR "/%s", name);
    FILE *f = fopen(rec, "r");
    if (!f) return mask;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;
        char path[1200], owner[WARP_MAX_NAME];
        snprintf(path, sizeof(path), WARP_SYSROOT "%s", line);
        link_owner(path, owner, sizeof(owner));
        if (strcmp(owner, name) == 0) unlink(path);   /* replaced by someone else since: not ours to touch */
    }
    fclose(f);
    unlink(rec);
    return mask;
}

/* Link the files of `store_pkg` listed in its manifest; returns its triggers.
 * *failed counts the links that could not be made. */
unsigned links_apply(const char *name, const char *store_pkg, int *failed) {
    *failed = 0;
    json_t *jm = load_manifest(store_pkg);
    if (!jm) return 0;
    unsigned mask = trigger_mask(jm);
    json_t *jl = json_get(jm, "install_links");
    if (!jl || jl->type != JSON_ARRAY || jl->v.arr.count == 0) { json_free(jm); return mask; }
    if (!LINKS_SUPPORTED) {
        warp_warn("%s has system links; they are made on Linux only, skipped here", name);
        json_free(jm);
        return 0;
    }
    if (mkdirs(LINKS_DIR, 0755) != WARP_OK) { json_free(jm); *failed = 1; return mask; }

    char rec[512];
    snprintf(rec, sizeof(rec), LINKS_DIR "/%s", name);
    FILE *out = fopen(rec, "a");
    if (!out) { warp_err("Cannot write %s: %s", rec, strerror(errno)); json_free(jm); *failed = 1; return mask; }

    for (int i = 0; i < jl->v.arr.count && i < LINKS_MAX; i++) {
        json_t *item = jl->v.arr.items[i];
        char from[512], to[512];
        if (!item || !resolve_link(item, from, sizeof(from), to, sizeof(to)) || !target_allowed(to)) {
            warp_err("%s: refusing link %s -> %s (outside the allowed system directories)", name,
                     item && item->type == JSON_STRING ? item->v.s : "?", to);
            (*failed)++;
            continue;
        }
        char src[1200], dst[1200];
        snprintf(src, sizeof(src), "%s/files/%s", store_pkg, from);
        snprintf(dst, sizeof(dst), WARP_SYSROOT "%s", to);
        struct stat st;
        if (lstat(src, &st) != 0) {
            warp_err("%s: %s is not in the package", name, from);
            (*failed)++;
            continue;
        }
        if (lstat(dst, &st) == 0) {
            char owner[WARP_MAX_NAME];
            link_owner(dst, owner, sizeof(owner));
            if (strcmp(owner, name) != 0) {
                warp_err("%s: %s already exists%s%s — left alone", name, to,
                         owner[0] ? " and belongs to " : " and is not WARP's", owner);
                (*failed)++;
                continue;
            }
            unlink(dst);       /* a stale link of another version of this package */
        }
        char parent[1200];
        snprintf(parent, sizeof(parent), "%s", dst);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) { *slash = '\0'; mkdirs(parent, 0755); }
        if (symlink(src, dst) != 0) {
            warp_err("%s: cannot link %s: %s", name, to, strerror(errno));
            (*failed)++;
            continue;
        }
        fprintf(out, "%s\n", to);
    }
    fclose(out);
    json_free(jm);
    return mask;
}

void links_run_triggers(unsigned mask) {
    if (!mask) return;
    int skip = WARP_SYSROOT[0] || getenv("WARP_NO_TRIGGERS");
    for (int k = 0; k < TRIGGER_COUNT; k++) {
        if (!(mask & (1u << k))) continue;
        if (skip) { warp_info("Trigger %s skipped", triggers[k].name); continue; }
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "command -v %s >/dev/null 2>&1", triggers[k].tool);
        if (system(cmd) != 0) { warp_info("Trigger %s: %s is not installed, skipped", triggers[k].name, triggers[k].tool); continue; }
        snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>&1", triggers[k].cmd);
        if (system(cmd) != 0) warp_warn("Trigger %s failed: %s", triggers[k].name, triggers[k].cmd);
    }
}

/* `warp links [pkg]`: what the active versions have linked into the system. */
int cmd_links(int argc, char **argv) {
    warp_installed_t *list; int count;
    if (store_list(&list, &count) != WARP_OK) return 1;
    int shown = 0;
    for (int i = 0; i < count; i++) {
        if (argc > 0 && strcmp(argv[0], list[i].name) != 0) continue;
        char rec[512];
        snprintf(rec, sizeof(rec), LINKS_DIR "/%s", list[i].name);
        FILE *f = fopen(rec, "r");
        if (!f) continue;
        printf("\n  " WARP_BOLD "%s" WARP_RESET " %s\n", list[i].name, list[i].version);
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            if (!line[0]) continue;
            char path[1200], owner[WARP_MAX_NAME];
            snprintf(path, sizeof(path), WARP_SYSROOT "%s", line);
            link_owner(path, owner, sizeof(owner));
            printf("    %s%s\n", line, strcmp(owner, list[i].name) == 0 ? "" : "  " WARP_YELLOW "(missing)" WARP_RESET);
            shown++;
        }
        fclose(f);
    }
    store_free_list(list, count);
    if (!shown) printf("  No system links%s%s.\n", argc > 0 ? " for " : "", argc > 0 ? argv[0] : "");
    printf("\n");
    return 0;
}
