#ifndef WARP_H
#define WARP_H

#include <stdint.h>
#include <stddef.h>

/* ── version & paths ─────────────────────────────────────────── */
#define WARP_VERSION     "0.1.0"
#define WARP_STORE_DIR   "/var/lib/warp"
#define WARP_INDEX_URL   "https://github.com/KEYTRON/WARP/releases/download/packages-v1/index.json"
#define WARP_ARCH        "x86_64"
#define WARP_MAX_VARIANTS 8

/* ── compile-time flags ──────────────────────────────────────── */
/* Define WARP_SKIP_SIG_VERIFY to skip Ed25519 signature checks (dev) */
/* #define WARP_SKIP_SIG_VERIFY */

/* ── return codes ────────────────────────────────────────────── */
#define WARP_OK           0
#define WARP_ERR_ARGS    -1
#define WARP_ERR_IO      -2
#define WARP_ERR_NET     -3
#define WARP_ERR_HASH    -4
#define WARP_ERR_SIG     -5
#define WARP_ERR_JSON    -6
#define WARP_ERR_NOENT   -7
#define WARP_ERR_EXIST   -8
#define WARP_ERR_INVAL   -9

/* ── manifest ────────────────────────────────────────────────── */
#define WARP_MAX_DEPS    32
#define WARP_MAX_BINS    64
#define WARP_MAX_NAME    128
#define WARP_MAX_URL     512
#define WARP_SHA256_HEX  65   /* 64 hex chars + NUL */

typedef struct {
    char   name[WARP_MAX_NAME];
    char   version[WARP_MAX_NAME];
    char   arch[32];
    char   description[256];
    char   developer[WARP_MAX_NAME];
    char   files_sha256[WARP_SHA256_HEX];
    char   signature[128];           /* base64 Ed25519 sig */
    char   deps[WARP_MAX_DEPS][WARP_MAX_NAME];
    int    deps_count;
    char   install_bins[WARP_MAX_BINS][WARP_MAX_NAME];
    int    bins_count;
} warp_manifest_t;

typedef struct {
    char   kind[32];
    char   url[WARP_MAX_URL];
    char   sha256[WARP_SHA256_HEX];
    char   signature[128];
    size_t size;
    int    priority;
} warp_release_variant_t;

/* ── index entry ─────────────────────────────────────────────── */
typedef struct {
    char   name[WARP_MAX_NAME];
    char   version[WARP_MAX_NAME];
    char   description[256];
    char   sha256[WARP_SHA256_HEX];
    char   url[WARP_MAX_URL];
    size_t size;
    char   signature[128];
    warp_release_variant_t variants[WARP_MAX_VARIANTS];
    int    variants_count;
} warp_pkg_entry_t;

typedef struct {
    warp_pkg_entry_t *entries;
    int               count;
    int               capacity;
    char              timestamp[32];
    char              signature[128];
} warp_index_t;

/* ── installed package info ──────────────────────────────────── */
typedef struct {
    char name[WARP_MAX_NAME];
    char version[WARP_MAX_NAME];
    char hash12[13];    /* first 12 chars of sha256 */
    char store_path[512];
} warp_installed_t;

/* ── crypto.h (inline) ───────────────────────────────────────── */
int  warp_sha256_file(const char *path, char out_hex[WARP_SHA256_HEX]);
int  warp_sha256_buf(const uint8_t *buf, size_t len, char out_hex[WARP_SHA256_HEX]);
int  warp_keygen(const char *privkey_path, const char *pubkey_path);
int  warp_verify_index_sig(const char *index_json, const char *sig_b64);
int  warp_sign_buf(const uint8_t *msg, size_t msg_len, const uint8_t *privkey, size_t privkey_len,
                   uint8_t sig[64]);
int  warp_sign_file(const char *path, const char *privkey_hex_path, char out_b64[128]);
int  warp_ed25519_verify(const uint8_t *msg, size_t msg_len,
                          const uint8_t sig[64],
                          const uint8_t pubkey[32]);
