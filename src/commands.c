#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "warp.h"

/* ── warp install <pkg> ──────────────────────────────────────── */
int cmd_install(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp install <package>"); return 1; }
    const char *name = argv[0];

    if (store_init() != WARP_OK) return 1;

    /* Check already installed */
    warp_installed_t info;
    if (store_is_installed(name, &info)) {
        warp_warn("%s is already installed (version %s)", name, info.version);
        printf("  Use 'warp rollback %s' to revert to previous version.\n", name);
        return 0;
    }

    /* Load index */
    warp_index_t idx;
    if (index_load(&idx, 0) != WARP_OK) {
        warp_err("Cannot load package index");
        return 1;
    }

    warp_pkg_entry_t entry;
    if (index_find(&idx, name, &entry) != WARP_OK) {
        warp_err("Package not found: %s", name);
        warp_info("Try: warp search %s", name);
        index_free(&idx);
        return 1;
    }
    index_free(&idx);

    printf("\n  " WARP_BOLD "%s" WARP_RESET " %s\n", entry.name, entry.version);
    if (entry.description[0])
        printf("  %s\n", entry.description);
    if (entry.size > 0)
        printf("  Size: %.1f KB\n\n", (double)entry.size / 1024.0);

    /* Download to temp file */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/warp-%s.warp", name);

    warp_info("Downloading %s ...", entry.url);
    warp_dl_opts_t dl = { .show_progress = 1 };
    if (warp_download(entry.url, tmp_path, &dl) != WARP_OK) {
        warp_err("Download failed");
        return 1;
    }

    /* Verify SHA256 */
    warp_info("Verifying integrity...");
    if (entry.sha256[0]) {
        if (strcmp(dl.computed_sha256, entry.sha256) != 0) {
            warp_err("SHA256 mismatch!");
            warp_err("  Expected: %s", entry.sha256);
            warp_err("  Got:      %s", dl.computed_sha256);
            remove(tmp_path);
            return 1;
        }
        warp_ok("SHA256 verified");
    } else {
        warp_warn("No SHA256 in index — skipping integrity check");
    }

    /* Parse manifest from archive */
    char manifest_tmp[512];
    snprintf(manifest_tmp, sizeof(manifest_tmp), "/tmp/warp-%s-manifest.json", name);
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -O manifest.json > %s 2>/dev/null",
             tmp_path, manifest_tmp);
    system(cmd);

    warp_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    strncpy(manifest.name,    name,          WARP_MAX_NAME-1);
    strncpy(manifest.version, entry.version, WARP_MAX_NAME-1);

    size_t mlen;
    char *ms = read_file(manifest_tmp, &mlen);
    if (ms) {
        json_t *jm = json_parse(ms);
        free(ms);
        if (jm) {
            const char *bins_key = "install_bins";
            json_t *bins = json_get(jm, bins_key);
            if (bins && bins->type == JSON_ARRAY) {
                manifest.bins_count = 0;
                for (int i = 0; i < bins->v.arr.count && i < WARP_MAX_BINS; i++) {
                    json_t *b = bins->v.arr.items[i];
                    if (b && b->type == JSON_STRING) {
                        strncpy(manifest.install_bins[manifest.bins_count++],
                                b->v.s, WARP_MAX_NAME-1);
                    }
                }
            }
            json_free(jm);
        }
    }
    remove(manifest_tmp);

    /* Add to store */
    char hash12[13];
    strncpy(hash12, dl.computed_sha256, 12);
    hash12[12] = '\0';

    warp_info("Installing to store...");
    if (store_add(&manifest, tmp_path, dl.computed_sha256) != WARP_OK) {
        warp_err("Failed to add to store");
        remove(tmp_path);
        return 1;
    }
    remove(tmp_path);

    /* Activate */
    if (store_activate(name, hash12) != WARP_OK) {
        warp_err("Failed to activate package");
        return 1;
    }

    warp_ok("Installed: %s %s", name, entry.version);
    return 0;
}

