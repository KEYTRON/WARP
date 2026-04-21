#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "warp.h"

#define INDEX_CACHE  WARP_STORE_DIR "/index.json"
#define INDEX_SIG_CACHE  WARP_STORE_DIR "/index.sig"
#define INDEX_MAX_AGE 3600   /* refresh if older than 1 hour */

extern char *warp_download_str(const char *url);
extern int   warp_verify_index_sig(const char *json, const char *sig_b64);

static int kind_rank(const char *kind) {
    if (!kind || !*kind) return 50;
    if (strcmp(kind, "torrent") == 0) return 0;
    if (strcmp(kind, "p2p") == 0) return 5;
    if (strcmp(kind, "magnet") == 0) return 10;
    if (strcmp(kind, "direct") == 0) return 20;
    if (strcmp(kind, "https") == 0) return 25;
    if (strcmp(kind, "http") == 0) return 30;
    if (strcmp(kind, "file") == 0) return 35;
    return 40;
}

static void parse_variant(json_t *node, warp_release_variant_t *out) {
    memset(out, 0, sizeof(*out));
    if (!node || node->type != JSON_OBJECT) return;
    strncpy(out->kind, json_str(node, "kind", "direct"), sizeof(out->kind) - 1);
    strncpy(out->url, json_str(node, "url", ""), sizeof(out->url) - 1);
    strncpy(out->sha256, json_str(node, "sha256", ""), sizeof(out->sha256) - 1);
    strncpy(out->signature, json_str(node, "signature", ""), sizeof(out->signature) - 1);
    out->size = (size_t)json_num(node, "size", 0);
    out->priority = (int)json_num(node, "priority", 0);
}

static int index_is_stale(void) {
    struct stat st;
    if (stat(INDEX_CACHE, &st) != 0) return 1;
    return (time(NULL) - st.st_mtime) > INDEX_MAX_AGE;
}

static int index_fetch_and_cache(void) {
    warp_info("Fetching index from %s ...", WARP_INDEX_URL);
    char *data = warp_download_str(WARP_INDEX_URL);
    if (!data) {
        warp_err("Failed to fetch index");
        return WARP_ERR_NET;
    }
    /* Write to cache */
    if (mkdirs(WARP_STORE_DIR, 0755) != WARP_OK) {
        free(data);
        return WARP_ERR_IO;
    }
    FILE *f = fopen(INDEX_CACHE, "w");
    if (!f) { free(data); return WARP_ERR_IO; }
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    free(data);

    char sig_url[1024];
    snprintf(sig_url, sizeof(sig_url), "%s.sig", WARP_INDEX_URL);
    char *sig = warp_download_str(sig_url);
    if (sig) {
        FILE *sf = fopen(INDEX_SIG_CACHE, "w");
        if (sf) {
            fwrite(sig, 1, strlen(sig), sf);
            fclose(sf);
        }
        free(sig);
    }
    return WARP_OK;
}

static int index_parse(const char *json_src, warp_index_t *idx) {
    json_t *root = json_parse(json_src);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        return WARP_ERR_JSON;
    }

    /* Verify signature before parsing content */
    const char *sig = json_str(root, "signature", NULL);
    char *sig_cache = NULL;
    if (!sig) {
        size_t sig_len = 0;
        sig_cache = read_file(INDEX_SIG_CACHE, &sig_len);
        sig = sig_cache;
    }
    if (sig && *sig) {
        if (warp_verify_index_sig(json_src, sig) != WARP_OK) {
            warp_err("Index signature verification failed!");
            free(sig_cache);
            json_free(root);
            return WARP_ERR_SIG;
        }
    } else {
        warp_warn("Index has no signature (unsigned)");
    }

    strncpy(idx->signature,  sig ? sig : "", sizeof(idx->signature)-1);
    const char *ts = json_str(root, "timestamp", "");
    strncpy(idx->timestamp, ts, sizeof(idx->timestamp)-1);

    json_t *pkgs = json_get(root, "packages");
    if (!pkgs || pkgs->type != JSON_OBJECT) {
        json_free(root);
        return WARP_ERR_JSON;
    }

    idx->capacity = pkgs->v.arr.count + 4;
    idx->entries  = malloc(idx->capacity * sizeof(warp_pkg_entry_t));
    idx->count    = 0;

    for (int i = 0; i < pkgs->v.arr.count; i++) {
        json_t *pkg = pkgs->v.arr.items[i];
        if (!pkg || pkg->type != JSON_OBJECT || !pkg->key) continue;

        warp_pkg_entry_t *e = &idx->entries[idx->count++];
        memset(e, 0, sizeof(*e));

        strncpy(e->name,        pkg->key,                          WARP_MAX_NAME-1);
        strncpy(e->version,     json_str(pkg, "version",     "?"), WARP_MAX_NAME-1);
        strncpy(e->description, json_str(pkg, "description", ""),  sizeof(e->description)-1);
        strncpy(e->sha256,      json_str(pkg, "sha256",      ""),  WARP_SHA256_HEX-1);
        strncpy(e->url,         json_str(pkg, "url",         ""),  WARP_MAX_URL-1);
        e->size = (size_t)json_num(pkg, "size", 0);
        strncpy(e->signature,   json_str(pkg, "signature", ""),  sizeof(e->signature)-1);

        json_t *variants = json_get(pkg, "variants");
        if (!variants) variants = json_get(pkg, "mirrors");
        if (variants && variants->type == JSON_ARRAY) {
            for (int j = 0; j < variants->v.arr.count && e->variants_count < WARP_MAX_VARIANTS; j++) {
                parse_variant(variants->v.arr.items[j], &e->variants[e->variants_count++]);
            }
        }
    }

    json_free(root);
    free(sig_cache);
    return WARP_OK;
}

