#if defined(__APPLE__)
#include <libproc.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <dirent.h>
#include <sys/statvfs.h>
#include <curl/curl.h>
#include "warp.h"

char g_volunteer_dir[512] = WARP_STORE_DIR "/volunteer";

/* ══════════════════════════════════════════════════════════════
 *  Size parser  "10G" / "500M" / "50K" → bytes
 * ══════════════════════════════════════════════════════════════ */

size_t warp_parse_size(const char *s) {
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    char *end;
    double val = strtod(s, &end);
    if (val < 0) return 0;
    while (*end == ' ') end++;
    switch (*end | 0x20) {
        case 'g': return (size_t)(val * 1073741824.0);
        case 'm': return (size_t)(val * 1048576.0);
        case 'k': return (size_t)(val * 1024.0);
        default:  return (size_t)val;
    }
}

void warp_fmt_size(size_t bytes, char *buf, size_t bufsz) {
    if      (bytes >= (size_t)1073741824) snprintf(buf, bufsz, "%.2f GB", (double)bytes / 1073741824.0);
    else if (bytes >= (size_t)1048576)    snprintf(buf, bufsz, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= (size_t)1024)       snprintf(buf, bufsz, "%.1f KB", (double)bytes / 1024.0);
    else                                  snprintf(buf, bufsz, "%zu B",    bytes);
}

/* ══════════════════════════════════════════════════════════════
 *  Seed config — load / save
 * ══════════════════════════════════════════════════════════════ */

void seed_config_defaults(warp_seed_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->serve = 1;
}

int seed_config_load(warp_seed_config_t *cfg) {
    seed_config_defaults(cfg);

    size_t len;
    char *s = read_file(WARP_SEED_CONF, &len);
    if (!s) return WARP_ERR_NOENT;

    json_t *j = json_parse(s);
    free(s);
    if (!j) return WARP_ERR_JSON;

    cfg->quota_bytes         = (size_t)json_num(j, "quota_bytes",         0);
    cfg->serve               = (int)   json_num(j, "serve",               1);
    cfg->monthly_limit_bytes = (size_t)json_num(j, "monthly_limit_bytes", 0);
    cfg->monthly_used_bytes  = (size_t)json_num(j, "monthly_used_bytes",  0);
    cfg->cheap_sd            = (int)   json_num(j, "cheap_sd",             0);
    cfg->volunteer           = (int)   json_num(j, "volunteer",            0);
    cfg->unlimited           = (int)   json_num(j, "unlimited",            0);
    cfg->max_packages        = (size_t)json_num(j, "max_packages",         0);
    cfg->reserve_bytes       = (size_t)json_num(j, "reserve_bytes",        0);
    cfg->uploaded_total      = (unsigned long long)json_num(j, "uploaded_total", 0);
    cfg->served_total        = (unsigned long long)json_num(j, "served_total",   0);
    cfg->stats_consent       = (int)   json_num(j, "stats_consent",        0);
    cfg->consent_asked       = (int)   json_num(j, "consent_asked",        0);
    cfg->consent_schema      = (int)   json_num(j, "consent_schema",       cfg->stats_consent ? 1 : 0);
    strncpy(cfg->month_tag, json_str(j, "month_tag", ""), sizeof(cfg->month_tag) - 1);
    strncpy(cfg->node_id,   json_str(j, "node_id",   ""), sizeof(cfg->node_id) - 1);
    json_free(j);

    if (cfg->cheap_sd) {
        strncpy(g_volunteer_dir, WARP_TMP_DIR "/warp-volunteer", sizeof(g_volunteer_dir) - 1);
    } else {
        strncpy(g_volunteer_dir, WARP_STORE_DIR "/volunteer", sizeof(g_volunteer_dir) - 1);
    }

    /* Reset monthly counter if calendar month rolled over */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char cur[8];
    strftime(cur, sizeof(cur), "%Y-%m", tm_info);
    if (strcmp(cfg->month_tag, cur) != 0) {
        cfg->monthly_used_bytes = 0;
        strncpy(cfg->month_tag, cur, sizeof(cfg->month_tag) - 1);
        /* Save immediately so we don't reset again next call */
        seed_config_save(cfg);
    }

    return WARP_OK;
}