/* ── warp remove <pkg> ───────────────────────────────────────── */
int cmd_remove(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp remove <package>"); return 1; }
    const char *name = argv[0];

    if (!store_is_installed(name, NULL)) {
        warp_err("Not installed: %s", name);
        return 1;
    }

    if (store_remove(name) != WARP_OK) return 1;
    warp_ok("Removed: %s", name);
    return 0;
}

/* ── warp list ───────────────────────────────────────────────── */
int cmd_list(int argc, char **argv) {
    (void)argc; (void)argv;
    warp_installed_t *list;
    int count;
    if (store_list(&list, &count) != WARP_OK) return 1;

    if (count == 0) {
        printf("  No packages installed.\n");
        printf("  Try: warp search <query>\n");
        return 0;
    }

    printf("\n  " WARP_BOLD "Installed packages:" WARP_RESET "\n\n");
    printf("  %-20s %-12s %s\n", "Package", "Version", "Store ID");
    printf("  %-20s %-12s %s\n", "-------", "-------", "--------");
    for (int i = 0; i < count; i++) {
        printf("  " WARP_GREEN "%-20s" WARP_RESET " %-12s %s\n",
               list[i].name, list[i].version, list[i].hash12);
    }
    printf("\n");
    store_free_list(list, count);
    return 0;
}

/* ── warp search <query> ─────────────────────────────────────── */
int cmd_search(int argc, char **argv) {
    const char *query = argc > 0 ? argv[0] : "";

    warp_index_t idx;
    if (index_load(&idx, 0) != WARP_OK) {
        warp_err("Cannot load package index");
        return 1;
    }

    if (idx.count == 0) {
        printf("  Index is empty.\n");
        index_free(&idx);
        return 0;
    }

    warp_pkg_entry_t *results;
    int count;
    index_search(&idx, query, &results, &count);

    if (count == 0) {
        printf("  No packages matching '%s'\n", query);
    } else {
        printf("\n  " WARP_BOLD "%-20s %-12s %s" WARP_RESET "\n\n",
               "Package", "Version", "Description");
        for (int i = 0; i < count; i++) {
            /* Mark installed */
            const char *marker = store_is_installed(results[i].name, NULL)
                                  ? WARP_GREEN " [installed]" WARP_RESET : "";
            printf("  " WARP_CYAN "%-20s" WARP_RESET " %-12s %s%s\n",
                   results[i].name, results[i].version,
                   results[i].description, marker);
        }
        printf("\n");
    }

    free(results);
    index_free(&idx);
    return 0;
}

/* ── warp rollback <pkg> ─────────────────────────────────────── */
int cmd_rollback(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp rollback <package>"); return 1; }
    const char *name = argv[0];

    if (store_rollback(name) != WARP_OK) return 1;
    warp_ok("Rolled back: %s", name);
    return 0;
}

/* ── warp info <pkg> ─────────────────────────────────────────── */
int cmd_info(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp info <package>"); return 1; }
    const char *name = argv[0];

    /* Show installed info */
    warp_installed_t inst;
    int installed = store_is_installed(name, &inst);
    if (installed) {
        printf("\n  " WARP_BOLD "%s" WARP_RESET " [installed]\n", name);
        printf("  Version:    %s\n", inst.version);
        printf("  Store ID:   %s\n", inst.hash12);
        printf("  Store path: %s\n\n", inst.store_path);
    }

    /* Show index info */
    warp_index_t idx;
    if (index_load(&idx, 0) == WARP_OK) {
        warp_pkg_entry_t entry;
        if (index_find(&idx, name, &entry) == WARP_OK) {
            if (!installed) printf("\n  " WARP_BOLD "%s" WARP_RESET "\n", name);
            printf("  Latest:     %s\n", entry.version);
            printf("  Size:       %.1f KB\n", (double)entry.size / 1024.0);
            printf("  SHA256:     %.16s...\n", entry.sha256);
            printf("  URL:        %s\n\n", entry.url);
        } else if (!installed) {
            warp_err("Package not found: %s", name);
        }
        index_free(&idx);
    }

    return installed ? 0 : 1;
}

