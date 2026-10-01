#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include "warp.h"

/* ── helpers ─────────────────────────────────────────────────── */
int mkdirs(const char *path, mode_t mode) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (tmp[len-1] == '/') tmp[--len] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode) == 0 || errno == EEXIST ? WARP_OK : WARP_ERR_IO;
}

int path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

long file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size_t len = (size_t)ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, len, f);
    buf[len] = '\0';
    fclose(f);
    if (out_len) *out_len = len;
    return buf;
}

int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return WARP_ERR_IO;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return WARP_ERR_IO; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    return WARP_OK;
}

/* ── store directories ───────────────────────────────────────── */
#define STORE_PATH(buf, ...) snprintf(buf, sizeof(buf), WARP_STORE_DIR "/" __VA_ARGS__)

int store_init(void) {
    const char *dirs[] = {
        WARP_STORE_DIR,
        WARP_STORE_DIR "/store",
        WARP_STORE_DIR "/active",
        WARP_STORE_DIR "/prev",
        WARP_STORE_DIR "/history",
        WARP_STORE_DIR "/pins",
        "/usr/local/bin",
        NULL
    };
    for (int i = 0; dirs[i]; i++) {
        if (mkdirs(dirs[i], 0755) != WARP_OK && !path_exists(dirs[i])) {
            warp_err("Cannot create %s: %s", dirs[i], strerror(errno));
            return WARP_ERR_IO;
        }
    }
    return WARP_OK;
}

/* ── add package to store ────────────────────────────────────── */
int store_add(const warp_manifest_t *m, const char *warp_path, const char *sha256) {
    char hash12[13];
    strncpy(hash12, sha256, 12);
    hash12[12] = '\0';

    char store_pkg[512];
    snprintf(store_pkg, sizeof(store_pkg),
             WARP_STORE_DIR "/store/%s-%s", m->name, hash12);

    if (path_exists(store_pkg)) {
        warp_info("Already in store: %s-%s", m->name, hash12);
        return WARP_OK;
    }

    /* Create store entry directory */
    char files_dir[768];
    snprintf(files_dir, sizeof(files_dir), "%s/files", store_pkg);
    if (mkdirs(files_dir, 0755) != WARP_OK) return WARP_ERR_IO;

    /* Save full sha256 for P2P serving */
    char sha_path[768];
    snprintf(sha_path, sizeof(sha_path), "%s/sha256", store_pkg);
    FILE *sha_f = fopen(sha_path, "w");
    if (sha_f) { fprintf(sha_f, "%s\n", sha256); fclose(sha_f); }

    /* Keep a copy of the .warp archive for seeding to peers */
    char warp_copy[768];
    snprintf(warp_copy, sizeof(warp_copy), "%s/package.warp", store_pkg);
    copy_file(warp_path, warp_copy);

    /* Extract .warp (tar.gz) into files/ */
    warp_info("Extracting package...");
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "tar -xzf %.400s -C %.300s --strip-components=1 files/ 2>/dev/null || "
             "tar -xzf %.400s -C %.300s 2>/dev/null",
             warp_path, files_dir, warp_path, files_dir);
    if (system(cmd) != 0) {
        warp_err("Extraction failed");
        /* cleanup */
        snprintf(cmd, sizeof(cmd), "rm -rf %s", store_pkg);
        system(cmd);
        return WARP_ERR_IO;
    }

    /* Extract manifest.json from archive */
    snprintf(cmd, sizeof(cmd),
             "tar -xzf %s -C %s manifest.json 2>/dev/null", warp_path, store_pkg);
    system(cmd);

    return WARP_OK;
}

/* ── names, activation history, pins ─────────────────────────── */

/* A package name ends up in file paths: keep it to a plain file name. */
int store_name_ok(const char *name) {
    if (!name || !*name || name[0] == '.' || strlen(name) >= WARP_MAX_NAME) return 0;
    for (const char *p = name; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.' || *p == '+')) return 0;
    return 1;
}

#define HIST_MAX 32
static int active_hash(const char *name, char hash12[13]);

int store_history(const char *name, char (*out)[13], int max) {
    if (!store_name_ok(name)) return 0;
    char path[512];
    snprintf(path, sizeof(path), WARP_STORE_DIR "/history/%s", name);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[64];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (strlen(line) != 12) continue;
        if (n >= max) { memmove(out[0], out[1], (size_t)(max - 1) * 13); n = max - 1; }
        memcpy(out[n], line, 13);
        n++;
    }
    fclose(f);
    return n;
}