int seed_config_save(const warp_seed_config_t *cfg) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"quota_bytes\": %zu,\n"
        "  \"serve\": %d,\n"
        "  \"monthly_limit_bytes\": %zu,\n"
        "  \"monthly_used_bytes\": %zu,\n"
        "  \"cheap_sd\": %d,\n"
        "  \"month_tag\": \"%s\",\n"
        "  \"volunteer\": %d,\n"
        "  \"unlimited\": %d,\n"
        "  \"max_packages\": %zu,\n"
        "  \"reserve_bytes\": %zu,\n"
        "  \"uploaded_total\": %llu,\n"
        "  \"served_total\": %llu,\n"
        "  \"stats_consent\": %d,\n"
        "  \"consent_asked\": %d,\n"
        "  \"consent_schema\": %d,\n"
        "  \"node_id\": \"%s\"\n"
        "}\n",
        cfg->quota_bytes, cfg->serve,
        cfg->monthly_limit_bytes, cfg->monthly_used_bytes,
        cfg->cheap_sd, cfg->month_tag,
        cfg->volunteer, cfg->unlimited, cfg->max_packages, cfg->reserve_bytes,
        cfg->uploaded_total, cfg->served_total,
        cfg->stats_consent, cfg->consent_asked, cfg->consent_schema, cfg->node_id);

    /* tmp + rename: a crash must not leave half a config (it holds the counters) */
    char tmp[sizeof(WARP_SEED_CONF) + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", WARP_SEED_CONF);
    FILE *f = fopen(tmp, "w");
    if (!f) return WARP_ERR_IO;
    size_t want = strlen(buf);
    int ok = fwrite(buf, 1, want, f) == want;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, WARP_SEED_CONF) != 0) { remove(tmp); return WARP_ERR_IO; }
    return WARP_OK;
}

/* ══════════════════════════════════════════════════════════════
 *  Volunteer cache helpers
 * ══════════════════════════════════════════════════════════════ */

size_t volunteer_used_bytes(void) {
    DIR *d = opendir(g_volunteer_dir);
    if (!d) return 0;
    size_t total = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t nl = strlen(ent->d_name);
        if (nl < 6 || strcmp(ent->d_name + nl - 5, ".warp") != 0) continue;
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", g_volunteer_dir, ent->d_name);
        long sz = file_size(path);
        if (sz > 0) total += (size_t)sz;
    }
    closedir(d);
    return total;
}

size_t volunteer_count(void) {
    DIR *d = opendir(g_volunteer_dir);
    if (!d) return 0;
    size_t n = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        size_t nl = strlen(ent->d_name);
        if (nl >= 6 && strcmp(ent->d_name + nl - 5, ".warp") == 0) n++;
    }
    closedir(d);
    return n;
}

static int volunteer_has_pkg(const char *sha256) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.warp", g_volunteer_dir, sha256);
    return path_exists(path);
}

static char *volunteer_find_warp_by_sha256(const char *sha256) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.warp", g_volunteer_dir, sha256);
    return path_exists(path) ? strdup(path) : NULL;
}

/* Save companion .meta file alongside volunteer .warp */
static void volunteer_write_meta(const char *sha256, const char *name,
                                  const char *version) {
    char meta[600];
    snprintf(meta, sizeof(meta), "%s/%s.meta", g_volunteer_dir, sha256);
    FILE *f = fopen(meta, "w");
    if (!f) return;
    fprintf(f, "{\"name\":\"%s\",\"version\":\"%s\"}\n", name, version);
    fclose(f);
}

/* ══════════════════════════════════════════════════════════════
 *  Global seed config (used by seed server for upload limits)
 * ══════════════════════════════════════════════════════════════ */

static warp_seed_config_t g_seed_cfg  = {0};
static int                g_have_seed_cfg = 0;
static time_t             g_last_cfg_save = 0;

/* ══════════════════════════════════════════════════════════════
 *  Peer list — fetch, cache, parse
 * ══════════════════════════════════════════════════════════════ */

static int peers_cache_stale(void) {
    struct stat st;
    if (stat(WARP_PEERS_CACHE, &st) != 0) return 1;
    return (time(NULL) - st.st_mtime) > WARP_PEERS_MAX_AGE;
}

/* peers.json format:
   { "updated": "...", "peers": ["http://1.2.3.4:7777", ...] } */
static int parse_peer_json(const char *json_src, warp_peer_list_t *out) {
    out->count = 0;
    json_t *root = json_parse(json_src);
    if (!root) return WARP_ERR_JSON;

    json_t *arr = json_get(root, "peers");
    if (!arr || arr->type != JSON_ARRAY) {
        json_free(root);
        return WARP_ERR_JSON;
    }

    for (int i = 0; i < arr->v.arr.count && out->count < WARP_PEERS_MAX; i++) {
        json_t *item = arr->v.arr.items[i];
        const char *url = NULL;
        if (item) {
            if (item->type == JSON_STRING) {
                url = item->v.s;
            } else if (item->type == JSON_OBJECT) {
                url = json_str(item, "url", NULL);
            }
        }
        if (!url || !url[0]) continue;
        strncpy(out->peers[out->count].url, url, WARP_PEER_URL_MAX - 1);
        out->count++;
    }

    json_free(root);
    return WARP_OK;
}

