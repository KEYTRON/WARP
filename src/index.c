#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "warp.h"

#define INDEX_CACHE      WARP_STORE_DIR "/index.json"
#define INDEX_SIG_CACHE  WARP_STORE_DIR "/index.json.sig"
#define INDEX_MAX_AGE 3600   /* refresh if older than 1 hour */

extern char *warp_download_str(const char *url);
extern int   warp_verify_index_sig(const char *data, const char *sig_b64);

static int index_is_stale(void) {
    struct stat st;
    if (stat(INDEX_CACHE, &st) != 0) return 1;
    return (time(NULL) - st.st_mtime) > INDEX_MAX_AGE;
}

static int index_fetch_and_cache(void) {
    char *data = NULL;
    char url[512];
    int mirror_i = -1;
    for (int i = 0; i < WARP_INDEX_MIRRORS; i++) {
        snprintf(url, sizeof(url), "%s/index.json", g_warp_mirrors[i]);
        warp_info("Fetching index from mirror %d ...", i + 1);
        data = warp_download_str(url);
        if (data) { mirror_i = i; break; }
    }
    if (!data) {
        warp_err("Failed to fetch index from all mirrors");
        return WARP_ERR_NET;
    }

    /* Fetch the detached signature — try the mirror that served the
     * index first, then fall back to the others. A mismatched or
     * missing signature is a verification failure, not a fetch
     * failure, so it's handled later in index_parse(). */
    char *sig = NULL;
    for (int i = 0; i < WARP_INDEX_MIRRORS && !sig; i++) {
        int m = (mirror_i + i) % WARP_INDEX_MIRRORS;
        snprintf(url, sizeof(url), "%s/index.json.sig", g_warp_mirrors[m]);
        sig = warp_download_str(url);
    }

    /* Write to cache */
    if (mkdirs(WARP_STORE_DIR, 0755) != WARP_OK) {
        warp_err("Failed to create store dir %s (Permission denied?)", WARP_STORE_DIR);
        free(data);
        free(sig);
        return WARP_ERR_IO;
    }
    FILE *f = fopen(INDEX_CACHE, "w");
    if (!f) {
        warp_err("Failed to write %s (Permission denied?). Run with sudo!", INDEX_CACHE);
        free(data);
        free(sig);
        return WARP_ERR_IO;
    }
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    free(data);

    /* Always resync the cached signature with the cached index: a
     * fetch that finds no .sig must not leave a stale one behind
     * that happens to belong to a previous, different index.json. */
    if (sig) {
        FILE *sf = fopen(INDEX_SIG_CACHE, "w");
        if (sf) { fwrite(sig, 1, strlen(sig), sf); fclose(sf); }
        free(sig);
    } else {
        remove(INDEX_SIG_CACHE);
    }
    return WARP_OK;
}

static int index_parse(const char *json_src, warp_index_t *idx) {
    json_t *root = json_parse(json_src);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        return WARP_ERR_JSON;
    }

    /* Verify the detached signature (index.json.sig) before trusting
     * anything parsed out of this document. Signing the raw index.json
     * bytes — rather than a "signature" field embedded inside the same
     * JSON — avoids the self-referential trap the old scheme had: you
     * cannot verify a signature over a document that already contains
     * that signature as one of its own fields. */
    size_t sig_len = 0;
    char *sig = read_file(INDEX_SIG_CACHE, &sig_len);
    if (sig) {
        while (sig_len > 0 && (sig[sig_len-1] == '\n' || sig[sig_len-1] == '\r' || sig[sig_len-1] == ' '))
            sig[--sig_len] = '\0';
    }
    if (sig && *sig) {
        if (warp_verify_index_sig(json_src, sig) != WARP_OK) {
            warp_err("Index signature verification failed!");
            free(sig);
            json_free(root);
            return WARP_ERR_SIG;
        }
    } else {
#ifdef WARP_SKIP_SIG_VERIFY
        warp_warn("Index has no signature (unsigned) — allowed only because WARP_SKIP_SIG_VERIFY is set");
#else
        warp_err("Index has no signature (index.json.sig missing) — refusing to trust it");
        free(sig);
        json_free(root);
        return WARP_ERR_SIG;
#endif
    }

    strncpy(idx->signature,  sig ? sig : "", sizeof(idx->signature)-1);
    free(sig);
    const char *ts = json_str(root, "timestamp", "");
    strncpy(idx->timestamp, ts, sizeof(idx->timestamp)-1);
    const char *plurl = json_str(root, "peer_list_url", "");
    strncpy(idx->peer_list_url, plurl, WARP_MAX_URL-1);

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
    }

    json_free(root);
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

void index_free(warp_index_t *idx) {
    free(idx->entries);
    memset(idx, 0, sizeof(*idx));
}