static void hist_write(const char *name, char (*hs)[13], int n) {
    char path[512];
    snprintf(path, sizeof(path), WARP_STORE_DIR "/history/%s", name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", hs[i]);
    fclose(f);
}

static void hist_push(const char *name, const char *hash12) {
    char hs[HIST_MAX][13];
    int n = store_history(name, hs, HIST_MAX);
    if (n > 0 && strcmp(hs[n - 1], hash12) == 0) return;
    if (n >= HIST_MAX) { memmove(hs[0], hs[1], (size_t)(HIST_MAX - 1) * 13); n = HIST_MAX - 1; }
    memcpy(hs[n], hash12, 13);
    hist_write(name, hs, n + 1);
}

int store_pin_get(const char *name, char *ver, size_t sz) {
    if (!store_name_ok(name)) return 0;
    char path[512];
    snprintf(path, sizeof(path), WARP_STORE_DIR "/pins/%s", name);
    size_t len;
    char *s = read_file(path, &len);
    if (!s) return 0;
    s[strcspn(s, "\r\n")] = '\0';
    if (ver && sz) snprintf(ver, sz, "%s", s);
    free(s);
    return 1;
}

int store_pin_set(const char *name, const char *ver) {
    if (!store_name_ok(name)) return WARP_ERR_INVAL;
    char path[512];
    snprintf(path, sizeof(path), WARP_STORE_DIR "/pins/%s", name);
    FILE *f = fopen(path, "w");
    if (!f) return WARP_ERR_IO;
    fprintf(f, "%s\n", ver);
    fclose(f);
    return WARP_OK;
}

void store_pin_clear(const char *name) {
    if (!store_name_ok(name)) return;
    char path[512];
    snprintf(path, sizeof(path), WARP_STORE_DIR "/pins/%s", name);
    unlink(path);
}

/* ── activate a version ──────────────────────────────────────── */
static int activate_impl(const char *name, const char *hash12, int push) {
    char old_hash[13] = "";
    int have_old = active_hash(name, old_hash);
    char store_pkg[512];
    snprintf(store_pkg, sizeof(store_pkg),
             WARP_STORE_DIR "/store/%s-%s", name, hash12);

    if (!path_exists(store_pkg)) {
        warp_err("Store entry not found: %s-%s", name, hash12);
        return WARP_ERR_NOENT;
    }

    char active_link[512], prev_link[512];
    snprintf(active_link, sizeof(active_link), WARP_STORE_DIR "/active/%s", name);
    snprintf(prev_link,   sizeof(prev_link),   WARP_STORE_DIR "/prev/%s",   name);

    /* Move current active → prev */
    if (path_exists(active_link)) {
        unlink(prev_link);
        /* Read current target */
        char cur_target[512] = {0};
        ssize_t n = readlink(active_link, cur_target, sizeof(cur_target)-1);
        if (n > 0) {
            cur_target[n] = '\0';
            symlink(cur_target, prev_link);
        }
        unlink(active_link);
    }

    /* Create new active symlink */
    if (symlink(store_pkg, active_link) != 0) {
        warp_err("Failed to create symlink: %s", strerror(errno));
        return WARP_ERR_IO;
    }
    if (push) {
        /* A package installed before the history existed: seed it with what was active. */
        char probe[1][13];
        if (have_old && store_history(name, probe, 1) == 0) hist_push(name, old_hash);
        hist_push(name, hash12);
    }

    /* Expose binaries in /usr/local/bin */
    char manifest_path[768];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", store_pkg);
    if (path_exists(manifest_path)) {
        size_t len;
        char *manifest_str = read_file(manifest_path, &len);
        if (manifest_str) {
            json_t *jm = json_parse(manifest_str);
            free(manifest_str);
            if (jm) {
                json_t *bins = json_get(jm, "install_bins");
                if (bins && bins->type == JSON_ARRAY) {
                    for (int i = 0; i < bins->v.arr.count; i++) {
                        json_t *bin_entry = bins->v.arr.items[i];
                        const char *bin = (bin_entry && bin_entry->type == JSON_STRING)
                                          ? bin_entry->v.s : NULL;
                        if (!bin) continue;
                        /* basename of bin path */
                        const char *bname = strrchr(bin, '/');
                        bname = bname ? bname + 1 : bin;

                        char bin_src[768], bin_dst[512];
                        snprintf(bin_src, sizeof(bin_src), "%s/files/%s", store_pkg, bin);
                        snprintf(bin_dst, sizeof(bin_dst), "/usr/local/bin/%s", bname);

                        unlink(bin_dst);
                        if (symlink(bin_src, bin_dst) == 0) {
                            chmod(bin_src, 0755);
                        }
                    }
                }
                json_free(jm);
            }
        }
    }

    return WARP_OK;
}

int store_activate(const char *name, const char *hash12) {
    return activate_impl(name, hash12, 1);
}

/* ── remove package ──────────────────────────────────────────── */
int store_remove(const char *name) {
    char active_link[512];
    snprintf(active_link, sizeof(active_link), WARP_STORE_DIR "/active/%s", name);

    if (!path_exists(active_link)) {
        warp_err("Not installed: %s", name);
        return WARP_ERR_NOENT;
    }

    /* Remove /usr/local/bin symlinks pointing into this package */
    char store_pkg[512] = {0};
    ssize_t n = readlink(active_link, store_pkg, sizeof(store_pkg)-1);
    if (n > 0) {
        store_pkg[n] = '\0';
        DIR *d = opendir("/usr/local/bin");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char lpath[512], target[512] = {0};
                snprintf(lpath, sizeof(lpath), "/usr/local/bin/%s", ent->d_name);
                ssize_t m = readlink(lpath, target, sizeof(target)-1);
                if (m > 0) {
                    target[m] = '\0';
                    if (strncmp(target, store_pkg, strlen(store_pkg)) == 0)
                        unlink(lpath);
                }
            }
            closedir(d);
        }
    }

    unlink(active_link);
    char prev_link[512], hist[512];
    snprintf(prev_link, sizeof(prev_link), WARP_STORE_DIR "/prev/%s", name);
    snprintf(hist,      sizeof(hist),      WARP_STORE_DIR "/history/%s", name);
    unlink(prev_link);
    unlink(hist);
    store_pin_clear(name);       /* the store entries stay until `warp gc` */
    return WARP_OK;
}