int index_load(warp_index_t *idx, int force_refresh) {
    memset(idx, 0, sizeof(*idx));

    if (force_refresh || index_is_stale()) {
        int rc = index_fetch_and_cache();
        if (rc != WARP_OK) {
            if (!path_exists(INDEX_CACHE)) return rc;
            warp_warn("Using cached index (fetch failed)");
        }
    }

    size_t len;
    char *data = read_file(INDEX_CACHE, &len);
    if (!data) return WARP_ERR_IO;

    int rc = index_parse(data, idx);
    free(data);
    return rc;
}

int index_find(const warp_index_t *idx, const char *name, warp_pkg_entry_t *out) {
    for (int i = 0; i < idx->count; i++) {
        if (strcmp(idx->entries[i].name, name) == 0) {
            if (out) *out = idx->entries[i];
            return WARP_OK;
        }
    }
    return WARP_ERR_NOENT;
}

int index_search(const warp_index_t *idx, const char *query,
                  warp_pkg_entry_t **results, int *count) {
    *results = malloc(idx->count * sizeof(warp_pkg_entry_t));
    *count = 0;
    for (int i = 0; i < idx->count; i++) {
        const warp_pkg_entry_t *e = &idx->entries[i];
        if (strcasestr(e->name, query) || strcasestr(e->description, query)) {
            (*results)[(*count)++] = *e;
        }
    }
    return WARP_OK;
}

int index_pick_variant(const warp_pkg_entry_t *entry, const char **reason,
                       warp_release_variant_t *out) {
    if (!entry || !out) return WARP_ERR_INVAL;

    warp_release_variant_t best;
    memset(&best, 0, sizeof(best));
    best.priority = -1;

    const char *preferred = getenv("WARP_PREFERRED_VARIANTS");
    if (!preferred || !*preferred) preferred = "torrent,p2p,magnet,direct,https,http,file";

    char order_buf[256];
    snprintf(order_buf, sizeof(order_buf), "%s", preferred);

    int best_rank = 999;
    int best_prio = -999999;

    char *saveptr = NULL;
    for (char *tok = strtok_r(order_buf, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
        for (int i = 0; i < entry->variants_count; i++) {
            const warp_release_variant_t *v = &entry->variants[i];
            if (*v->kind && strcmp(v->kind, tok) != 0) continue;
            int rank = kind_rank(v->kind);
            if (rank < best_rank || (rank == best_rank && v->priority > best_prio)) {
                best = *v;
                best_rank = rank;
                best_prio = v->priority;
            }
        }
    }

    if (best_rank == 999) {
        if (entry->url[0]) {
            strncpy(best.kind, "direct", sizeof(best.kind) - 1);
            strncpy(best.url, entry->url, sizeof(best.url) - 1);
            strncpy(best.sha256, entry->sha256, sizeof(best.sha256) - 1);
            strncpy(best.signature, entry->signature, sizeof(best.signature) - 1);
            best.size = entry->size;
            best.priority = 0;
            best_rank = kind_rank(best.kind);
            if (reason) *reason = "legacy fields";
        } else {
            return WARP_ERR_NOENT;
        }
    } else if (reason) {
        *reason = "selected";
    }

    *out = best;
    return WARP_OK;
}

void index_free(warp_index_t *idx) {
    free(idx->entries);
    memset(idx, 0, sizeof(*idx));
}