int p2p_load_peers(warp_peer_list_t *out, const char *list_url) {
    memset(out, 0, sizeof(*out));

    /* Fall back to the compiled-in tracker's public peer list (/dashboard is admin-only) */
    if (!list_url || !list_url[0])
        list_url = WARP_TRACKER_URL "/peers";

    if (!list_url[0]) return WARP_ERR_NET;

    /* Try cache first if fresh */
    if (!peers_cache_stale()) {
        size_t len;
        char *data = read_file(WARP_PEERS_CACHE, &len);
        if (data) {
            int rc = parse_peer_json(data, out);
            free(data);
            if (rc == WARP_OK && out->count > 0) return WARP_OK;
        }
    }

    /* Fetch from URL */
    warp_info("Fetching peer list...");
    char *data = warp_download_str(list_url);
    if (!data) {
        /* Stale cache is better than nothing */
        size_t len;
        char *cached = read_file(WARP_PEERS_CACHE, &len);
        if (cached) {
            warp_warn("Using stale peer list (tracker unreachable)");
            int rc = parse_peer_json(cached, out);
            free(cached);
            return rc;
        }
        return WARP_ERR_NET;
    }

    /* Cache the fresh response */
    FILE *f = fopen(WARP_PEERS_CACHE, "w");
    if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }

    int rc = parse_peer_json(data, out);
    free(data);
    return rc;
}

/* ══════════════════════════════════════════════════════════════
 *  P2P download — try peers sequentially, fallback to direct URL
 * ══════════════════════════════════════════════════════════════ */

/* libcurl callback: write to file + stream SHA256 */
typedef struct {
    FILE       *fp;
    void       *sha_ctx;   /* EVP_MD_CTX* — use void* to avoid OpenSSL header here */
    int         show_progress;
} p2p_dl_state_t;

int p2p_download(const char *pkg_name, const char *sha256_expected,
                 const char *dest_path, warp_peer_list_t *peers) {
    if (!peers || peers->count == 0) return WARP_ERR_NET;

    int tries = peers->count < WARP_P2P_MAX_TRIES
                ? peers->count : WARP_P2P_MAX_TRIES;

    for (int i = 0; i < tries; i++) {
        /* Build peer URL: http://host:port/warp/v1/pkg/<sha256> */
        char url[WARP_PEER_URL_MAX + 128];
        snprintf(url, sizeof(url), "%s/warp/v1/pkg/%s",
                 peers->peers[i].url, sha256_expected);

        warp_info("Peer %d/%d: %s", i + 1, tries, peers->peers[i].url);

        /* Use a temp file so we don't clobber dest on partial download */
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s.p2p%d", dest_path, i);

        warp_dl_opts_t dl = { .show_progress = 1 };

        CURL *curl = curl_easy_init();
        if (!curl) continue;

        FILE *fp = fopen(tmp, "wb");
        if (!fp) { curl_easy_cleanup(curl); continue; }

        /* Reuse existing download infrastructure via warp_download */
        fclose(fp);
        curl_easy_cleanup(curl);

        /* Set short timeout so a dead peer doesn't stall everything */
        /* warp_download uses libcurl defaults; we call it directly and
           rely on CURLOPT_TIMEOUT via env or accept the default.
           For simplicity, delegate to warp_download then verify. */
        int rc = warp_download(url, tmp, &dl);
        if (rc != WARP_OK) {
            warp_warn("  Peer %d: connection failed", i + 1);
            remove(tmp);
            continue;
        }

        /* Verify SHA256 against ground truth from GitHub index */
        if (strcmp(dl.computed_sha256, sha256_expected) != 0) {
            warp_warn("  Peer %d: " WARP_RED "SHA256 MISMATCH — peer may be poisoned" WARP_RESET,
                      i + 1);
            warp_warn("  Expected: %.16s...", sha256_expected);
            warp_warn("  Got:      %.16s...", dl.computed_sha256);
            remove(tmp);
            continue;  /* try next peer */
        }

        /* Success */
        rename(tmp, dest_path);
        warp_ok("Verified — downloaded from peer %d/%d", i + 1, tries);
        return WARP_OK;
    }

    return WARP_ERR_NET;   /* all peers failed */
}

/* ══════════════════════════════════════════════════════════════
 *  Announce self to tracker
 * ══════════════════════════════════════════════════════════════ */

/* Build packages JSON array from installed store for announce body.
   Caller provides a pre-allocated buffer; returns number of packages added. */
