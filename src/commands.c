#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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

    /* Download to temp file — try P2P first, fall back to direct */
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/warp-%s.warp", name);

    int downloaded_ok = 0;

    if (entry.sha256[0] && idx.peer_list_url[0]) {
        warp_peer_list_t peers;
        if (p2p_load_peers(&peers, idx.peer_list_url) == WARP_OK && peers.count > 0) {
            warp_info("Found %d peer(s) — trying P2P download...", peers.count);
            if (p2p_download(entry.name, entry.sha256, tmp_path, &peers) == WARP_OK) {
                downloaded_ok = 1;   /* SHA256 already verified inside p2p_download */
            } else {
                warp_warn("P2P failed — falling back to direct download");
            }
        } else {
            warp_warn("No peers available — downloading directly");
        }
    }

    warp_dl_opts_t dl = { .show_progress = 1 };
    if (!downloaded_ok) {
        warp_info("Downloading %s ...", entry.url);
        if (warp_download(entry.url, tmp_path, &dl) != WARP_OK) {
            warp_err("Download failed");
            return 1;
        }

        /* Verify SHA256 from direct download */
        if (entry.sha256[0]) {
            warp_info("Verifying integrity...");
            if (strcmp(dl.computed_sha256, entry.sha256) != 0) {
                warp_err("SHA256 mismatch!");
                warp_err("  Expected: %s", entry.sha256);
                warp_err("  Got:      %s", dl.computed_sha256);
                remove(tmp_path);
                return 1;
            }
        } else {
            warp_warn("No SHA256 in index — skipping integrity check");
        }
    }
    if (downloaded_ok || entry.sha256[0]) {
        warp_ok("SHA256 verified");
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

    /* Announce to tracker so others can download from us */
    if (idx.peer_list_url[0] || WARP_TRACKER_URL[0]) {
        const char *tracker = idx.peer_list_url[0]
                              ? idx.peer_list_url : WARP_TRACKER_URL;
        /* Tracker announce base should not include dashboard or peer-list paths. */
        char announce_url[WARP_MAX_URL];
        snprintf(announce_url, sizeof(announce_url), "%s", tracker);
        /* Remove trailing tracker UI/API suffixes if present */
        char *tail = strstr(announce_url, "/dashboard");
        if (tail) *tail = '\0';
        tail = strstr(announce_url, "/peers");
        if (tail) *tail = '\0';

        if (p2p_announce(announce_url, entry.name, entry.sha256, WARP_PEER_PORT, 0) == WARP_OK)
            warp_info("Announced to tracker — now seeding %s", name);
    }

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

/* ── warp seed [--port N] ────────────────────────────────────── */
int cmd_seed(int argc, char **argv) {
    int port = WARP_PEER_PORT;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    warp_installed_t *list;
    int count;
    if (store_list(&list, &count) != WARP_OK || count == 0) {
        warp_warn("No packages installed — nothing to seed");
        return 0;
    }

    printf("\n  " WARP_BOLD "Seeding %d package(s):" WARP_RESET "\n", count);
    for (int i = 0; i < count; i++)
        printf("  " WARP_GREEN "  %-20s" WARP_RESET " %s\n",
               list[i].name, list[i].version);
    printf("\n");
    store_free_list(list, count);

    /* Announce full store to tracker before starting seed server */
    warp_info("Announcing to tracker...");
    if (p2p_announce(WARP_TRACKER_URL, NULL, NULL, port, 0) == WARP_OK)
        warp_info("Announced — visible to peers");
    else
        warp_warn("Tracker unreachable — seeding locally only");

    p2p_seed(port);
    return 0;
}

/* ── warp volunteer [--quota N] [--no-serve] [--monthly N] ──── */
int cmd_volunteer(int argc, char **argv) {
    warp_seed_config_t cfg;
    int have_cfg = (seed_config_load(&cfg) == WARP_OK);
    int do_setup = !have_cfg;
    int port     = WARP_PEER_PORT;

    /* Parse flags */
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--setup") == 0) {
            do_setup = 1;
        } else if (strcmp(argv[i], "--cheap-sd") == 0) {
            cfg.cheap_sd = 1;
            if (!have_cfg) { memset(&cfg, 0, sizeof(cfg)); cfg.cheap_sd = 1; cfg.serve = 1; }
        } else if (strcmp(argv[i], "--status") == 0) {
            if (!have_cfg) {
                printf("\n  No volunteer config yet.\n");
                printf("  Run: warp volunteer --setup\n\n");
            } else {
                char sz_used[32], sz_quota[32], sz_lim[32], sz_uploaded[32];
                size_t used = volunteer_used_bytes();
                if      (used >= (size_t)1073741824) snprintf(sz_used, sizeof(sz_used), "%.2f GB", (double)used / 1073741824.0);
                else if (used >= (size_t)1048576)    snprintf(sz_used, sizeof(sz_used), "%.1f MB", (double)used / 1048576.0);
                else                                 snprintf(sz_used, sizeof(sz_used), "%.1f KB", (double)used / 1024.0);

                if (cfg.quota_bytes >= (size_t)1073741824)
                    snprintf(sz_quota, sizeof(sz_quota), "%.2f GB", (double)cfg.quota_bytes / 1073741824.0);
                else snprintf(sz_quota, sizeof(sz_quota), "%.1f MB", (double)cfg.quota_bytes / 1048576.0);

                if (cfg.monthly_limit_bytes)
                    snprintf(sz_lim, sizeof(sz_lim), "%.2f GB", (double)cfg.monthly_limit_bytes / 1073741824.0);
                else snprintf(sz_lim, sizeof(sz_lim), "unlimited");

                if (cfg.monthly_used_bytes >= (size_t)1048576)
                    snprintf(sz_uploaded, sizeof(sz_uploaded), "%.1f MB", (double)cfg.monthly_used_bytes / 1048576.0);
                else snprintf(sz_uploaded, sizeof(sz_uploaded), "%.1f KB", (double)cfg.monthly_used_bytes / 1024.0);

                printf("\n  " WARP_BOLD "Volunteer status:" WARP_RESET "\n\n");
                printf("  Cache:         %s / %s\n", sz_used, sz_quota);
                printf("  Serving:       %s\n",  cfg.serve ? WARP_GREEN "yes" WARP_RESET : WARP_YELLOW "no" WARP_RESET);
                printf("  Cheap SD mode: %s\n",  cfg.cheap_sd ? WARP_GREEN "yes (cache in RAM /tmp)" WARP_RESET : WARP_YELLOW "no" WARP_RESET);
                printf("  Monthly limit: %s", sz_lim);
                if (cfg.monthly_limit_bytes)
                    printf(" (uploaded this month: %s)\n", sz_uploaded);
                else printf("\n");
                printf("  Month:         %s\n\n", cfg.month_tag);
            }
            return 0;
        } else if (strcmp(argv[i], "--no-serve") == 0) {
            cfg.serve = 0;
            if (!have_cfg) { memset(&cfg, 0, sizeof(cfg)); cfg.serve = 0; }
        } else if (strcmp(argv[i], "--quota") == 0 && i + 1 < argc) {
            if (!have_cfg) memset(&cfg, 0, sizeof(cfg));
            cfg.quota_bytes = 0;
            /* parse_size is in p2p.c — call volunteer with this flag instead */
            const char *qs = argv[++i];
            double val = strtod(qs, NULL);
            char last = qs[strlen(qs) - 1] | 0x20;
            if      (last == 'g') cfg.quota_bytes = (size_t)(val * 1073741824.0);
            else if (last == 'm') cfg.quota_bytes = (size_t)(val * 1048576.0);
            else if (last == 'k') cfg.quota_bytes = (size_t)(val * 1024.0);
            else                  cfg.quota_bytes = (size_t)val;
            cfg.serve = 1;
            do_setup = 0;
        } else if (strcmp(argv[i], "--monthly") == 0 && i + 1 < argc) {
            const char *ms = argv[++i];
            double val = strtod(ms, NULL);
            char last = ms[strlen(ms) - 1] | 0x20;
            if      (last == 'g') cfg.monthly_limit_bytes = (size_t)(val * 1073741824.0);
            else if (last == 'm') cfg.monthly_limit_bytes = (size_t)(val * 1048576.0);
            else if (last == 'k') cfg.monthly_limit_bytes = (size_t)(val * 1024.0);
            else                  cfg.monthly_limit_bytes = (size_t)val;
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
    }

    if (do_setup) {
        /* Interactive wizard */
        printf("\n  " WARP_BOLD "WARP Volunteer Seeding Setup" WARP_RESET "\n\n");
        printf("  Volunteer mode downloads and seeds less popular packages.\n");
        printf("  You control disk space and bandwidth limits.\n\n");

        memset(&cfg, 0, sizeof(cfg));
        cfg.serve = 1;

        char inp[64];

        /* Quota */
        while (cfg.quota_bytes == 0) {
            printf("  Disk quota for volunteer cache [e.g. 10G, 500M]: ");
            fflush(stdout);
            if (!fgets(inp, sizeof(inp), stdin)) return 1;
            inp[strcspn(inp, "\r\n")] = '\0';
            double val = strtod(inp, NULL);
            char last = inp[strlen(inp) - 1] | 0x20;
            if      (last == 'g') cfg.quota_bytes = (size_t)(val * 1073741824.0);
            else if (last == 'm') cfg.quota_bytes = (size_t)(val * 1048576.0);
            else if (last == 'k') cfg.quota_bytes = (size_t)(val * 1024.0);
            else                  cfg.quota_bytes = (size_t)val;
            if (cfg.quota_bytes == 0)
                printf("  " WARP_RED "Invalid size." WARP_RESET " Try e.g. 10G or 500M.\n");
        }

        /* Cheap SD mode */
        printf("  Cheap SD-card mode? (uses RAM /tmp for volunteer cache to prevent write wear) [y/N]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        cfg.cheap_sd = (inp[0] == 'y' || inp[0] == 'Y') ? 1 : 0;

        /* Serve */
        printf("  Serve files to peers? (slower internet = answer n) [Y/n]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        cfg.serve = (inp[0] == 'n' || inp[0] == 'N') ? 0 : 1;

        /* Monthly limit */
        printf("  Monthly upload limit (0 or Enter = unlimited) [0]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        inp[strcspn(inp, "\r\n")] = '\0';
        if (inp[0] && inp[0] != '0') {
            double val = strtod(inp, NULL);
            char last = inp[strlen(inp) - 1] | 0x20;
            if      (last == 'g') cfg.monthly_limit_bytes = (size_t)(val * 1073741824.0);
            else if (last == 'm') cfg.monthly_limit_bytes = (size_t)(val * 1048576.0);
            else                  cfg.monthly_limit_bytes = (size_t)val;
        }

        /* Init month tag */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        strftime(cfg.month_tag, sizeof(cfg.month_tag), "%Y-%m", tm_info);

        if (seed_config_save(&cfg) != WARP_OK) {
            warp_err("Failed to save config to %s", WARP_SEED_CONF);
            return 1;
        }
        warp_ok("Config saved: %s", WARP_SEED_CONF);
    }

    if (cfg.quota_bytes == 0) {
        warp_err("No quota set. Use --quota 10G or run --setup.");
        return 1;
    }

    if (store_init() != WARP_OK) return 1;

    /* If cheap SD mode is active, reload config to apply the RAM path immediately */
    if (cfg.cheap_sd) {
        seed_config_load(&cfg);
    }

    printf("\n  " WARP_BOLD "Starting volunteer mode" WARP_RESET "\n");
    printf("  Quota:    ");
    if (cfg.quota_bytes >= (size_t)1073741824)
        printf("%.2f GB\n", (double)cfg.quota_bytes / 1073741824.0);
    else printf("%.1f MB\n", (double)cfg.quota_bytes / 1048576.0);
    printf("  Serving:  %s\n", cfg.serve ? "yes" : "no (cache only)");
    printf("  Cheap SD: %s\n", cfg.cheap_sd ? "yes" : "no");
    if (cfg.monthly_limit_bytes)
        printf("  Monthly:  %.2f GB\n\n", (double)cfg.monthly_limit_bytes / 1073741824.0);
    else printf("  Monthly:  unlimited\n\n");

    p2p_volunteer(&cfg, port);
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
    printf("    \"url\": \"https://github.com/KEYTRON/K1OS/releases/download/packages/%s\"\n", out_name);
    printf("  }\n\n");

    return 0;
}
