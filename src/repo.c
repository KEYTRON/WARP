/* Package repositories.
 *
 * A repository is a name, one or more mirrors that serve index.json,
 * index.json.sig and the archives side by side, and the Ed25519 public key
 * that must have signed the index. The built-in "k1os" repository uses the
 * compiled-in mirrors and master key; more can be added with `warp repo add`
 * and are kept in /var/lib/warp/repos.json. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "warp.h"

extern const uint8_t WARP_MASTER_PUBKEY[32];

static void builtin_repo(warp_repo_t *r) {
    memset(r, 0, sizeof(*r));
    strncpy(r->name, WARP_DEFAULT_REPO, sizeof(r->name) - 1);
    for (int i = 0; i < WARP_INDEX_MIRRORS && i < WARP_MAX_MIRRORS; i++) {
        strncpy(r->mirrors[i], g_warp_mirrors[i], WARP_MAX_URL - 1);
        r->mirror_count++;
    }
    memcpy(r->pubkey, WARP_MASTER_PUBKEY, 32);
    r->enabled = 1;
    r->builtin = 1;
}

int repos_load(warp_repo_t *out, int *count) {
    *count = 0;
    size_t len;
    char *text = read_file(WARP_REPOS_CONF, &len);
    if (!text) {
        builtin_repo(&out[(*count)++]);
        return WARP_OK;
    }
    json_t *root = json_parse(text);
    free(text);
    json_t *list = root ? json_get(root, "repos") : NULL;
    if (!list || list->type != JSON_ARRAY) {
        json_free(root);
        warp_warn("%s is not a valid repos file — using the built-in repository", WARP_REPOS_CONF);
        builtin_repo(&out[(*count)++]);
        return WARP_OK;
    }
    for (int i = 0; i < list->v.arr.count && *count < WARP_MAX_REPOS; i++) {
        json_t *j = list->v.arr.items[i];
        if (!j || j->type != JSON_OBJECT) continue;
        warp_repo_t *r = &out[*count];
        memset(r, 0, sizeof(*r));
        strncpy(r->name, json_str(j, "name", ""), sizeof(r->name) - 1);
        if (!r->name[0]) continue;
        r->enabled = json_num(j, "enabled", 1) != 0;
        r->builtin = strcmp(r->name, WARP_DEFAULT_REPO) == 0;
        json_t *mirrors = json_get(j, "mirrors");
        if (mirrors && mirrors->type == JSON_ARRAY) {
            for (int m = 0; m < mirrors->v.arr.count && r->mirror_count < WARP_MAX_MIRRORS; m++) {
                json_t *u = mirrors->v.arr.items[m];
                if (u && u->type == JSON_STRING && u->v.s[0])
                    strncpy(r->mirrors[r->mirror_count++], u->v.s, WARP_MAX_URL - 1);
            }
        }
        const char *pub = json_str(j, "pubkey", "");
        size_t klen = 0;
        if (warp_hex_decode(pub, r->pubkey, 32, &klen) != WARP_OK || klen != 32) {
            if (r->builtin) {
                memcpy(r->pubkey, WARP_MASTER_PUBKEY, 32);
            } else {
                warp_warn("repo '%s' has no valid public key — disabled", r->name);
                r->enabled = 0;
            }
        }
        if (r->mirror_count == 0) {
            if (r->builtin) {
                for (int m = 0; m < WARP_INDEX_MIRRORS; m++)
                    strncpy(r->mirrors[r->mirror_count++], g_warp_mirrors[m], WARP_MAX_URL - 1);
            } else {
                warp_warn("repo '%s' has no mirrors — disabled", r->name);
                r->enabled = 0;
            }
        }
        (*count)++;
    }
    json_free(root);
    if (*count == 0) builtin_repo(&out[(*count)++]);
    return WARP_OK;
}

static void hex_encode(const uint8_t *in, size_t len, char *out) {
    for (size_t i = 0; i < len; i++) sprintf(out + 2 * i, "%02x", in[i]);
    out[2 * len] = '\0';
}

int repos_save(const warp_repo_t *repos, int count) {
    if (mkdirs(WARP_STORE_DIR, 0755) != WARP_OK) return WARP_ERR_IO;
    FILE *f = fopen(WARP_REPOS_CONF, "w");
    if (!f) return WARP_ERR_IO;
    fprintf(f, "{\n  \"repos\": [\n");
    for (int i = 0; i < count; i++) {
        const warp_repo_t *r = &repos[i];
        char pub[65];
        hex_encode(r->pubkey, 32, pub);
        fprintf(f, "    {\n      \"name\": \"%s\",\n      \"enabled\": %s,\n      \"pubkey\": \"%s\",\n      \"mirrors\": [",
                r->name, r->enabled ? "true" : "false", pub);
        for (int m = 0; m < r->mirror_count; m++)
            fprintf(f, "%s\"%s\"", m ? ", " : "", r->mirrors[m]);
        fprintf(f, "]\n    }%s\n", i + 1 < count ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    return WARP_OK;
}

const warp_repo_t *repos_find(const warp_repo_t *repos, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (strcmp(repos[i].name, name) == 0) return &repos[i];
    return NULL;
}

void repo_cache_dir(const warp_repo_t *r, char *out, size_t cap) {
    snprintf(out, cap, WARP_REPOS_DIR "/%s", r->name);
}