static int build_announce_packages(char *buf, size_t bufsz) {
    /* Walk store to collect all installed packages with their sha256 */
    DIR *d = opendir(WARP_STORE_DIR "/store");
    if (!d) return 0;

    size_t pos = 0;
    int count = 0;
    pos += snprintf(buf + pos, bufsz - pos, "[");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char sha_file[512], manifest_file[512];
        snprintf(sha_file,      sizeof(sha_file),
                 WARP_STORE_DIR "/store/%s/sha256", ent->d_name);
        snprintf(manifest_file, sizeof(manifest_file),
                 WARP_STORE_DIR "/store/%s/manifest.json", ent->d_name);

        size_t len;
        char *sha = read_file(sha_file, &len);
        if (!sha) continue;
        char *nl = strchr(sha, '\n'); if (nl) *nl = '\0';

        char pkg_name[WARP_MAX_NAME] = "";
        char *ms = read_file(manifest_file, &len);
        if (ms) {
            json_t *jm = json_parse(ms); free(ms);
            if (jm) {
                strncpy(pkg_name, json_str(jm, "name", ""), WARP_MAX_NAME - 1);
                json_free(jm);
            }
        }

        if (pkg_name[0] && pos + 200 < bufsz) {
            pos += snprintf(buf + pos, bufsz - pos,
                            "%s{\"name\":\"%s\",\"sha256\":\"%s\"}",
                            count ? "," : "", pkg_name, sha);
            count++;
        }
        free(sha);
    }
    closedir(d);

    /* Also include volunteer cached packages */
    DIR *vd = opendir(g_volunteer_dir);
    if (vd) {
        while ((ent = readdir(vd)) != NULL) {
            size_t nl = strlen(ent->d_name);
            if (nl < 6 || strcmp(ent->d_name + nl - 5, ".warp") != 0) continue;

            char sha[WARP_SHA256_HEX];
            strncpy(sha, ent->d_name, nl - 5); sha[nl - 5] = '\0';

            char meta_file[600];
            snprintf(meta_file, sizeof(meta_file),
                     "%s/%s.meta", g_volunteer_dir, sha);
            char pkg_name[WARP_MAX_NAME] = "";
            size_t mlen;
            char *ms = read_file(meta_file, &mlen);
            if (ms) {
                json_t *jm = json_parse(ms); free(ms);
                if (jm) {
                    strncpy(pkg_name, json_str(jm, "name", ""), WARP_MAX_NAME - 1);
                    json_free(jm);
                }
            }
            if (pkg_name[0] && pos + 200 < bufsz) {
                pos += snprintf(buf + pos, bufsz - pos,
                                "%s{\"name\":\"%s\",\"sha256\":\"%s\",\"volunteer\":true}",
                                count ? "," : "", pkg_name, sha);
                count++;
            }
        }
        closedir(vd);
    }

    if (pos + 2 < bufsz) snprintf(buf + pos, bufsz - pos, "]");
    return count;
}

int p2p_announce(const char *announce_url, const char *pkg_name,
                 const char *sha256, int port, int is_volunteer) {
    if (!announce_url || !announce_url[0]) return WARP_ERR_NET;

    /* Build packages array */
    char packages[4096];
    if (pkg_name && sha256 && sha256[0]) {
        /* Single package announce (after install) */
        snprintf(packages, sizeof(packages),
                 "[{\"name\":\"%s\",\"sha256\":\"%s\"}]", pkg_name, sha256);
    } else {
        /* Full store announce (warp seed / volunteer) */
        build_announce_packages(packages, sizeof(packages));
    }

    char body[4608];
    snprintf(body, sizeof(body),
             "{\"port\":%d,\"packages\":%s,\"is_volunteer\":%s}",
             port, packages, is_volunteer ? "true" : "false");

    CURL *curl = curl_easy_init();
    if (!curl) return WARP_ERR_NET;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    char url[WARP_MAX_URL];
    snprintf(url, sizeof(url), "%s/announce", announce_url);

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "warp/" WARP_VERSION);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        10L);
    if (warp_ca_bundle()) curl_easy_setopt(curl, CURLOPT_CAINFO, warp_ca_bundle());
    /* Discard response body */
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    return res == CURLE_OK ? WARP_OK : WARP_ERR_NET;
}

/* ══════════════════════════════════════════════════════════════
 *  Seed server — minimal HTTP server over BSD sockets
 *
 *  GET /warp/v1/list           → JSON array of {name,sha256,size}
 *  GET /warp/v1/pkg/<sha256>   → raw .warp file bytes
 * ══════════════════════════════════════════════════════════════ */

/* Find package.warp in store by full sha256 — store only.
   Returns malloc'd path (caller frees) or NULL. */
static char *store_find_warp_by_sha256(const char *sha256) {
    DIR *d = opendir(WARP_STORE_DIR "/store");
    if (!d) return NULL;

    struct dirent *ent;
    char *result = NULL;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char sha_file[512];
        snprintf(sha_file, sizeof(sha_file),
                 WARP_STORE_DIR "/store/%s/sha256", ent->d_name);

        size_t len;
        char *stored = read_file(sha_file, &len);
        if (!stored) continue;

        /* strip trailing newline */
        char *nl = strchr(stored, '\n');
        if (nl) *nl = '\0';

        if (strcmp(stored, sha256) == 0) {
            char path[512];
            snprintf(path, sizeof(path),
                     WARP_STORE_DIR "/store/%s/package.warp", ent->d_name);
            if (path_exists(path)) {
                result = strdup(path);
                free(stored);
                break;
            }
        }
        free(stored);
    }

    closedir(d);
    return result;
}

/* Find .warp by sha256 in store OR volunteer cache */
static char *find_warp_by_sha256(const char *sha256) {
    char *p = store_find_warp_by_sha256(sha256);
    if (p) return p;
    return volunteer_find_warp_by_sha256(sha256);
}