int  warp_base64_decode(const char *in, uint8_t *out, size_t *out_len);
int  warp_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_cap);
int  warp_hex_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len);

/* ── json.h (inline) ─────────────────────────────────────────── */
typedef enum { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING,
               JSON_ARRAY, JSON_OBJECT } json_type_t;

typedef struct json_t {
    json_type_t       type;
    char             *key;       /* set when inside object */
    union {
        int           b;         /* JSON_BOOL  */
        double        n;         /* JSON_NUMBER*/
        char         *s;         /* JSON_STRING*/
        struct {
            struct json_t **items;
            int             count;
        } arr;                   /* JSON_ARRAY + JSON_OBJECT */
    } v;
} json_t;

json_t *json_parse(const char *src);
json_t *json_get(json_t *obj, const char *key);
const char *json_str(json_t *node, const char *key, const char *def);
double      json_num(json_t *node, const char *key, double def);
void        json_free(json_t *node);

/* ── download.h (inline) ─────────────────────────────────────── */
typedef struct {
    int   show_progress;
    char  computed_sha256[WARP_SHA256_HEX];   /* filled after download */
} warp_dl_opts_t;

int warp_download(const char *url, const char *dest_path, warp_dl_opts_t *opts);
int warp_download_variant(const warp_release_variant_t *variant, const char *dest_path, warp_dl_opts_t *opts);

/* ── store.h (inline) ────────────────────────────────────────── */
int  store_init(void);
int  store_add(const warp_manifest_t *m, const char *warp_path, const char *sha256);
int  store_activate(const char *name, const char *hash12);
int  store_remove(const char *name);
int  store_rollback(const char *name);
int  store_list(warp_installed_t **out, int *count);
int  store_is_installed(const char *name, warp_installed_t *info);
void store_free_list(warp_installed_t *list, int count);

/* ── index.h (inline) ────────────────────────────────────────── */
int  index_load(warp_index_t *idx, int force_refresh);
int  index_search(const warp_index_t *idx, const char *query,
                  warp_pkg_entry_t **results, int *count);
int  index_find(const warp_index_t *idx, const char *name,
                warp_pkg_entry_t *out);
int  index_pick_variant(const warp_pkg_entry_t *entry, const char **reason,
                        warp_release_variant_t *out);
void index_free(warp_index_t *idx);

/* ── commands ────────────────────────────────────────────────── */
int cmd_install (int argc, char **argv);
int cmd_remove  (int argc, char **argv);
int cmd_list    (int argc, char **argv);
int cmd_search  (int argc, char **argv);
int cmd_rollback(int argc, char **argv);
int cmd_info    (int argc, char **argv);
int cmd_update  (int argc, char **argv);
int cmd_keygen  (int argc, char **argv);
int cmd_pack    (int argc, char **argv);
int cmd_sign    (int argc, char **argv);

/* ── utils ───────────────────────────────────────────────────── */
#define WARP_RED    "\033[0;31m"
#define WARP_GREEN  "\033[0;32m"
#define WARP_YELLOW "\033[1;33m"
#define WARP_CYAN   "\033[0;36m"
#define WARP_BOLD   "\033[1m"
#define WARP_RESET  "\033[0m"

#define warp_ok(fmt, ...)  fprintf(stdout, WARP_GREEN  "  ✓ " WARP_RESET fmt "\n", ##__VA_ARGS__)
#define warp_err(fmt, ...) fprintf(stderr, WARP_RED    "  ✗ " WARP_RESET fmt "\n", ##__VA_ARGS__)
#define warp_info(fmt,...) fprintf(stdout, WARP_CYAN   "  → " WARP_RESET fmt "\n", ##__VA_ARGS__)
#define warp_warn(fmt,...) fprintf(stdout, WARP_YELLOW "  ! " WARP_RESET fmt "\n", ##__VA_ARGS__)

int  mkdirs(const char *path, mode_t mode);
int  path_exists(const char *path);
int  copy_file(const char *src, const char *dst);
long file_size(const char *path);
char *read_file(const char *path, size_t *len);

#endif /* WARP_H */
