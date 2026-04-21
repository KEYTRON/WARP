#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include "warp.h"

/* ── write callback: write to file + update sha256 ───────────── */
typedef struct {
    FILE        *fp;
    EVP_MD_CTX  *sha_ctx;
    curl_off_t   total;
    curl_off_t   received;
    int          show_progress;
} dl_state_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    dl_state_t *st = (dl_state_t *)userdata;
    size_t bytes = size * nmemb;

    if (fwrite(ptr, 1, bytes, st->fp) != bytes) return 0;
    EVP_DigestUpdate(st->sha_ctx, ptr, bytes);
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

    EVP_MD_CTX *sha_ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(sha_ctx, EVP_sha256(), NULL);

    dl_state_t st = {
        .fp           = fp,
        .sha_ctx      = sha_ctx,
        .show_progress = opts ? opts->show_progress : 1,
    };

    curl_easy_setopt(curl, CURLOPT_URL,              url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,    write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,        &st);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA,     &st);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,        "warp/" WARP_VERSION);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,      1L);
    /* Use system CA certs */
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");

    CURLcode res = curl_easy_perform(curl);

    if (st.show_progress) fprintf(stderr, "\n");

    int rc = WARP_OK;
    if (res != CURLE_OK) {
        warp_err("Download failed: %s", curl_easy_strerror(res));
        rc = WARP_ERR_NET;
    }

    /* Finalise SHA256 */
    if (opts) {
        uint8_t digest[EVP_MAX_MD_SIZE];
        unsigned int digest_len = 0;
        EVP_DigestFinal_ex(sha_ctx, digest, &digest_len);
        for (unsigned int i = 0; i < digest_len; i++)
            snprintf(opts->computed_sha256 + i*2, 3, "%02x", digest[i]);
        opts->computed_sha256[digest_len * 2] = '\0';
    }

    EVP_MD_CTX_free(sha_ctx);
    fclose(fp);
    curl_easy_cleanup(curl);

    if (rc != WARP_OK) remove(dest_path);
    return rc;
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
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}