/* Build JSON list of all seedable packages (store + volunteer cache) */
static char *build_pkg_list_json(void) {
    DIR *d = opendir(WARP_STORE_DIR "/store");
    if (!d) return strdup("{\"packages\":[]}");

    /* Estimate buffer: ~200 bytes per package */
    size_t cap = 2048, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { closedir(d); return NULL; }

    pos += snprintf(buf + pos, cap - pos, "{\"packages\":[");

    int first = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char sha_file[512], warp_file[512], manifest_file[512];
        snprintf(sha_file,      sizeof(sha_file),
                 WARP_STORE_DIR "/store/%s/sha256",       ent->d_name);
        snprintf(warp_file,     sizeof(warp_file),
                 WARP_STORE_DIR "/store/%s/package.warp", ent->d_name);
        snprintf(manifest_file, sizeof(manifest_file),
                 WARP_STORE_DIR "/store/%s/manifest.json", ent->d_name);

        if (!path_exists(warp_file)) continue;

        size_t len;
        char *sha = read_file(sha_file, &len);
        if (!sha) continue;
        char *nl = strchr(sha, '\n'); if (nl) *nl = '\0';

        /* Get package name from manifest */
        char pkg_name[WARP_MAX_NAME] = "unknown";
        char *ms = read_file(manifest_file, &len);
        if (ms) {
            json_t *jm = json_parse(ms);
            free(ms);
            if (jm) {
                strncpy(pkg_name, json_str(jm, "name", "unknown"), WARP_MAX_NAME - 1);
                json_free(jm);
            }
        }

        long sz = file_size(warp_file);

        /* Grow buffer if needed */
        if (pos + 256 >= cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { free(sha); closedir(d); return NULL; }
        }

        pos += snprintf(buf + pos, cap - pos,
                        "%s{\"name\":\"%s\",\"sha256\":\"%s\",\"size\":%ld}",
                        first ? "" : ",", pkg_name, sha, sz);
        first = 0;
        free(sha);
    }
    closedir(d);

    /* Also include volunteer cache packages */
    DIR *vd = opendir(g_volunteer_dir);
    if (vd) {
        while ((ent = readdir(vd)) != NULL) {
            size_t nl = strlen(ent->d_name);
            if (nl < 6 || strcmp(ent->d_name + nl - 5, ".warp") != 0) continue;

            char warp_file[600], meta_file[600];
            snprintf(warp_file, sizeof(warp_file),
                     "%s/%s", g_volunteer_dir, ent->d_name);
            /* sha256 is the filename minus .warp */
            char sha[WARP_SHA256_HEX];
            strncpy(sha, ent->d_name, nl - 5);
            sha[nl - 5] = '\0';

            /* Read companion .meta for name */
            char pkg_name[WARP_MAX_NAME] = "unknown";
            snprintf(meta_file, sizeof(meta_file),
                     "%s/%s.meta", g_volunteer_dir, sha);
            size_t mlen;
            char *ms = read_file(meta_file, &mlen);
            if (ms) {
                json_t *jm = json_parse(ms); free(ms);
                if (jm) {
                    strncpy(pkg_name, json_str(jm, "name", "unknown"), WARP_MAX_NAME - 1);
                    json_free(jm);
                }
            }

            long sz = file_size(warp_file);

            if (pos + 256 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
                if (!buf) { closedir(vd); return NULL; }
            }
            pos += snprintf(buf + pos, cap - pos,
                            "%s{\"name\":\"%s\",\"sha256\":\"%s\",\"size\":%ld,\"volunteer\":true}",
                            first ? "" : ",", pkg_name, sha, sz);
            first = 0;
        }
        closedir(vd);
    }

    if (pos + 4 >= cap) { cap += 8; buf = realloc(buf, cap); }
    pos += snprintf(buf + pos, cap - pos, "]}");
    return buf;
}

/* Send HTTP response header */
static void http_header(int fd, int code, const char *ctype, long content_len) {
    char hdr[256];
    const char *status = (code == 200) ? "OK" : "Not Found";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.0 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %ld\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, status, ctype, content_len);
    write(fd, hdr, n);
}

static void seed_serve_list(int fd) {
    char *json = build_pkg_list_json();
    if (!json) {
        http_header(fd, 500, "text/plain", 0);
        return;
    }
    long len = (long)strlen(json);
    http_header(fd, 200, "application/json", len);
    write(fd, json, len);
    free(json);
}

