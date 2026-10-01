#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <curl/curl.h>
#include "warp.h"

/* ── CA bundle ─────────────────────────────────────────────────
 * Where the root certificates live differs per system: Debian, Alpine, Arch and
 * Void use /etc/ssl/certs/ca-certificates.crt, Fedora and Alma /etc/pki/..., macOS
 * and Alpine/BSD /etc/ssl/cert.pem, Termux $PREFIX/etc/tls/cert.pem. A bundle that
 * is not there makes every HTTPS request fail, so: an explicit override first, then
 * the known places, and when none exists libcurl's own default (macOS: the system
 * trust store) is used. NULL means "do not set CURLOPT_CAINFO". */
const char *warp_ca_bundle(void) {
    static const char *cached;
    static int done;
    if (done) return cached;
    done = 1;
    const char *env[] = { getenv("WARP_CA_BUNDLE"), getenv("SSL_CERT_FILE") };
    for (int i = 0; i < 2; i++)
        if (env[i] && *env[i] && access(env[i], R_OK) == 0) return cached = env[i];
    static const char *known[] = {
        "/etc/ssl/certs/ca-certificates.crt",                          /* Debian, Ubuntu, Alpine, Arch, Void, K1OS */
        "/etc/pki/tls/certs/ca-bundle.crt",                            /* Fedora, RHEL, AlmaLinux */
        "/etc/ssl/ca-bundle.pem",                                      /* openSUSE */
        "/etc/ssl/cert.pem",                                           /* macOS, Alpine, BSD */
        "/data/data/com.termux/files/usr/etc/tls/cert.pem",            /* Termux */
        NULL
    };
    for (int i = 0; known[i]; i++)
        if (access(known[i], R_OK) == 0) return cached = known[i];
    return cached = NULL;
}

/* ── write callback: write to file + update sha256 ───────────── */
typedef struct {
    FILE        *fp;
    warp_sha256_t sha_ctx;
    curl_off_t   total;
    curl_off_t   received;
    int          show_progress;
} dl_state_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    dl_state_t *st = (dl_state_t *)userdata;
    size_t bytes = size * nmemb;

    if (fwrite(ptr, 1, bytes, st->fp) != bytes) return 0;
    warp_sha256_update(&st->sha_ctx, ptr, bytes);
    return bytes;
}

/* ── progress bar ────────────────────────────────────────────── */
static int progress_cb(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                        curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    dl_state_t *st = (dl_state_t *)userdata;
    if (!st->show_progress || dltotal <= 0) return 0;

    int pct = (int)((dlnow * 100) / dltotal);
    int bar_w = 30;
    int filled = (pct * bar_w) / 100;

    fprintf(stderr, "\r  " WARP_CYAN "[" WARP_RESET);
    for (int i = 0; i < bar_w; i++)
        fprintf(stderr, i < filled ? WARP_GREEN "█" WARP_RESET : "░");
    fprintf(stderr, WARP_CYAN "]" WARP_RESET " %3d%%  %.1f / %.1f KB",
            pct, (double)dlnow/1024.0, (double)dltotal/1024.0);
    fflush(stderr);
    return 0;
}

/* ── main download function ───────────────────────────────────── */
int warp_download(const char *url, const char *dest_path, warp_dl_opts_t *opts) {
    CURL *curl = curl_easy_init();
    if (!curl) return WARP_ERR_NET;

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) { curl_easy_cleanup(curl); return WARP_ERR_IO; }

    dl_state_t st = {
        .fp           = fp,
        .show_progress = opts ? opts->show_progress : 1,
    };
    warp_sha256_init(&st.sha_ctx);

    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &st);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        "warp/" WARP_VERSION);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,      1L);
    /* A dead host must cost seconds, not minutes: no total time limit (packages can be big),
     * but a connection that never opens or stalls is dropped. */
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,   (long)(opts && opts->connect_timeout > 0 ? opts->connect_timeout : 15));
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT,  1000L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME,   (long)(opts && opts->stall_timeout > 0 ? opts->stall_timeout : 30));
    if (warp_ca_bundle()) curl_easy_setopt(curl, CURLOPT_CAINFO, warp_ca_bundle());

    CURLcode res = curl_easy_perform(curl);

    if (st.show_progress) fprintf(stderr, "\n");

    int rc = WARP_OK;
    if (res != CURLE_OK) {
        warp_err("Download failed: %s", curl_easy_strerror(res));
        rc = WARP_ERR_NET;
    }

    /* Finalise SHA256 */
    if (opts) {
        uint8_t digest[32];
        warp_sha256_final(&st.sha_ctx, digest);
        for (unsigned int i = 0; i < 32; i++)
            snprintf(opts->computed_sha256 + i*2, 3, "%02x", digest[i]);
        opts->computed_sha256[64] = '\0';
    }

    fclose(fp);
    curl_easy_cleanup(curl);

    if (rc != WARP_OK) remove(dest_path);
    return rc;
}

int warp_download_pkg(const char *url, const warp_repo_t *repo, const char *dest_path, warp_dl_opts_t *opts) {
    const char *filename = strrchr(url, '/');
    if (filename) filename++; else filename = url;

    if (url[0] && strncmp(url, "http", 4) == 0) {
        warp_info("Downloading %s", url);
        if (warp_download(url, dest_path, opts) == WARP_OK) return WARP_OK;
    }
    if (!repo) return WARP_ERR_NET;
    for (int i = 0; i < repo->mirror_count; i++) {
        char try_url[WARP_MAX_URL + 300];
        snprintf(try_url, sizeof(try_url), "%s/%s", repo->mirrors[i], filename);
        if (strcmp(try_url, url) == 0) continue;   /* already tried as published */
        warp_info("Trying mirror %d: %s", i + 1, try_url);
        if (warp_download(try_url, dest_path, opts) == WARP_OK) return WARP_OK;
    }
    return WARP_ERR_NET;
}

/* ── download to a string buffer (for index.json etc) ────────── */
typedef struct { char *buf; size_t len; size_t cap; } str_buf_t;

static size_t str_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    str_buf_t *sb = (str_buf_t *)ud;
    size_t bytes = size * nmemb;
    if (sb->len + bytes + 1 >= sb->cap) {
        sb->cap = (sb->len + bytes + 1) * 2;
        sb->buf = realloc(sb->buf, sb->cap);
        if (!sb->buf) return 0;
    }
    memcpy(sb->buf + sb->len, ptr, bytes);
    sb->len += bytes;
    sb->buf[sb->len] = '\0';
    return bytes;
}

char *warp_download_str(const char *url) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    str_buf_t sb = { .buf = malloc(4096), .len = 0, .cap = 4096 };
    if (sb.buf) sb.buf[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL,            url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  str_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &sb);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      "warp/" WARP_VERSION);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,    1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        30L);      /* indexes and peer lists are small */
    if (warp_ca_bundle()) curl_easy_setopt(curl, CURLOPT_CAINFO, warp_ca_bundle());

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        warp_warn("curl error: %s", curl_easy_strerror(res)); free(sb.buf);
        return NULL;
    }
    return sb.buf;
}
