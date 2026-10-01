/* Package index: one signed index.json per repository, merged in repository
 * order. Only an index whose detached signature verifies with that
 * repository's key is ever cached or trusted. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "warp.h"

#define INDEX_MAX_AGE 3600   /* refresh if older than 1 hour */

static void cache_paths(const warp_repo_t *r, char *idx, size_t idx_cap, char *sig, size_t sig_cap) {
    char dir[512];
    repo_cache_dir(r, dir, sizeof(dir));
    snprintf(idx, idx_cap, "%s/index.json", dir);
    snprintf(sig, sig_cap, "%s/index.json.sig", dir);
}

static int is_stale(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 1;
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
static int fetch_and_cache(const warp_repo_t *r) {
    char url[WARP_MAX_URL + 32];
    int saw_unverified = 0;

    for (int i = 0; i < r->mirror_count; i++) {
        warp_info("[%s] Fetching index from mirror %d ...", r->name, i + 1);
        snprintf(url, sizeof(url), "%s/index.json", r->mirrors[i]);
        char *data = warp_download_str(url);
        if (!data) continue;

        snprintf(url, sizeof(url), "%s/index.json.sig", r->mirrors[i]);
        char *sig = warp_download_str(url);
        if (!sig || !*sig) {
            warp_warn("[%s] Mirror %d serves an unsigned index — skipping it", r->name, i + 1);
            free(data);
            free(sig);
            saw_unverified = 1;
            continue;
        }
        trim_sig(sig);
        if (warp_verify_sig_with_key(data, sig, r->pubkey) != WARP_OK) {
            warp_warn("[%s] Mirror %d: index signature does not verify (stale or tampered) — skipping it", r->name, i + 1);
            free(data);
            free(sig);
            saw_unverified = 1;
            continue;
        }

        char dir[512], idx_path[600], sig_path[600];
        repo_cache_dir(r, dir, sizeof(dir));
        cache_paths(r, idx_path, sizeof(idx_path), sig_path, sizeof(sig_path));
        if (mkdirs(dir, 0755) != WARP_OK) {
            warp_err("Failed to create %s (Permission denied?)", dir);
            free(data);
            free(sig);
            return WARP_ERR_IO;
        }
        /* Signature first: a crash between the two writes must never
         * leave a new index next to the previous index's signature. */
        remove(sig_path);
        int rc = write_file(idx_path, data);
        if (rc == WARP_OK) rc = write_file(sig_path, sig);
        free(data);
        free(sig);
        if (rc != WARP_OK) {
            warp_err("Failed to write %s (Permission denied?). Run with sudo!", idx_path);
            return WARP_ERR_IO;
        }
        return WARP_OK;
    }

    if (saw_unverified) {
        warp_err("[%s] No mirror served a correctly signed index", r->name);
        return WARP_ERR_SIG;
    }
    warp_err("[%s] Failed to fetch index from all mirrors", r->name);
    return WARP_ERR_NET;
}

/* Two-letter language from a value like "ru_RU.UTF-8" or "de:en"; false when empty. */
static int lang_prefix(const char *v, char out[8]) {
    size_t n = 0;
    while (n < 7 && v[n] && v[n] != '_' && v[n] != '.' && v[n] != ':' && v[n] != '@' && v[n] != '-')
        { out[n] = (char)((v[n] >= 'A' && v[n] <= 'Z') ? v[n] + 32 : v[n]); n++; }
    out[n] = '\0';
    return n > 0;
}

/* Language of the user interface, the way gettext decides it: WARP_LANG wins; the locale
 * (LC_ALL, LC_MESSAGES, LANG) says C/POSIX = English whatever else is set; otherwise the
 * LANGUAGE list, then the locale's own language. "en" when nothing is set. */
const char *warp_ui_lang(void) {
    static char lang[8];
    static int done;
    if (done) return lang;
    done = 1;
    snprintf(lang, sizeof(lang), "en");

    const char *forced = getenv("WARP_LANG");
    if (forced && lang_prefix(forced, lang)) return lang;
    snprintf(lang, sizeof(lang), "en");

    const char *loc = getenv("LC_ALL");
    if (!loc || !*loc) loc = getenv("LC_MESSAGES");
    if (!loc || !*loc) loc = getenv("LANG");
    if (loc && (strcmp(loc, "C") == 0 || strcmp(loc, "POSIX") == 0)) return lang;

    const char *list = getenv("LANGUAGE");
    if (list && lang_prefix(list, lang)) return lang;
    if (loc && lang_prefix(loc, lang)) return lang;
    snprintf(lang, sizeof(lang), "en");
    return lang;
}

/* The part of an entry that differs per build: version, archive, deltas, deps.
 * `src` is the build object (or the entry itself for a legacy index);
 * `top` supplies what a build leaves out. */
static void parse_build(json_t *src, json_t *top, warp_pkg_entry_t *e) {
    strncpy(e->version, json_str(src, "version", json_str(top, "version", "?")), WARP_MAX_NAME-1);
    strncpy(e->sha256,  json_str(src, "sha256", ""), WARP_SHA256_HEX-1);
    strncpy(e->url,     json_str(src, "url",    ""), WARP_MAX_URL-1);
    e->size = (size_t)json_num(src, "size", 0);

    json_t *deltas = json_get(src, "deltas");
    if (deltas && deltas->type == JSON_ARRAY) {
        for (int i = 0; i < deltas->v.arr.count && e->delta_count < WARP_MAX_DELTAS; i++) {
            json_t *d = deltas->v.arr.items[i];
            if (!d || d->type != JSON_OBJECT) continue;
            warp_delta_ref_t *ref = &e->deltas[e->delta_count];
            strncpy(ref->from_version, json_str(d, "from_version", ""), WARP_MAX_NAME-1);
            strncpy(ref->from_sha256,  json_str(d, "from_sha256",  ""), WARP_SHA256_HEX-1);
            strncpy(ref->url,          json_str(d, "url",          ""), WARP_MAX_URL-1);
            strncpy(ref->sha256,       json_str(d, "sha256",       ""), WARP_SHA256_HEX-1);
            ref->size = (size_t)json_num(d, "size", 0);
            if (ref->from_sha256[0] && ref->url[0] && ref->sha256[0]) e->delta_count++;
        }
    }

    json_t *deps = json_get(src, "deps");
    if (!deps) deps = json_get(top, "deps");
    if (deps && deps->type == JSON_ARRAY) {
        for (int i = 0; i < deps->v.arr.count && e->dep_count < WARP_MAX_ENTRY_DEPS; i++) {
            json_t *d = deps->v.arr.items[i];
            if (!d) continue;
            warp_dep_ref_t *dep = &e->deps[e->dep_count];
            if (d->type == JSON_STRING) {
                strncpy(dep->name, d->v.s, WARP_MAX_NAME-1);
            } else if (d->type == JSON_OBJECT) {
                strncpy(dep->name,    json_str(d, "name",    ""), WARP_MAX_NAME-1);
                strncpy(dep->version, json_str(d, "version", ""), WARP_MAX_NAME-1);
                strncpy(dep->repo,    json_str(d, "repo",    ""), WARP_REPO_NAME-1);
            }
            if (dep->name[0]) e->dep_count++;
        }
    }
}

/* Pick the build made for this machine. Only a build for this very platform
 * (or an explicit "any" for architecture-independent packages) is used: a
 * binary for another OS or CPU is never installed, and nothing is built here. */
static void parse_entry(json_t *pkg, const warp_repo_t *r, warp_pkg_entry_t *e) {
    memset(e, 0, sizeof(*e));
    /* "name" is the latest release, "name@1.2" an older one kept for pinning. */
    const char *at = strchr(pkg->key, '@');
    size_t nl = at ? (size_t)(at - pkg->key) : strlen(pkg->key);
    if (nl >= WARP_MAX_NAME) nl = WARP_MAX_NAME - 1;
    memcpy(e->name, pkg->key, nl);
    e->versioned = at != NULL;
    strncpy(e->description, json_str(pkg, "description", ""),  sizeof(e->description)-1);
    strncpy(e->repo,        r->name,                           WARP_REPO_NAME-1);
    /* The description in the user's language, when the index has one (English is the default). */
    json_t *tr = json_get(pkg, "descriptions");
    if (tr && tr->type == JSON_OBJECT && strcmp(warp_ui_lang(), "en") != 0) {
        const char *t = json_str(tr, warp_ui_lang(), "");
        if (*t) strncpy(e->description, t, sizeof(e->description)-1);
    }

    json_t *builds = json_get(pkg, "builds");
    if (builds && builds->type == JSON_OBJECT) {
        json_t *mine = NULL;
        const char *cand[4];
        int nc = warp_platform_candidates(cand, 4);
        for (int c = 0; c < nc && !mine; c++) {
            mine = json_get(builds, cand[c]);
            if (mine && mine->type != JSON_OBJECT) mine = NULL;
            else if (mine) snprintf(e->platform, sizeof(e->platform), "%s", cand[c]);
        }
        if (!mine) {
            mine = json_get(builds, "any");
            if (mine) snprintf(e->platform, sizeof(e->platform), "any");
        }
        if (mine && mine->type == JSON_OBJECT) {
            parse_build(mine, pkg, e);
            return;
        }
        for (int i = 0; i < builds->v.arr.count; i++) {
            json_t *b = builds->v.arr.items[i];
            if (!b || !b->key) continue;
            size_t used = strlen(e->available);
            snprintf(e->available + used, sizeof(e->available) - used, "%s%s", used ? ", " : "", b->key);
        }
    } else if (strcmp(warp_platform(), "linux-x86_64") == 0) {
        snprintf(e->platform, sizeof(e->platform), "linux-x86_64");
        parse_build(pkg, pkg, e);          /* legacy index: the entry is the x86_64 build */
        return;
    } else {
        snprintf(e->available, sizeof(e->available), "linux-x86_64");
    }

    e->no_build = 1;
    strncpy(e->version, json_str(pkg, "version", "?"), WARP_MAX_NAME-1);
}

/* Parse one repository's cached index (re-verifying the cached pair) and
 * append its packages; a name already provided by an earlier repository
 * wins, so repository order is priority order. */
static int parse_repo(warp_repo_t *r, warp_index_t *idx) {
    char idx_path[600], sig_path[600];
    cache_paths(r, idx_path, sizeof(idx_path), sig_path, sizeof(sig_path));

    size_t len;
    char *data = read_file(idx_path, &len);
    if (!data) return WARP_ERR_IO;
    size_t sig_len = 0;
    char *sig = read_file(sig_path, &sig_len);
    if (sig) trim_sig(sig);
    if (!sig || !*sig || warp_verify_sig_with_key(data, sig, r->pubkey) != WARP_OK) {
        warp_err("[%s] Cached index has no valid signature — refusing to trust it", r->name);
        free(sig);
        free(data);
        return WARP_ERR_SIG;
    }
    free(sig);

    json_t *root = json_parse(data);
    free(data);
    if (!root || root->type != JSON_OBJECT) {
        json_free(root);
        return WARP_ERR_JSON;
    }
    if (!idx->timestamp[0])
        strncpy(idx->timestamp, json_str(root, "timestamp", ""), sizeof(idx->timestamp)-1);
    strncpy(r->peer_list_url, json_str(root, "peer_list_url", ""), WARP_MAX_URL-1);

    json_t *pkgs = json_get(root, "packages");
    if (!pkgs || pkgs->type != JSON_OBJECT) {
        json_free(root);
        return WARP_ERR_JSON;
    }

    int need = idx->count + pkgs->v.arr.count;
    if (need > idx->capacity) {
        idx->capacity = need + 8;
        idx->entries = realloc(idx->entries, idx->capacity * sizeof(warp_pkg_entry_t));
    }
    for (int i = 0; i < pkgs->v.arr.count; i++) {
        json_t *pkg = pkgs->v.arr.items[i];
        if (!pkg || pkg->type != JSON_OBJECT || !pkg->key) continue;
        if (index_find(idx, pkg->key, NULL) == WARP_OK) continue;   /* earlier repo wins */
        parse_entry(pkg, r, &idx->entries[idx->count++]);
    }
    json_free(root);
    return WARP_OK;
}

int index_load(warp_index_t *idx, int force_refresh) {
    memset(idx, 0, sizeof(*idx));
    repos_load(idx->repos, &idx->repo_count);

    int loaded = 0;
    for (int i = 0; i < idx->repo_count; i++) {
        warp_repo_t *r = &idx->repos[i];
        if (!r->enabled) continue;
        char idx_path[600], sig_path[600];
        cache_paths(r, idx_path, sizeof(idx_path), sig_path, sizeof(sig_path));
        if (force_refresh || is_stale(idx_path)) {
            int rc = fetch_and_cache(r);
            if (rc != WARP_OK) {
                idx->refresh_failed++;
                if (!path_exists(idx_path)) continue;
                warp_warn("[%s] Using cached index (fetch failed)", r->name);
            }
        }
        if (parse_repo(r, idx) == WARP_OK) loaded++;
    }
    return loaded ? WARP_OK : WARP_ERR_NET;
}

/* `name`, `repo/name`, `name@version` or `repo/name@version`. A repository
 * prefix selects the entry from one repository even when an earlier one
 * publishes the same package name; a version selects that release instead of
 * the latest. */
int index_find(const warp_index_t *idx, const char *name, warp_pkg_entry_t *out) {
    const char *slash = strchr(name, '/');
    char repo[64] = "";
    if (slash) {
        size_t n = (size_t)(slash - name);
        if (n == 0 || n >= sizeof(repo) || !slash[1]) return WARP_ERR_NOENT;
        memcpy(repo, name, n);
        repo[n] = '\0';
        name = slash + 1;
    }
    char base[WARP_MAX_NAME];
    const char *ver = NULL;
    const char *at = strchr(name, '@');
    size_t bl = at ? (size_t)(at - name) : strlen(name);
    if (bl == 0 || bl >= sizeof(base)) return WARP_ERR_NOENT;
    memcpy(base, name, bl);
    base[bl] = '\0';
    if (at) {
        ver = at + 1;
        if (!*ver) return WARP_ERR_NOENT;
    }
    for (int i = 0; i < idx->count; i++) {
        const warp_pkg_entry_t *e = &idx->entries[i];
        if (repo[0] && strcmp(e->repo, repo) != 0) continue;
        if (strcmp(e->name, base) != 0) continue;
        if (ver ? strcmp(e->version, ver) != 0 : e->versioned) continue;
        if (out) *out = *e;
        return WARP_OK;
    }
    return WARP_ERR_NOENT;
}

const warp_repo_t *index_repo_of(const warp_index_t *idx, const warp_pkg_entry_t *e) {
    const warp_repo_t *r = repos_find(idx->repos, idx->repo_count, e->repo);
    return r ? r : &idx->repos[0];
}

int index_search(const warp_index_t *idx, const char *query,
                  warp_pkg_entry_t **results, int *count) {
    *results = malloc((idx->count ? idx->count : 1) * sizeof(warp_pkg_entry_t));
    *count = 0;
    for (int i = 0; i < idx->count; i++) {
        const warp_pkg_entry_t *e = &idx->entries[i];
        if (e->versioned) continue;      /* older releases show up in `warp versions` */
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

/* Part of an archive's name after name-version-: the architecture on Linux (the
 * names that already exist), `<os>_<arch>` on any other OS. */
const char *warp_archive_tag(void) {
    static char tag[64];
    if (strcmp(WARP_OS, "linux") == 0) snprintf(tag, sizeof(tag), "%s", WARP_ARCH);
    else                               snprintf(tag, sizeof(tag), "%s_%s", WARP_OS, WARP_ARCH);
    return tag;
}