static void seed_serve_pkg(int fd, const char *sha256) {
    /* Basic validation: sha256 should be 64 hex chars */
    if (strlen(sha256) != 64) {
        http_header(fd, 404, "text/plain", 9);
        write(fd, "Not Found", 9);
        return;
    }
    for (int i = 0; i < 64; i++) {
        char c = sha256[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            http_header(fd, 404, "text/plain", 9);
            write(fd, "Not Found", 9);
            return;
        }
    }

    /* Check if serving is enabled */
    if (g_have_seed_cfg && !g_seed_cfg.serve) {
        const char *r = "HTTP/1.0 503 Seeding Disabled\r\n"
                        "Content-Length: 16\r\n\r\nSeeding disabled";
        write(fd, r, strlen(r));
        return;
    }

    /* Check monthly upload limit */
    if (g_have_seed_cfg &&
        g_seed_cfg.monthly_limit_bytes > 0 &&
        g_seed_cfg.monthly_used_bytes >= g_seed_cfg.monthly_limit_bytes) {
        char lim[64]; warp_fmt_size(g_seed_cfg.monthly_limit_bytes, lim, sizeof(lim));
        warp_warn("Monthly upload limit (%s) reached — not serving", lim);
        const char *r = "HTTP/1.0 503 Monthly Limit Reached\r\n"
                        "Content-Length: 0\r\n\r\n";
        write(fd, r, strlen(r));
        return;
    }

    char *path = find_warp_by_sha256(sha256);
    if (!path) {
        http_header(fd, 404, "text/plain", 9);
        write(fd, "Not Found", 9);
        return;
    }

    long sz = file_size(path);
    if (sz < 0) {
        free(path);
        http_header(fd, 404, "text/plain", 9);
        write(fd, "Not Found", 9);
        return;
    }

    http_header(fd, 200, "application/octet-stream", sz);

    FILE *f = fopen(path, "rb");
    free(path);
    if (!f) return;

    char buf[65536];
    size_t n;
    size_t bytes_sent = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        write(fd, buf, n);
        bytes_sent += n;
    }
    fclose(f);

    /* Track uploaded bytes */
    if (g_have_seed_cfg && bytes_sent > 0) {
        g_seed_cfg.monthly_used_bytes += bytes_sent;
        g_seed_cfg.uploaded_total     += bytes_sent;
        g_seed_cfg.served_total++;
        time_t now = time(NULL);
        /* Write to disk at most once per minute to reduce I/O */
        if (now - g_last_cfg_save >= 60) {
            seed_config_save(&g_seed_cfg);
            g_last_cfg_save = now;
        }
    }
}