/* ── installed versions of one package ───────────────────────── */
static int is_hex12(const char *s) {
    if (strlen(s) != 12) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

/* Split "<name>-<hash12>"; returns 1 and fills name/hash when it has that shape. */
int store_split_dir(const char *dirname, char *name, size_t nsz, char *hash12) {
    size_t len = strlen(dirname);
    if (len < 14 || dirname[len - 13] != '-' || !is_hex12(dirname + len - 12)) return 0;
    size_t nl = len - 13;
    if (nl >= nsz) return 0;
    memcpy(name, dirname, nl);
    name[nl] = '\0';
    memcpy(hash12, dirname + len - 12, 13);
    return 1;
}

static void manifest_version(const char *store_dir, char *out, size_t sz) {
    char path[768];
    snprintf(path, sizeof(path), "%s/manifest.json", store_dir);
    snprintf(out, sz, "?");
    size_t len;
    char *ms = read_file(path, &len);
    if (!ms) return;
    json_t *jm = json_parse(ms);
    free(ms);
    if (jm) { snprintf(out, sz, "%s", json_str(jm, "version", "?")); json_free(jm); }
}

static int active_hash(const char *name, char hash12[13]) {
    char link[512], target[512] = {0};
    snprintf(link, sizeof(link), WARP_STORE_DIR "/active/%s", name);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0) return 0;
    target[n] = '\0';
    const char *dash = strrchr(target, '-');
    if (!dash || !is_hex12(dash + 1)) return 0;
    memcpy(hash12, dash + 1, 13);
    return 1;
}

