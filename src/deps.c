#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "warp.h"

/* ── reverse dependencies ────────────────────────────────────────
 * Every store entry keeps the dependencies the signed index gave it when it
 * was installed (<store>/<name>-<hash>/deps: "name<TAB>version<TAB>repo"
 * lines). With them `remove` and `upgrade` can see who needs a package — and
 * which exact version — among the versions that are active right now, and
 * stop before they break one. Entries installed by an older warp have no
 * file and simply depend on nothing. */

void store_write_deps(const char *name, const char *hash12, const warp_pkg_entry_t *e) {
    char path[600];
    snprintf(path, sizeof(path), WARP_STORE_DIR "/store/%s-%s/deps", name, hash12);
    if (e->dep_count == 0) { remove(path); return; }
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < e->dep_count; i++)
        fprintf(f, "%s\t%s\t%s\n", e->deps[i].name, e->deps[i].version, e->deps[i].repo);
    fclose(f);
}

/* Active packages (other than `except`) whose active version depends on `name`. */
int store_dependents(const char *name, const char *except, warp_dependent_t *out, int max) {
    warp_installed_t *list; int count;
    if (store_list(&list, &count) != WARP_OK) return 0;
    int n = 0;
    for (int i = 0; i < count && n < max; i++) {
        if (strcmp(list[i].name, name) == 0 || (except && strcmp(list[i].name, except) == 0)) continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/deps", list[i].store_path);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[512];
        while (fgets(line, sizeof(line), f) && n < max) {
            line[strcspn(line, "\r\n")] = '\0';
            char *ver = strchr(line, '\t');
            if (ver) { *ver++ = '\0'; char *t = strchr(ver, '\t'); if (t) *t = '\0'; }
            if (strcmp(line, name) != 0) continue;
            snprintf(out[n].pkg, sizeof(out[n].pkg), "%s", list[i].name);
            snprintf(out[n].pkg_version, sizeof(out[n].pkg_version), "%s", list[i].version);
            snprintf(out[n].wants, sizeof(out[n].wants), "%s", ver ? ver : "");
            n++;
        }
        fclose(f);
    }
    store_free_list(list, count);
    return n;
}

/* Dependents of `name` that would break if `version` became the active one
 * (version NULL: if `name` went away). Prints them after `what`; returns how many. */
int deps_blockers(const char *name, const char *version, const char *except, const char *what) {
    warp_dependent_t d[64];
    int n = store_dependents(name, except, d, 64), bad = 0;
    for (int i = 0; i < n; i++) {
        if (version && (!d[i].wants[0] || strcmp(d[i].wants, version) == 0)) continue;
        if (!bad++ && what) warp_err("%s", what);
        if (what) warp_err("  %s %s needs %s%s%s", d[i].pkg, d[i].pkg_version, name,
                           d[i].wants[0] ? " " : "", d[i].wants);
    }
    return bad;
}

/* After an explicit switch/rollback: say which active packages now have the wrong version. */
void deps_warn_active(const char *name) {
    warp_installed_t info;
    memset(&info, 0, sizeof(info));
    if (!store_is_installed(name, &info)) return;
    warp_version_t *v; int n;
    store_versions(name, &v, &n);
    char ver[WARP_MAX_NAME] = "";
    for (int i = 0; i < n; i++) if (v[i].active) snprintf(ver, sizeof(ver), "%s", v[i].version);
    free(v);
    warp_dependent_t d[64];
    int k = store_dependents(name, NULL, d, 64);
    for (int i = 0; i < k; i++)
        if (d[i].wants[0] && strcmp(d[i].wants, ver) != 0)
            warp_warn("%s %s needs %s %s, now %s is active", d[i].pkg, d[i].pkg_version, name, d[i].wants, ver);
}
