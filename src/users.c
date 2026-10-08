#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/wait.h>
#include "warp.h"

/* ── system users and groups ─────────────────────────────────────
 * A display manager runs its greeter as its own user, a daemon wants its
 * group. The manifest declares them, sysusers-style:
 *
 *   "groups": ["ddm", {"name": "seat"}],
 *   "users":  [{"name": "ddm", "group": "ddm", "groups": ["video"],
 *               "home": "/var/lib/ddm", "shell": "/sbin/nologin",
 *               "comment": "DDM greeter", "create_home": true}]
 *
 * They are created as system accounts when a version becomes active, if they
 * do not exist yet; existing ones are never changed and none are ever deleted
 * (files owned by a uid outlive the package, and a reused uid would hand them
 * to someone else). Names, paths and the comment are checked before anything
 * runs, and groupadd/useradd get an argv, never a shell line. */

static int name_ok(const char *s) {
    if (!s || !*s || strlen(s) > 31) return 0;
    if (!((*s >= 'a' && *s <= 'z') || *s == '_')) return 0;
    for (const char *p = s + 1; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return 0;
    return 1;
}

static int path_ok(const char *s) {
    if (!s || s[0] != '/' || strlen(s) > 200 || strstr(s, "/..")) return 0;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
              *p == '/' || *p == '_' || *p == '-' || *p == '.')) return 0;
    return 1;
}

static int comment_ok(const char *s) {
    if (strlen(s) > 100) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p < 0x20 || *p == ':' || *p == 0x7f) return 0;
    return 1;
}

static int run_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int dry(void) { return WARP_SYSROOT[0] != '\0'; }

static int ensure_group(const char *pkg, const char *g) {
    if (!name_ok(g)) { warp_err("%s: bad group name '%s'", pkg, g); return 1; }
    if (dry()) { warp_info("Group %s would be created (scratch root)", g); return 0; }
    if (getgrnam(g)) return 0;
    char *argv[] = { "groupadd", "-r", (char *)g, NULL };
    int rc = run_argv(argv);
    if (rc != 0) { warp_err("%s: groupadd %s failed (%d)", pkg, g, rc); return 1; }
    warp_ok("Created system group %s", g);
    return 0;
}

static int ensure_user(const char *pkg, json_t *u) {
    const char *name    = json_str(u, "name", "");
    const char *group   = json_str(u, "group", "");
    const char *home    = json_str(u, "home", "/var/empty");
    const char *shell   = json_str(u, "shell", "/sbin/nologin");
    const char *comment = json_str(u, "comment", "");
    if (!name_ok(name))                   { warp_err("%s: bad user name '%s'", pkg, name); return 1; }
    if (group[0] && !name_ok(group))      { warp_err("%s: bad group '%s' for user %s", pkg, group, name); return 1; }
    if (!path_ok(home) || !path_ok(shell)){ warp_err("%s: bad home or shell for user %s", pkg, name); return 1; }
    if (!comment_ok(comment))             { warp_err("%s: bad comment for user %s", pkg, name); return 1; }

    char extra[512] = "";
    json_t *gs = json_get(u, "groups");
    if (gs && gs->type == JSON_ARRAY) {
        for (int i = 0; i < gs->v.arr.count; i++) {
            json_t *g = gs->v.arr.items[i];
            if (!g || g->type != JSON_STRING || !name_ok(g->v.s)) { warp_err("%s: bad supplementary group for user %s", pkg, name); return 1; }
            if (strlen(extra) + strlen(g->v.s) + 2 >= sizeof(extra)) break;
            if (extra[0]) strcat(extra, ",");
            strcat(extra, g->v.s);
        }
    }
    if (group[0] && ensure_group(pkg, group) != 0) return 1;

    if (dry()) { warp_info("User %s would be created (scratch root)", name); return 0; }
    if (getpwnam(name)) return 0;
    /* supplementary groups that do not exist are left out rather than failing the account */
    char have[512] = "";
    if (extra[0]) {
        char tmp[512]; snprintf(tmp, sizeof(tmp), "%s", extra);
        for (char *t = strtok(tmp, ","); t; t = strtok(NULL, ",")) {
            if (!getgrnam(t)) { warp_warn("%s: group %s does not exist, user %s is not added to it", pkg, t, name); continue; }
            if (have[0]) strcat(have, ",");
            strcat(have, t);
        }
    }
    char *argv[20]; int a = 0;
    argv[a++] = "useradd"; argv[a++] = "-r";
    argv[a++] = "-d"; argv[a++] = (char *)home;
    argv[a++] = "-s"; argv[a++] = (char *)shell;
    if (group[0])   { argv[a++] = "-g"; argv[a++] = (char *)group; }
    else            { argv[a++] = "-U"; }
    if (have[0])    { argv[a++] = "-G"; argv[a++] = have; }
    if (comment[0]) { argv[a++] = "-c"; argv[a++] = (char *)comment; }
    json_t *ch = json_get(u, "create_home");
    if (ch && ch->type == JSON_BOOL && ch->v.b) argv[a++] = "-m";
    argv[a++] = (char *)name;
    argv[a] = NULL;
    int rc = run_argv(argv);
    if (rc != 0) { warp_err("%s: useradd %s failed (%d)", pkg, name, rc); return 1; }
    warp_ok("Created system user %s (home %s)", name, home);
    return 0;
}

/* Groups first (users may name them), then users. Returns how many failed. */
int users_apply(const char *name, const json_t *manifest) {
    if (!manifest) return 0;
    json_t *gs = json_get((json_t *)manifest, "groups");
    json_t *us = json_get((json_t *)manifest, "users");
    int any = (gs && gs->type == JSON_ARRAY && gs->v.arr.count) || (us && us->type == JSON_ARRAY && us->v.arr.count);
    if (!any) return 0;
#if !defined(__linux__) || defined(__ANDROID__)
    warp_warn("%s declares system users/groups; they are created on Linux only, skipped here", name);
    return 0;
#else
    if (!dry() && geteuid() != 0) { warp_err("%s needs system users/groups: run warp as root", name); return 1; }
    int failed = 0;
    if (gs && gs->type == JSON_ARRAY)
        for (int i = 0; i < gs->v.arr.count; i++) {
            json_t *g = gs->v.arr.items[i];
            const char *gn = !g ? "" : g->type == JSON_STRING ? g->v.s : g->type == JSON_OBJECT ? json_str(g, "name", "") : "";
            failed += ensure_group(name, gn);
        }
    if (us && us->type == JSON_ARRAY)
        for (int i = 0; i < us->v.arr.count; i++) {
            json_t *u = us->v.arr.items[i];
            if (!u || u->type != JSON_OBJECT) { warp_err("%s: a user must be an object", name); failed++; continue; }
            failed += ensure_user(name, u);
        }
    return failed;
#endif
}