int store_versions(const char *name, warp_version_t **out, int *count) {
    *out = NULL; *count = 0;
    if (!store_name_ok(name)) return WARP_ERR_INVAL;
    DIR *d = opendir(WARP_STORE_DIR "/store");
    if (!d) return WARP_OK;
    int cap = 8;
    warp_version_t *v = malloc(cap * sizeof(*v));
    char act[13] = "";
    int have_act = active_hash(name, act);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        char nm[WARP_MAX_NAME], h[13];
        if (!store_split_dir(ent->d_name, nm, sizeof(nm), h) || strcmp(nm, name) != 0) continue;
        if (*count >= cap) { cap *= 2; v = realloc(v, cap * sizeof(*v)); }
        warp_version_t *e = &v[(*count)++];
        memcpy(e->hash12, h, 13);
        snprintf(e->path, sizeof(e->path), WARP_STORE_DIR "/store/%s", ent->d_name);
        manifest_version(e->path, e->version, sizeof(e->version));
        e->active = have_act && strcmp(act, h) == 0;
    }
    closedir(d);
    *out = v;
    return WARP_OK;
}

/* Activate any installed version, by version string or hash prefix. */
int store_switch(const char *name, const char *spec) {
    warp_version_t *v; int n;
    if (store_versions(name, &v, &n) != WARP_OK) { warp_err("Bad package name: %s", name); return WARP_ERR_INVAL; }
    int match = -1, matches = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i].version, spec) == 0 || (strlen(spec) >= 4 && strncmp(v[i].hash12, spec, strlen(spec)) == 0)) {
            if (match < 0 || !v[match].active) match = i;
            matches++;
        }
    }
    if (matches == 0) {
        warp_err("%s %s is not in the store. Installed versions:", name, spec);
        for (int i = 0; i < n; i++) warp_err("  %s (%s)%s", v[i].version, v[i].hash12, v[i].active ? "  active" : "");
        if (n == 0) warp_err("  none");
        warp_err("Fetch it with: warp install %s@%s", name, spec);
        free(v);
        return WARP_ERR_NOENT;
    }
    if (matches > 1) {
        warp_err("%s %s matches several builds; pick one by its id:", name, spec);
        for (int i = 0; i < n; i++)
            if (strcmp(v[i].version, spec) == 0) warp_err("  %s (%s)", v[i].version, v[i].hash12);
        free(v);
        return WARP_ERR_INVAL;
    }
    char hash[13], ver[WARP_MAX_NAME];
    memcpy(hash, v[match].hash12, 13);
    snprintf(ver, sizeof(ver), "%s", v[match].version);
    int already = v[match].active;
    free(v);
    if (already) { warp_info("%s %s is already active", name, ver); return WARP_OK; }

    int rc = activate_impl(name, hash, 1);
    if (rc == WARP_OK) {
        char pv[WARP_MAX_NAME];
        if (store_pin_get(name, pv, sizeof(pv))) store_pin_set(name, ver);   /* a pin follows an explicit choice */
    }
    return rc;
}

/* A pin means "this version, on purpose": it moves with an explicit switch or
 * rollback instead of silently pointing at a version that is no longer used. */
static void pin_follow(const char *name) {
    char pv[WARP_MAX_NAME], h[13], ver[WARP_MAX_NAME], dir[512];
    if (!store_pin_get(name, pv, sizeof(pv)) || !active_hash(name, h)) return;
    snprintf(dir, sizeof(dir), WARP_STORE_DIR "/store/%s-%s", name, h);
    manifest_version(dir, ver, sizeof(ver));
    store_pin_set(name, ver);
}