/* ── warp update ─────────────────────────────────────────────── */
int cmd_update(int argc, char **argv) {
    (void)argc; (void)argv;
    if (store_init() != WARP_OK) return 1;
    warp_index_t idx;
    if (index_load(&idx, 1) != WARP_OK) return 1;
    warp_ok("Index updated: %d packages available", idx.count);
    index_free(&idx);
    return 0;
}

/* ── warp keygen ─────────────────────────────────────────────── */
int cmd_keygen(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *priv = "/root/.warp-privkey.hex";
    const char *pub  = "/root/.warp-pubkey.hex";
    printf("\n  Generating Ed25519 keypair...\n\n");
    if (warp_keygen(priv, pub) != WARP_OK) {
        warp_err("keygen failed");
        return 1;
    }
    printf("\n");
    warp_ok("Private key: %s", priv);
    warp_ok("Public key:  %s", pub);
    printf("\n  " WARP_YELLOW "Keep the private key secure!" WARP_RESET "\n");
    printf("  Paste the C array above into packages/warp/src/crypto.c\n\n");
    return 0;
}

/* ── warp pack <dir> ─────────────────────────────────────────── */
int cmd_pack(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp pack <directory>"); return 1; }
    const char *dir = argv[0];

    if (!path_exists(dir)) {
        warp_err("Directory not found: %s", dir);
        return 1;
    }

    /* Check manifest exists */
    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", dir);
    if (!path_exists(manifest_path)) {
        warp_err("manifest.json not found in %s", dir);
        return 1;
    }

    /* Read name/version from manifest */
    size_t mlen;
    char *ms = read_file(manifest_path, &mlen);
    if (!ms) { warp_err("Cannot read manifest.json"); return 1; }

    json_t *jm = json_parse(ms);
    free(ms);
    char pkg_name[WARP_MAX_NAME], pkg_ver[WARP_MAX_NAME];
    strncpy(pkg_name, json_str(jm, "name",    "unknown"), WARP_MAX_NAME-1);
    strncpy(pkg_ver,  json_str(jm, "version", "0.0.0"),   WARP_MAX_NAME-1);
    pkg_name[WARP_MAX_NAME-1] = pkg_ver[WARP_MAX_NAME-1] = '\0';
    json_free(jm); jm = NULL;

    char out_name[512];
    snprintf(out_name, sizeof(out_name), "%s-%s-%s.warp", pkg_name, pkg_ver, WARP_ARCH);

    /* Create tar.gz */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "tar -czf %s -C %s manifest.json files/ 2>/dev/null || "
             "tar -czf %s -C %s .",
             out_name, dir, out_name, dir);
    if (system(cmd) != 0) {
        json_free(jm);
        warp_err("Failed to create archive");
        return 1;
    }

    /* Compute and print sha256 */
    char sha256[WARP_SHA256_HEX];
    warp_sha256_file(out_name, sha256);
    long sz = file_size(out_name);

    warp_ok("Created: %s", out_name);
    printf("  SHA256: %s\n", sha256);
    printf("  Size:   %ld bytes\n\n", sz);
    printf("  Add to index.json:\n");
    printf("  \"%s\": {\n", pkg_name);
    printf("    \"version\": \"%s\",\n", pkg_ver);
    printf("    \"description\": \"...\",\n");
    printf("    \"sha256\": \"%s\",\n", sha256);
    printf("    \"size\": %ld,\n", sz);
    printf("    \"url\": \"https://github.com/KEYTRON/WARP/releases/download/packages/%s\"\n", out_name);
    printf("  }\n\n");

    return 0;
}