static void seed_handle_request(int fd) {
    /* Read request line (we only need the first line) */
    char req[2048] = {0};
    int total = 0;
    while (total < (int)sizeof(req) - 1) {
        int n = (int)read(fd, req + total, sizeof(req) - 1 - total);
        if (n <= 0) break;
        total += n;
        /* Stop after we have the first line */
        if (memchr(req, '\n', total)) break;
    }

    char method[16] = {0}, path[512] = {0};
    sscanf(req, "%15s %511s", method, path);

    if (strcmp(method, "GET") != 0) {
        const char *r = "HTTP/1.0 405 Method Not Allowed\r\n\r\n";
        write(fd, r, strlen(r));
        return;
    }

    if (strcmp(path, "/warp/v1/list") == 0) {
        seed_serve_list(fd);
    } else if (strncmp(path, "/warp/v1/pkg/", 13) == 0) {
        seed_serve_pkg(fd, path + 13);
    } else {
        http_header(fd, 404, "text/plain", 9);
        write(fd, "Not Found", 9);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Volunteer: download low-seeder packages into volunteer cache
 * ══════════════════════════════════════════════════════════════ */

/* Seeder counts per package name from the tracker's public /popularity:
   { "nano": 3, "ripgrep": 1, ... } */
static json_t *fetch_popularity(void) {
    char url[WARP_MAX_URL];
    snprintf(url, sizeof(url), "%s/popularity", WARP_TRACKER_URL);
    char *body = warp_download_str(url);
    if (!body) return NULL;
    json_t *j = json_parse(body);
    free(body);
    return j;
}

typedef struct { int idx; int seeders; } vol_cand_t;

/* Least seeded first, index order breaks ties. */
static int vol_cand_cmp(const void *a, const void *b) {
    const vol_cand_t *x = a, *y = b;
    if (x->seeders != y->seeders) return x->seeders - y->seeders;
    return x->idx - y->idx;
}

static size_t disk_free_bytes(const char *path) {
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) return (size_t)-1;
    return (size_t)sv.f_bavail * (size_t)sv.f_frsize;
}

/* Fill the volunteer cache within the user's limits: size, package count
 * and the disk space that must stay free. Returns the number of new packages. */
static int volunteer_fill(warp_seed_config_t *cfg) {
    mkdirs(g_volunteer_dir, 0755);

    warp_index_t idx;
    if (index_load(&idx, 0) != WARP_OK) {
        warp_err("Cannot load package index");
        return WARP_ERR_IO;
    }

    warp_info("Fetching package popularity from tracker...");
    json_t *pop = fetch_popularity();
    if (!pop)
        warp_warn("Tracker unreachable — ignoring popularity, using index order");

    size_t used    = volunteer_used_bytes();
    size_t have    = volunteer_count();
    size_t reserve = cfg->reserve_bytes ? cfg->reserve_bytes : WARP_RESERVE_DEFAULT;
    size_t limit   = cfg->unlimited ? (size_t)-1 : cfg->quota_bytes;

    char sz_used[32], sz_quota[32];
    warp_fmt_size(used, sz_used, sizeof(sz_used));
    if (cfg->unlimited) snprintf(sz_quota, sizeof(sz_quota), "unlimited");
    else                warp_fmt_size(cfg->quota_bytes, sz_quota, sizeof(sz_quota));
    if (cfg->max_packages)
        warp_info("Volunteer cache: %s / %s, %zu / %zu packages",
                  sz_used, sz_quota, have, cfg->max_packages);
    else
        warp_info("Volunteer cache: %s / %s, %zu packages (no count limit)",
                  sz_used, sz_quota, have);

    /* Candidates: not installed, not cached yet; rarely seeded first.
       "Everything" mode takes well-seeded packages too. */
    vol_cand_t *cand = calloc((size_t)(idx.count > 0 ? idx.count : 1), sizeof(*cand));
    int ncand = 0;
    for (int i = 0; cand && i < idx.count; i++) {
        warp_pkg_entry_t *e = &idx.entries[i];
        if (!e->sha256[0] || !e->url[0] || e->versioned) continue;
        if (store_is_installed(e->name, NULL)) continue;
        if (volunteer_has_pkg(e->sha256))      continue;
        int seeders = pop ? (int)json_num(pop, e->name, 0) : 0;
        if (!cfg->unlimited && seeders >= 5) continue;   /* well seeded already */
        cand[ncand].idx = i;
        cand[ncand].seeders = seeders;
        ncand++;
    }
    if (ncand > 1) qsort(cand, (size_t)ncand, sizeof(*cand), vol_cand_cmp);

    int downloaded = 0;
    for (int c = 0; c < ncand; c++) {
        warp_pkg_entry_t *e = &idx.entries[cand[c].idx];

        if (cfg->max_packages && have >= cfg->max_packages) {
            warp_info("Package limit reached (%zu)", cfg->max_packages);
            break;
        }

        size_t room  = (limit == (size_t)-1) ? (size_t)-1 : (limit > used ? limit - used : 0);
        size_t dfree = disk_free_bytes(g_volunteer_dir);
        size_t droom = (dfree == (size_t)-1) ? (size_t)-1 : (dfree > reserve ? dfree - reserve : 0);
        if (droom < room) room = droom;
        if (room == 0) {
            warp_warn("No room left (quota or the disk space kept free) — stopping");
            break;
        }
        if (e->size > 0 && (size_t)e->size > room) continue;

        char pkg_sz[32];
        warp_fmt_size((size_t)e->size, pkg_sz, sizeof(pkg_sz));
        warp_info("Volunteering: " WARP_CYAN "%s %s" WARP_RESET " (%s, seeders: %d)",
                  e->name, e->version, pkg_sz, cand[c].seeders);

        char dest[600], tmp[620];
        snprintf(dest, sizeof(dest), "%s/%s.warp", g_volunteer_dir, e->sha256);
        snprintf(tmp,  sizeof(tmp),  "%s.tmp",     dest);

        warp_dl_opts_t dl = { .show_progress = 1 };
        if (warp_download_pkg(e->url, index_repo_of(&idx, e), tmp, &dl) != WARP_OK) {
            warp_warn("  Download failed: %s", e->name);
            remove(tmp);
            continue;
        }
        if (strcmp(dl.computed_sha256, e->sha256) != 0) {
            warp_warn("  SHA256 mismatch for %s — discarding", e->name);
            remove(tmp);
            continue;
        }

        rename(tmp, dest);
        volunteer_write_meta(e->sha256, e->name, e->version);

        long sz = file_size(dest);
        if (sz > 0) used += (size_t)sz;
        have++;
        downloaded++;
        warp_ok("Cached: %s", e->name);
    }

    free(cand);
    if (pop) json_free(pop);
    index_free(&idx);

    warp_fmt_size(volunteer_used_bytes(), sz_used, sizeof(sz_used));
    if (downloaded > 0)
        warp_ok("Volunteered %d package(s) — cache now %s / %s", downloaded, sz_used, sz_quota);
    else
        warp_info("No new packages to volunteer for");
    return downloaded;
}

/* ══════════════════════════════════════════════════════════════
 *  The node: one process that seeds, and in volunteer mode also fills the
 *  cache. SIGHUP re-reads the config; SIGTERM/SIGINT stop it.
 * ══════════════════════════════════════════════════════════════ */

static volatile sig_atomic_t node_running = 1;
static volatile sig_atomic_t node_reload  = 0;
static void node_on_term(int s) { (void)s; node_running = 0; }
static void node_on_hup(int s)  { (void)s; node_reload  = 1; }

/* Signal the running node (sig 0 = just check it is alive). The pid file can
 * be stale and its pid reused, so only a process that is really `warp` is
 * touched: a stray SIGHUP would kill an unrelated program. */
int p2p_node_signal(int sig) {
    size_t len;
    char *s = read_file(WARP_NODE_PID, &len);
    if (!s) return WARP_ERR_NOENT;
    long pid = atol(s);
    free(s);
    if (pid <= 1) return WARP_ERR_NOENT;

    char comm[64] = "";
#if defined(__APPLE__)
    if (proc_name((int)pid, comm, sizeof(comm)) <= 0) return WARP_ERR_NOENT;     /* macOS has no /proc */
#else
    char comm_path[64];
    snprintf(comm_path, sizeof(comm_path), "/proc/%ld/comm", pid);
    FILE *f = fopen(comm_path, "r");
    if (!f) return WARP_ERR_NOENT;
    if (fgets(comm, sizeof(comm), f)) comm[strcspn(comm, "\n")] = '\0';
    fclose(f);
#endif
    if (strcmp(comm, "warp") != 0) return WARP_ERR_NOENT;

    if (sig == 0) return kill((pid_t)pid, 0) == 0 ? WARP_OK : WARP_ERR_NOENT;
    return kill((pid_t)pid, sig) == 0 ? WARP_OK : WARP_ERR_IO;
}

static void node_announce(int port) {
    if (!g_seed_cfg.serve) return;
    if (p2p_announce(WARP_TRACKER_URL, NULL, NULL, port, g_seed_cfg.volunteer) != WARP_OK)
        warp_warn("Tracker unreachable — seeding locally only");
}

/* Settings come from the file, counters stay the ones in memory. */
static void node_reload_config(void) {
    warp_seed_config_t fresh;
    if (seed_config_load(&fresh) != WARP_OK) {
        warp_warn("Reload: cannot read %s, keeping current settings", WARP_SEED_CONF);
        return;
    }
    fresh.monthly_used_bytes = g_seed_cfg.monthly_used_bytes;
    fresh.uploaded_total     = g_seed_cfg.uploaded_total;
    fresh.served_total       = g_seed_cfg.served_total;
    if (g_seed_cfg.node_id[0]) memcpy(fresh.node_id, g_seed_cfg.node_id, sizeof(fresh.node_id));
    g_seed_cfg = fresh;
    warp_info("Config reloaded (volunteer: %s)", g_seed_cfg.volunteer ? "on" : "off");
}

static int node_serve(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        warp_err("socket: %s", strerror(errno));
        return WARP_ERR_NET;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        warp_err("bind: %s (port %d)", strerror(errno), port);
        close(srv);
        return WARP_ERR_NET;
    }
    if (listen(srv, 16) < 0) {
        warp_err("listen: %s", strerror(errno));
        close(srv);
        return WARP_ERR_NET;
    }

    warp_ok("Seeding on port %d  (Ctrl+C to stop)", port);
    warp_info("Add yourself to peers list: http://<your-ip>:%d", port);
    warp_info("Peers fetch: http://<your-ip>:%d/warp/v1/list", port);
    printf("\n");

    time_t last_tick = time(NULL), last_fill = last_tick;

    while (node_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };

        int sel = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0 && errno != EINTR) {
            warp_err("select: %s", strerror(errno));
            break;
        }

        if (sel > 0) {
            struct sockaddr_in cli_addr;
            socklen_t cli_len = sizeof(cli_addr);
            int cli = accept(srv, (struct sockaddr *)&cli_addr, &cli_len);
            if (cli >= 0) {
                seed_handle_request(cli);
                close(cli);
            }
        }

        time_t now = time(NULL);

        if (node_reload) {
            node_reload = 0;
            node_reload_config();
            if (g_seed_cfg.volunteer) { volunteer_fill(&g_seed_cfg); last_fill = now; }
            node_announce(port);
            stats_report(&g_seed_cfg);
            last_tick = now;
        }

        /* Every 5 minutes: keep the tracker's record fresh, save counters, report */
        if (now - last_tick >= 300) {
            node_announce(port);
            stats_report(&g_seed_cfg);
            seed_config_save(&g_seed_cfg);
            g_last_cfg_save = now;
            last_tick = now;
        }

        /* Volunteer: look for newly rare packages every 6 hours */
        if (g_seed_cfg.volunteer && now - last_fill >= 6 * 3600) {
            volunteer_fill(&g_seed_cfg);
            last_fill = now;
        }
    }

    close(srv);
    printf("\n");
    warp_info("Seeding stopped");
    return WARP_OK;
}

int p2p_node_run(warp_seed_config_t *cfg, int port) {
    if (p2p_node_signal(0) == WARP_OK) {
        warp_warn("A WARP node is already running — it keeps serving; use SIGHUP to reload.");
        return WARP_ERR_EXIST;
    }

    stats_ensure_node_id(cfg);
    g_seed_cfg      = *cfg;
    g_have_seed_cfg = 1;
    g_last_cfg_save = time(NULL);

    FILE *pf = fopen(WARP_NODE_PID, "w");
    if (pf) { fprintf(pf, "%ld\n", (long)getpid()); fclose(pf); }

    signal(SIGINT,  node_on_term);
    signal(SIGTERM, node_on_term);
    signal(SIGHUP,  node_on_hup);

    if (g_seed_cfg.volunteer) volunteer_fill(&g_seed_cfg);

    int rc = WARP_OK;
    if (g_seed_cfg.serve) {
        warp_info("Announcing to tracker...");
        node_announce(port);
        stats_report(&g_seed_cfg);
        rc = node_serve(port);
    }

    seed_config_save(&g_seed_cfg);
    remove(WARP_NODE_PID);
    return rc;
}