/* ── roll back along the activation history ──────────────────── */
int store_rollback(const char *name) {
    if (!store_name_ok(name)) { warp_err("Bad package name: %s", name); return WARP_ERR_INVAL; }
    char active_link[512], prev_link[512];
    snprintf(active_link, sizeof(active_link), WARP_STORE_DIR "/active/%s", name);
    snprintf(prev_link,   sizeof(prev_link),   WARP_STORE_DIR "/prev/%s",   name);

    /* History first: every call goes one version further back. Entries whose
     * store directory is gone (warp gc) are skipped. */
    char hs[HIST_MAX][13], keep[HIST_MAX][13];
    int hn = store_history(name, hs, HIST_MAX), kn = 0;
    char cur[13] = "";
    int have_cur = active_hash(name, cur);
    for (int i = 0; i < hn; i++) {
        char dir[512];
        snprintf(dir, sizeof(dir), WARP_STORE_DIR "/store/%s-%s", name, hs[i]);
        if (path_exists(dir)) memcpy(keep[kn++], hs[i], 13);
    }
    if (have_cur && kn >= 2 && strcmp(keep[kn - 1], cur) == 0) {
        char target[13];
        memcpy(target, keep[kn - 2], 13);
        hist_write(name, keep, kn - 1);                 /* drop the version we leave */
        int rc = activate_impl(name, target, 0);
        if (rc == WARP_OK) pin_follow(name);
        return rc;
    }

    /* Only a package whose history was never written falls back to the old
     * single-slot link; with a history, its beginning is the end. */
    if (hn > 0 || !path_exists(prev_link)) {
        warp_err("No previous version for: %s", name);
        return WARP_ERR_NOENT;
    }

    char prev_target[512] = {0};
    ssize_t n = readlink(prev_link, prev_target, sizeof(prev_target)-1);
    if (n <= 0) return WARP_ERR_IO;
    prev_target[n] = '\0';

    /* Swap active ↔ prev */
    char active_target[512] = {0};
    n = readlink(active_link, active_target, sizeof(active_target)-1);

    unlink(active_link);
    symlink(prev_target, active_link);

    unlink(prev_link);
    if (n > 0) {
        active_target[n] = '\0';
        symlink(active_target, prev_link);
    }

    /* Reactivate bins */
    const char *hash12 = strrchr(prev_target, '-');
    if (hash12) {
        char one[1][13];
        memcpy(one[0], hash12 + 1, 13);
        hist_write(name, one, 1);
        activate_impl(name, hash12 + 1, 0);
        pin_follow(name);
    }

    return WARP_OK;
}

/* ── list installed packages ─────────────────────────────────── */
int store_list(warp_installed_t **out, int *count) {
    *out = NULL; *count = 0;
    DIR *d = opendir(WARP_STORE_DIR "/active");
    if (!d) return WARP_OK;  /* no packages installed yet */

    int cap = 16;
    warp_installed_t *list = malloc(cap * sizeof(warp_installed_t));

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char link[512], target[512] = {0};
        snprintf(link, sizeof(link), WARP_STORE_DIR "/active/%s", ent->d_name);
        ssize_t n = readlink(link, target, sizeof(target)-1);
        if (n <= 0) continue;
        target[n] = '\0';

        if (*count >= cap) { cap *= 2; list = realloc(list, cap * sizeof(warp_installed_t)); }
        warp_installed_t *pkg = &list[*count];
        strncpy(pkg->name, ent->d_name, WARP_MAX_NAME-1);
        strncpy(pkg->store_path, target, sizeof(pkg->store_path)-1);

        /* Extract hash12 from store path: .../name-hash12 */
        const char *dash = strrchr(target, '-');
        strncpy(pkg->hash12, dash ? dash + 1 : "?", 12);
        pkg->hash12[12] = '\0';

        /* Read version from manifest */
        char manifest[512];
        snprintf(manifest, sizeof(manifest), "%s/manifest.json", target);
        size_t mlen;
        char *ms = read_file(manifest, &mlen);
        if (ms) {
            json_t *jm = json_parse(ms);
            free(ms);
            if (jm) {
                const char *v = json_str(jm, "version", "?");
                strncpy(pkg->version, v, WARP_MAX_NAME-1);
                json_free(jm);
            }
        } else {
            strcpy(pkg->version, "?");
        }

        (*count)++;
    }
    closedir(d);
    *out = list;
    return WARP_OK;
}

int store_is_installed(const char *name, warp_installed_t *info) {
    char link[512];
    snprintf(link, sizeof(link), WARP_STORE_DIR "/active/%s", name);
    if (!path_exists(link)) return 0;
    if (info) {
        strncpy(info->name, name, WARP_MAX_NAME-1);
        char target[512] = {0};
        ssize_t n = readlink(link, target, sizeof(target)-1);
        if (n > 0) {
            target[n] = '\0';
            strncpy(info->store_path, target, sizeof(info->store_path)-1);
            const char *dash = strrchr(target, '-');
            strncpy(info->hash12, dash ? dash+1 : "?", 12);
            info->hash12[12] = '\0';
        }
    }
    return 1;
}

void store_free_list(warp_installed_t *list, int count) {
    (void)count;
    free(list);
}
