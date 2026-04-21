#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
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

/* ── activate a version ──────────────────────────────────────── */
int store_activate(const char *name, const char *hash12) {
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
    char prev_link[512];
    snprintf(prev_link, sizeof(prev_link), WARP_STORE_DIR "/prev/%s", name);
    unlink(prev_link);
    return WARP_OK;
}

/* ── rollback to previous version ───────────────────────────── */
int store_rollback(const char *name) {
    char active_link[512], prev_link[512];
    snprintf(active_link, sizeof(active_link), WARP_STORE_DIR "/active/%s", name);
    snprintf(prev_link,   sizeof(prev_link),   WARP_STORE_DIR "/prev/%s",   name);

    if (!path_exists(prev_link)) {
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
    if (hash12) store_activate(name, hash12 + 1);

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
