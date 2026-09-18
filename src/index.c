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

static void trim_sig(char *sig) {
    size_t n = strlen(sig);
    while (n > 0 && (sig[n-1] == '\n' || sig[n-1] == '\r' || sig[n-1] == ' '))
        sig[--n] = '\0';
}

static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return WARP_ERR_IO;
    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return WARP_OK;
}

/* Fetch index.json and its detached index.json.sig from the same mirror
 * and verify the pair in memory before anything touches the cache. A
 * mirror that is stale, unsigned or tampered is skipped for the next
 * one; mixing an index from one mirror with a signature from another
 * can never verify and only hides which mirror is broken. */
static int index_fetch_and_cache(void) {
    char url[512];
    int saw_unverified = 0;

    for (int i = 0; i < WARP_INDEX_MIRRORS; i++) {
        warp_info("Fetching index from mirror %d ...", i + 1);
        snprintf(url, sizeof(url), "%s/index.json", g_warp_mirrors[i]);
        char *data = warp_download_str(url);
        if (!data) continue;

        snprintf(url, sizeof(url), "%s/index.json.sig", g_warp_mirrors[i]);
        char *sig = warp_download_str(url);
        if (!sig || !*sig) {
            warp_warn("Mirror %d serves an unsigned index — skipping it", i + 1);
            free(data);
            free(sig);
            saw_unverified = 1;
            continue;
        }
        trim_sig(sig);
        if (warp_verify_index_sig(data, sig) != WARP_OK) {
            warp_warn("Mirror %d: index signature does not verify (stale or tampered) — skipping it", i + 1);
            free(data);
            free(sig);
            saw_unverified = 1;
            continue;
        }

        if (mkdirs(WARP_STORE_DIR, 0755) != WARP_OK) {
            warp_err("Failed to create store dir %s (Permission denied?)", WARP_STORE_DIR);
            free(data);
            free(sig);
            return WARP_ERR_IO;
        }
        /* Signature first: a crash between the two writes must never
         * leave a new index next to the previous index's signature. */
        remove(INDEX_SIG_CACHE);
        int rc = write_file(INDEX_CACHE, data);
        if (rc == WARP_OK) rc = write_file(INDEX_SIG_CACHE, sig);
        free(data);
        free(sig);
        if (rc != WARP_OK) {
            warp_err("Failed to write %s (Permission denied?). Run with sudo!", INDEX_CACHE);
            return WARP_ERR_IO;
        }
        return WARP_OK;
    }

    if (saw_unverified) {
        warp_err("No mirror served a correctly signed index");
        return WARP_ERR_SIG;
    }
    warp_err("Failed to fetch index from all mirrors");
    return WARP_ERR_NET;
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
    if (sig) trim_sig(sig);
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
