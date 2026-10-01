#include <stdio.h>
#include <signal.h>
#include <strings.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "warp.h"

/* ── shared install machinery ────────────────────────────────── */

/* Fetch the archive for `entry` into `tmp_path`: P2P first, then the
 * published URL and the repository's mirrors. Returns WARP_OK only if the
 * bytes hash to entry->sha256. */
static int fetch_archive(const warp_pkg_entry_t *entry, const warp_repo_t *repo,
                         const char *peer_list_url, const char *tmp_path) {
    if (peer_list_url && peer_list_url[0]) {
        warp_peer_list_t peers;
        if (p2p_load_peers(&peers, peer_list_url) == WARP_OK && peers.count > 0) {
            warp_info("Found %d peer(s) — trying P2P download...", peers.count);
            if (p2p_download(entry->name, entry->sha256, tmp_path, &peers) == WARP_OK)
                return WARP_OK;   /* SHA256 already verified inside p2p_download */
            warp_warn("P2P failed — falling back to direct download");
        } else {
            warp_warn("No peers available — downloading directly");
        }
    }

    warp_dl_opts_t dl = { .show_progress = 1 };
    if (warp_download_pkg(entry->url, repo, tmp_path, &dl) != WARP_OK) {
        warp_err("Download failed");
        return WARP_ERR_NET;
    }
    warp_info("Verifying integrity...");
    if (strcmp(dl.computed_sha256, entry->sha256) != 0) {
        warp_err("SHA256 mismatch!");
        warp_err("  Expected: %s", entry->sha256);
        warp_err("  Got:      %s", dl.computed_sha256);
        remove(tmp_path);
        return WARP_ERR_HASH;
    }
    return WARP_OK;
}

/* Rebuild the new archive from an installed version plus a published delta.
 * Returns WARP_OK with the archive at tmp_path (sha256 == entry->sha256),
 * or an error so the caller can fall back to a full download. */
static int fetch_via_delta(const warp_pkg_entry_t *entry, const warp_repo_t *repo,
                           const warp_installed_t *installed, const char *tmp_path) {
    char sha_path[600], old_archive[600];
    snprintf(sha_path,    sizeof(sha_path),    "%s/sha256",       installed->store_path);
    snprintf(old_archive, sizeof(old_archive), "%s/package.warp", installed->store_path);
    size_t n = 0;
    char *old_sha = read_file(sha_path, &n);
    if (!old_sha || !path_exists(old_archive)) { free(old_sha); return WARP_ERR_NOENT; }
    old_sha[strcspn(old_sha, "\r\n")] = '\0';

    const warp_delta_ref_t *ref = NULL;
    for (int i = 0; i < entry->delta_count; i++)
        if (strcmp(entry->deltas[i].from_sha256, old_sha) == 0) { ref = &entry->deltas[i]; break; }
    free(old_sha);
    if (!ref) return WARP_ERR_NOENT;

    printf("  Delta available: %.1f KB instead of %.1f KB (%.0f%% less)\n",
           (double)ref->size / 1024.0, (double)entry->size / 1024.0,
           entry->size ? 100.0 * (1.0 - (double)ref->size / (double)entry->size) : 0.0);

    char delta_tmp[600];
    snprintf(delta_tmp, sizeof(delta_tmp), "%s.delta", tmp_path);
    warp_dl_opts_t dl = { .show_progress = 1 };
    if (warp_download_pkg(ref->url, repo, delta_tmp, &dl) != WARP_OK) {
        warp_warn("Delta download failed");
        return WARP_ERR_NET;
    }
    if (strcmp(dl.computed_sha256, ref->sha256) != 0) {
        warp_warn("Delta SHA256 mismatch — ignoring it");
        remove(delta_tmp);
        return WARP_ERR_HASH;
    }

    char out_sha[WARP_SHA256_HEX];
    int rc = delta_apply(old_archive, delta_tmp, tmp_path, entry->sha256, out_sha);
    remove(delta_tmp);
    if (rc != WARP_OK) return rc;
    if (strcmp(out_sha, entry->sha256) != 0) {
        warp_warn("Rebuilt archive does not match the index — falling back to full download");
        remove(tmp_path);
        return WARP_ERR_HASH;
    }
    warp_ok("Archive rebuilt from delta, SHA256 verified");
    return WARP_OK;
}

/* Extract the manifest, add the (already verified) archive to the store
 * and activate it. */
static int install_archive(const warp_pkg_entry_t *entry, const char *tmp_path) {
    char manifest_tmp[600];
    snprintf(manifest_tmp, sizeof(manifest_tmp), "/tmp/warp-%s-manifest.json", entry->name);
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "tar -xzf %s -O manifest.json > %s 2>/dev/null",
             tmp_path, manifest_tmp);
    system(cmd);

    warp_manifest_t manifest;
    memset(&manifest, 0, sizeof(manifest));
    strncpy(manifest.name,    entry->name,    WARP_MAX_NAME-1);
    strncpy(manifest.version, entry->version, WARP_MAX_NAME-1);

    size_t mlen;
    char *ms = read_file(manifest_tmp, &mlen);
    if (ms) {
        json_t *jm = json_parse(ms);
        free(ms);
        if (jm) {
            /* The signed index is authoritative; a manifest that claims to be
             * something else means the archive is not what the index promised
             * (or the publisher mislabelled it) — either way, refuse. */
            json_t *jn = json_get(jm, "name");
            json_t *jv = json_get(jm, "version");
            if ((jn && jn->type == JSON_STRING && strcmp(jn->v.s, entry->name) != 0) ||
                (jv && jv->type == JSON_STRING && strcmp(jv->v.s, entry->version) != 0)) {
                warp_err("Archive manifest says %s %s, index says %s %s — refusing",
                         jn && jn->type == JSON_STRING ? jn->v.s : "?",
                         jv && jv->type == JSON_STRING ? jv->v.s : "?",
                         entry->name, entry->version);
                json_free(jm);
                remove(tmp_path);
                return WARP_ERR_SIG;
            }
            json_t *jd = json_get(jm, "deps");
            if (jd && jd->type == JSON_ARRAY) {
                for (int i = 0; i < jd->v.arr.count; i++) {
                    json_t *d = jd->v.arr.items[i];
                    if (!d || d->type != JSON_STRING) continue;
                    char dep[WARP_MAX_NAME];
                    strncpy(dep, d->v.s, sizeof(dep) - 1);
                    dep[sizeof(dep) - 1] = '\0';
                    dep[strcspn(dep, "@=<>~ ")] = '\0';        /* drop any version qualifier */
                    int known = 0;
                    for (int k = 0; k < entry->dep_count && !known; k++)
                        known = strcmp(entry->deps[k].name, dep) == 0;
                    if (!known) {
                        warp_err("Archive manifest depends on '%s' but the index does not list it — refusing", dep);
                        json_free(jm);
                        remove(tmp_path);
                        return WARP_ERR_SIG;
                    }
                }
            }
            json_t *bins = json_get(jm, "install_bins");
            if (bins && bins->type == JSON_ARRAY) {
                for (int i = 0; i < bins->v.arr.count && i < WARP_MAX_BINS; i++) {
                    json_t *b = bins->v.arr.items[i];
                    if (b && b->type == JSON_STRING)
                        strncpy(manifest.install_bins[manifest.bins_count++], b->v.s, WARP_MAX_NAME-1);
                }
            }
            json_free(jm);
        }
    }
    remove(manifest_tmp);

    char hash12[13];
    strncpy(hash12, entry->sha256, 12);
    hash12[12] = '\0';

    warp_info("Installing to store...");
    if (store_add(&manifest, tmp_path, entry->sha256) != WARP_OK) {
        warp_err("Failed to add to store");
        remove(tmp_path);
        return WARP_ERR_IO;
    }
    remove(tmp_path);

    if (store_activate(entry->name, hash12) != WARP_OK) {
        warp_err("Failed to activate package");
        return WARP_ERR_IO;
    }
    return WARP_OK;
}

/* A repository's own tracker, from its signed index. Only the built-in k1os
 * repository falls back to the compiled-in tracker: a third-party repository
 * without a tracker gets no P2P, and its installs are never reported to ours. */
static const char *repo_tracker(const warp_repo_t *repo) {
    if (repo->peer_list_url[0]) return repo->peer_list_url;
    return repo->builtin ? WARP_TRACKER_URL "/peers" : "";
}

static void announce(const warp_pkg_entry_t *entry, const warp_repo_t *repo) {
    const char *tracker = repo_tracker(repo);
    if (!tracker[0]) return;
    /* Tracker announce base should not include dashboard or peer-list paths. */
    char announce_url[WARP_MAX_URL];
    snprintf(announce_url, sizeof(announce_url), "%s", tracker);
    char *tail = strstr(announce_url, "/dashboard");
    if (tail) *tail = '\0';
    tail = strstr(announce_url, "/peers");
    if (tail) *tail = '\0';
    if (p2p_announce(announce_url, entry->name, entry->sha256, WARP_PEER_PORT, 0) == WARP_OK)
        warp_info("Announced to tracker — now seeding %s", entry->name);
}

static void print_entry_header(const warp_pkg_entry_t *entry) {
    printf("\n  " WARP_BOLD "%s" WARP_RESET " %s  [%s]\n", entry->name, entry->version, entry->repo);
    if (entry->description[0]) printf("  %s\n", entry->description);
    if (entry->size > 0) printf("  Size: %.1f KB\n\n", (double)entry->size / 1024.0);
}

/* Install (or upgrade to) `entry`. `installed` is the currently active
 * version when upgrading, NULL for a fresh install. */
static int install_entry(const warp_index_t *idx, const warp_pkg_entry_t *entry,
                         const warp_installed_t *installed) {
    /* WARP installs prebuilt packages only: no build for this OS/CPU means no
     * install, never a download of someone else's binary or a local compile. */
    if (entry->no_build) {
        warp_err("'%s' has no build for %s", entry->name, warp_platform());
        if (entry->available[0]) warp_err("It is published for: %s", entry->available);
        warp_err("WARP installs prebuilt packages and does not compile anything");
        return WARP_ERR_NOENT;
    }
    /* An unhashed entry can't be verified after download — refuse it
     * outright instead of installing on trust alone. */
    if (!entry->sha256[0]) {
        warp_err("No SHA256 published for '%s' in the index", entry->name);
        warp_err("Refusing to install a package whose integrity can't be verified");
        return WARP_ERR_SIG;
    }
    const warp_repo_t *repo = index_repo_of(idx, entry);
    print_entry_header(entry);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/warp-%s.warp", entry->name);

    int rc = WARP_ERR_NOENT;
    if (installed && entry->delta_count > 0)
        rc = fetch_via_delta(entry, repo, installed, tmp_path);
    if (rc != WARP_OK)
        rc = fetch_archive(entry, repo, repo_tracker(repo), tmp_path);
    if (rc != WARP_OK) return rc;
    warp_ok("SHA256 verified");

    rc = install_archive(entry, tmp_path);
    if (rc != WARP_OK) return rc;

    warp_ok("%s: %s %s", installed ? "Upgraded" : "Installed", entry->name, entry->version);
    announce(entry, repo);
    return WARP_OK;
}

/* ── warp install <pkg>[@version] ────────────────────────────── */
int cmd_install(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp install <package>[@version]"); return 1; }
    const char *spec = argv[0];

    if (store_init() != WARP_OK) return 1;

    /* name@version (also repo/name@version): that release, next to what is there */
    const char *at = strchr(spec, '@');
    if (at) {
        const char *slash = strchr(spec, '/');
        const char *bare = slash && slash < at ? slash + 1 : spec;
        char name[WARP_MAX_NAME];
        size_t bl = (size_t)(at - bare);
        if (bl == 0 || bl >= sizeof(name) || !at[1]) { warp_err("Usage: warp install <package>@<version>"); return 1; }
        memcpy(name, bare, bl);
        name[bl] = '\0';
        const char *want = at + 1;
        if (!store_name_ok(name)) { warp_err("Bad package name: %s", name); return 1; }

        /* Already in the store: no download, just make it the active one. */
        warp_version_t *v; int n;
        store_versions(name, &v, &n);
        int have = 0;
        for (int i = 0; i < n; i++) if (strcmp(v[i].version, want) == 0) have = 1;
        free(v);
        if (have) {
            if (store_switch(name, want) != WARP_OK) return 1;
            warp_ok("%s %s was already in the store: now active", name, want);
            return 0;
        }

        warp_index_t idx;
        if (index_load(&idx, 0) != WARP_OK) { warp_err("Cannot load package index"); return 1; }
        warp_pkg_entry_t entry, latest;
        if (index_find(&idx, spec, &entry) != WARP_OK) {
            warp_err("%s %s is not published", name, want);
            warp_info("Published versions: warp versions %s", name);
            index_free(&idx);
            return 1;
        }
        int is_latest = index_find(&idx, name, &latest) == WARP_OK && strcmp(latest.version, entry.version) == 0;
        int rc = install_entry(&idx, &entry, NULL);
        index_free(&idx);
        if (rc != WARP_OK) return 1;
        /* An older release is chosen on purpose: keep `warp upgrade` from undoing it. */
        if (!is_latest && store_pin_set(name, entry.version) == WARP_OK)
            warp_info("Pinned at %s: 'warp upgrade' leaves it alone ('warp unpin %s' to release)", entry.version, name);
        return 0;
    }

    const char *name = spec;
    warp_installed_t info;
    if (store_is_installed(name, &info)) {
        warp_warn("%s is already installed (version %s)", name, info.version);
        printf("  Use 'warp upgrade %s' to move to the latest version,\n", name);
        printf("  'warp install %s@<version>' for a specific one, or 'warp rollback %s'.\n", name, name);
        return 0;
    }

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
    int rc = install_entry(&idx, &entry, NULL);
    index_free(&idx);
    return rc == WARP_OK ? 0 : 1;
}

/* ── warp upgrade [pkg...] ───────────────────────────────────── */
static int installed_sha(const warp_installed_t *inst, char out[WARP_SHA256_HEX]) {
    char sha_path[600];
    snprintf(sha_path, sizeof(sha_path), "%s/sha256", inst->store_path);
    size_t n = 0;
    char *s = read_file(sha_path, &n);
    if (!s) return WARP_ERR_NOENT;
    s[strcspn(s, "\r\n")] = '\0';
    strncpy(out, s, WARP_SHA256_HEX - 1);
    out[WARP_SHA256_HEX - 1] = '\0';
    free(s);
    return WARP_OK;
}

int cmd_upgrade(int argc, char **argv) {
    if (store_init() != WARP_OK) return 1;

    warp_installed_t *list;
    int count;
    if (store_list(&list, &count) != WARP_OK) return 1;
    if (count == 0) {
        printf("  No packages installed.\n");
        return 0;
    }

    warp_index_t idx;
    if (index_load(&idx, 1) != WARP_OK) {
        warp_err("Cannot load package index");
        store_free_list(list, count);
        return 1;
    }

    int failed = 0, upgraded = 0, checked = 0;
    for (int i = 0; i < count; i++) {
        if (argc > 0) {
            int wanted = 0;
            for (int a = 0; a < argc; a++) if (strcmp(argv[a], list[i].name) == 0) wanted = 1;
            if (!wanted) continue;
        }
        checked++;
        char pin[WARP_MAX_NAME];
        if (store_pin_get(list[i].name, pin, sizeof(pin))) {
            printf("  %-20s pinned at %s, left alone ('warp unpin %s' to release)\n", list[i].name, pin, list[i].name);
            continue;
        }
        warp_pkg_entry_t entry;
        if (index_find(&idx, list[i].name, &entry) != WARP_OK) {
            warp_warn("%s: not in any repository, skipping", list[i].name);
            continue;
        }
        if (entry.no_build) {
            warp_warn("%s: no build for %s in the index, keeping %s", list[i].name, warp_platform(), list[i].version);
            continue;
        }
        char have[WARP_SHA256_HEX] = "";
        installed_sha(&list[i], have);
        if (strcmp(have, entry.sha256) == 0) {
            printf("  %-20s %s is up to date\n", list[i].name, list[i].version);
            continue;
        }
        printf("  %-20s %s -> %s\n", list[i].name, list[i].version, entry.version);
        if (install_entry(&idx, &entry, &list[i]) == WARP_OK) upgraded++; else failed++;
    }
    if (argc > 0 && checked == 0)
        warp_warn("None of the named packages are installed");

    index_free(&idx);
    store_free_list(list, count);
    if (upgraded || failed)
        printf("\n  %d upgraded, %d failed\n\n", upgraded, failed);
    return failed ? 1 : 0;
}

/* ── warp repo list|add|remove|enable|disable ────────────────── */
static void repo_usage(void) {
    warp_err("Usage: warp repo list");
    warp_err("       warp repo add <name> <url> --pubkey <hex64> [--mirror <url>]...");
    warp_err("       warp repo remove|enable|disable <name>");
}

static int repo_name_ok(const char *n) {
    if (!*n || strlen(n) >= WARP_REPO_NAME) return 0;
    for (const char *p = n; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')) return 0;
    return 1;
}

int cmd_repo(int argc, char **argv) {
    if (argc < 1) { repo_usage(); return 1; }
    warp_repo_t repos[WARP_MAX_REPOS];
    int count = 0;
    repos_load(repos, &count);

    if (strcmp(argv[0], "list") == 0 || strcmp(argv[0], "ls") == 0) {
        printf("\n  " WARP_BOLD "%-12s %-8s %s" WARP_RESET "\n\n", "Repository", "State", "Mirrors / key");
        for (int i = 0; i < count; i++) {
            char key[65];
            for (int k = 0; k < 32; k++) sprintf(key + 2 * k, "%02x", repos[i].pubkey[k]);
            printf("  " WARP_CYAN "%-12s" WARP_RESET " %-8s %s%s\n", repos[i].name,
                   repos[i].enabled ? WARP_GREEN "on" WARP_RESET "      " : WARP_YELLOW "off" WARP_RESET "     ",
                   repos[i].mirrors[0], repos[i].builtin ? "  (built-in)" : "");
            for (int m = 1; m < repos[i].mirror_count; m++)
                printf("  %-12s %-8s %s\n", "", "", repos[i].mirrors[m]);
            printf("  %-12s %-8s key %.16s...\n", "", "", key);
        }
        printf("\n");
        return 0;
    }

    if (strcmp(argv[0], "add") == 0) {
        if (argc < 3) { repo_usage(); return 1; }
        const char *name = argv[1];
        if (!repo_name_ok(name)) { warp_err("Repository name must be [a-z0-9_-]"); return 1; }
        if (repos_find(repos, count, name)) { warp_err("Repository '%s' already exists", name); return 1; }
        if (count >= WARP_MAX_REPOS) { warp_err("Too many repositories"); return 1; }

        warp_repo_t r;
        memset(&r, 0, sizeof(r));
        strncpy(r.name, name, sizeof(r.name) - 1);
        strncpy(r.mirrors[r.mirror_count++], argv[2], WARP_MAX_URL - 1);
        r.enabled = 1;
        int have_key = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--pubkey") == 0 && i + 1 < argc) {
                size_t klen = 0;
                if (warp_hex_decode(argv[++i], r.pubkey, 32, &klen) != WARP_OK || klen != 32) {
                    warp_err("--pubkey must be 64 hex characters (Ed25519 public key)");
                    return 1;
                }
                have_key = 1;
            } else if (strcmp(argv[i], "--pubkey-file") == 0 && i + 1 < argc) {
                size_t n = 0;
                char *hex = read_file(argv[++i], &n);
                size_t klen = 0;
                if (!hex || warp_hex_decode(hex, r.pubkey, 32, &klen) != WARP_OK || klen != 32) {
                    warp_err("Cannot read a 32-byte hex public key from %s", argv[i]);
                    free(hex);
                    return 1;
                }
                free(hex);
                have_key = 1;
            } else if (strcmp(argv[i], "--mirror") == 0 && i + 1 < argc) {
                if (r.mirror_count < WARP_MAX_MIRRORS)
                    strncpy(r.mirrors[r.mirror_count++], argv[++i], WARP_MAX_URL - 1);
                else { warp_err("At most %d mirrors per repository", WARP_MAX_MIRRORS); return 1; }
            } else {
                repo_usage();
                return 1;
            }
        }
        if (!have_key) {
            warp_err("A repository needs the public key its index is signed with (--pubkey <hex>)");
            warp_err("Unsigned repositories are not supported: every mirror is untrusted by design");
            return 1;
        }
        repos[count++] = r;
        if (repos_save(repos, count) != WARP_OK) {
            warp_err("Cannot write %s (Permission denied? Run with sudo)", WARP_REPOS_CONF);
            return 1;
        }
        warp_ok("Added repository '%s' (%d mirror%s)", name, r.mirror_count, r.mirror_count > 1 ? "s" : "");
        printf("  Run 'warp update' to fetch its index.\n");
        return 0;
    }

    if (strcmp(argv[0], "remove") == 0 || strcmp(argv[0], "enable") == 0 || strcmp(argv[0], "disable") == 0) {
        if (argc < 2) { repo_usage(); return 1; }
        int at = -1;
        for (int i = 0; i < count; i++) if (strcmp(repos[i].name, argv[1]) == 0) at = i;
        if (at < 0) { warp_err("No such repository: %s", argv[1]); return 1; }
        if (strcmp(argv[0], "remove") == 0) {
            if (repos[at].builtin) { warp_err("The built-in repository can be disabled but not removed"); return 1; }
            for (int i = at; i + 1 < count; i++) repos[i] = repos[i + 1];
            count--;
        } else {
            repos[at].enabled = strcmp(argv[0], "enable") == 0;
        }
        if (repos_save(repos, count) != WARP_OK) {
            warp_err("Cannot write %s (Permission denied? Run with sudo)", WARP_REPOS_CONF);
            return 1;
        }
        warp_ok("%s: %s", argv[0], argv[1]);
        return 0;
    }

    repo_usage();
    return 1;
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
        char pin[WARP_MAX_NAME];
        printf("  " WARP_GREEN "%-20s" WARP_RESET " %-12s %s%s\n",
               list[i].name, list[i].version, list[i].hash12,
               store_pin_get(list[i].name, pin, sizeof(pin)) ? "  " WARP_YELLOW "[pinned]" WARP_RESET : "");
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
            char nb[96];
            snprintf(nb, sizeof(nb), WARP_YELLOW " [no build for %s]" WARP_RESET, warp_platform());
            const char *marker = results[i].no_build
                                  ? nb
                                  : store_is_installed(results[i].name, NULL)
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
            if (entry.no_build) {
                printf("  Build:      none for %s (published for: %s)\n\n", warp_platform(),
                       entry.available[0] ? entry.available : "?");
            } else {
                printf("  Platform:   %s\n", entry.platform[0] ? entry.platform : warp_platform());
                printf("  Size:       %.1f KB\n", (double)entry.size / 1024.0);
                printf("  SHA256:     %.16s...\n", entry.sha256);
                printf("  URL:        %s\n\n", entry.url);
            }
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
    int failed = idx.refresh_failed;
    int latest = 0;
    for (int i = 0; i < idx.count; i++) if (!idx.entries[i].versioned) latest++;
    warp_ok("Index updated: %d packages available", latest);
    index_free(&idx);
    /* Packages from the repositories that did refresh stay usable, but a
     * script or CI must still see that some repository was not updated. */
    if (failed) {
        warp_err("%d repositor%s could not be updated (see above)", failed, failed == 1 ? "y" : "ies");
        return 1;
    }
    return 0;
}

/* ── warp keygen [priv pub] ──────────────────────────────────── */
int cmd_keygen(int argc, char **argv) {
    const char *priv = argc > 0 ? argv[0] : "/root/.warp-privkey.hex";
    const char *pub  = argc > 1 ? argv[1] : "/root/.warp-pubkey.hex";
    printf("\n  Generating Ed25519 keypair...\n\n");
    if (warp_keygen(priv, pub) != WARP_OK) {
        warp_err("keygen failed");
        return 1;
    }
    printf("\n");
    warp_ok("Private key: %s", priv);
    warp_ok("Public key:  %s", pub);
    printf("\n  " WARP_YELLOW "Keep the private key secure!" WARP_RESET "\n");
    printf("  Paste the C array above into src/crypto.c\n\n");
    return 0;
}

/* ── warp sign <file> [privkey_hex] ──────────────────────────── */
int cmd_sign(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp sign <file> [privkey_hex]"); return 1; }
    const char *path = argv[0];
    const char *priv = argc > 1 ? argv[1] : "/root/.warp-privkey.hex";

    if (!path_exists(path)) {
        warp_err("File not found: %s", path);
        return 1;
    }

    char sig[128];
    if (warp_sign_file(path, priv, sig) != WARP_OK) {
        warp_err("Signing failed (missing/invalid private key at %s?)", priv);
        return 1;
    }

    char sig_path[768];
    snprintf(sig_path, sizeof(sig_path), "%s.sig", path);
    FILE *f = fopen(sig_path, "w");
    if (!f) {
        warp_err("Cannot write signature file: %s", sig_path);
        return 1;
    }
    fprintf(f, "%s\n", sig);
    fclose(f);

    warp_ok("Signed: %s", path);
    printf("  Signature file: %s\n", sig_path);
    printf("  Signature:      %s\n\n", sig);
    return 0;
}

/* ── node (seed / volunteer / stats) ─────────────────────────── */

/* "10G", "10.46G", "500M", "all" → bytes or unlimited. Returns 0 on success. */
static int parse_limit(const char *s, size_t *bytes, int *unlimited) {
    if (!s || !*s) return -1;
    if (strcasecmp(s, "all") == 0 || strcasecmp(s, "unlimited") == 0) {
        *unlimited = 1;
        return 0;
    }
    if (!((s[0] >= '0' && s[0] <= '9') || s[0] == '.')) return -1;
    size_t v = warp_parse_size(s);
    if (v == 0) return -1;
    *bytes = v;
    *unlimited = 0;
    return 0;
}

/* "46", "all" → package limit, 0 = no limit. Returns 0 on success. */
static int parse_count(const char *s, size_t *out) {
    if (!s || !*s) return -1;
    if (strcasecmp(s, "all") == 0 || strcasecmp(s, "unlimited") == 0) { *out = 0; return 0; }
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*end || v == 0) return -1;
    *out = (size_t)v;
    return 0;
}

/* Save the config and tell a running node to pick it up. */
static int apply_config(const warp_seed_config_t *cfg) {
    if (store_init() != WARP_OK) return 1;
    if (seed_config_save(cfg) != WARP_OK) {
        warp_err("Cannot write %s (try with sudo)", WARP_SEED_CONF);
        return 1;
    }
    return 0;
}

static void poke_running_node(void) {
    if (p2p_node_signal(SIGHUP) == WARP_OK)
        warp_ok("A running node picked up the new settings");
}

/* Run the node in the foreground; "already running" is not a failure. */
static int run_node(warp_seed_config_t *cfg, int port) {
    int rc = p2p_node_run(cfg, port);
    return (rc == WARP_OK || rc == WARP_ERR_EXIST) ? 0 : 1;
}

/* ── warp seed [--port N] ────────────────────────────────────── */
int cmd_seed(int argc, char **argv) {
    int port = WARP_PEER_PORT;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    warp_seed_config_t cfg;
    seed_config_load(&cfg);          /* defaults when there is no file yet */

    warp_installed_t *list;
    int count;
    int have_pkgs = (store_list(&list, &count) == WARP_OK && count > 0);
    if (have_pkgs) {
        printf("\n  " WARP_BOLD "Seeding %d package(s):" WARP_RESET "\n", count);
        for (int i = 0; i < count; i++)
            printf("  " WARP_GREEN "  %-20s" WARP_RESET " %s\n", list[i].name, list[i].version);
        printf("\n");
        store_free_list(list, count);
    } else if (!cfg.volunteer) {
        warp_warn("No packages installed — nothing to seed");
        return 0;
    }

    if (cfg.volunteer)
        warp_info("Volunteer mode is on: this node also caches rarely seeded packages");

    if (!cfg.consent_asked && stats_ask_consent(&cfg))
        seed_config_save(&cfg);

    return run_node(&cfg, port);
}

/* ── warp volunteer [flags] ──────────────────────────────────── */
int cmd_volunteer(int argc, char **argv) {
    warp_seed_config_t cfg;
    int have_cfg = (seed_config_load(&cfg) == WARP_OK);
    int do_setup = !have_cfg;
    int port     = WARP_PEER_PORT;
    int disable  = 0, status = 0, no_start = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        int more = (i + 1 < argc);
        if      (strcmp(a, "--setup") == 0)    do_setup = 1;
        else if (strcmp(a, "--status") == 0)   status = 1;
        else if (strcmp(a, "--no-start") == 0) no_start = 1;
        else if (strcmp(a, "--enable") == 0)   { cfg.volunteer = 1; do_setup = 0; }
        else if (strcmp(a, "--disable") == 0)  disable = 1;
        else if (strcmp(a, "--no-serve") == 0) { cfg.serve = 0; do_setup = 0; }
        else if (strcmp(a, "--cheap-sd") == 0) { cfg.cheap_sd = 1; do_setup = 0; }
        else if (strcmp(a, "--quota") == 0 && more) {
            if (parse_limit(argv[++i], &cfg.quota_bytes, &cfg.unlimited) != 0) {
                warp_err("Bad --quota value. Examples: 10G, 10.46G, 500M, all");
                return 1;
            }
            do_setup = 0;
        } else if (strcmp(a, "--packages") == 0 && more) {
            if (parse_count(argv[++i], &cfg.max_packages) != 0) {
                warp_err("Bad --packages value. Examples: 46, all");
                return 1;
            }
            do_setup = 0;
        } else if (strcmp(a, "--reserve") == 0 && more) {
            size_t v = warp_parse_size(argv[++i]);
            if (v == 0) { warp_err("Bad --reserve value. Example: 2G"); return 1; }
            cfg.reserve_bytes = v;
            do_setup = 0;
        } else if (strcmp(a, "--monthly") == 0 && more) {
            cfg.monthly_limit_bytes = warp_parse_size(argv[++i]);
        } else if (strcmp(a, "--port") == 0 && more) {
            port = atoi(argv[++i]);
        } else {
            warp_err("Unknown option: %s", a);
            return 1;
        }
    }

    if (status) {
        stats_print_local(&cfg);
        char lim[32];
        if (cfg.monthly_limit_bytes) warp_fmt_size(cfg.monthly_limit_bytes, lim, sizeof(lim));
        else snprintf(lim, sizeof(lim), "unlimited");
        printf("  Monthly cap:    %s\n  Disk kept free: %.2f GB\n\n", lim,
               (double)(cfg.reserve_bytes ? cfg.reserve_bytes : WARP_RESERVE_DEFAULT) / 1073741824.0);
        return 0;
    }

    if (disable) {
        cfg.volunteer = 0;
        if (apply_config(&cfg) != 0) return 1;
        warp_ok("Volunteer mode off — the node keeps seeding what you installed");
        poke_running_node();
        return 0;
    }

    if (do_setup) {
        printf("\n  " WARP_BOLD "WARP Volunteer Setup" WARP_RESET "\n\n");
        printf("  A volunteer node also caches rarely seeded packages and shares them.\n");
        printf("  You decide how much disk it may use and how many packages it keeps.\n\n");

        char inp[64];

        cfg.quota_bytes = 0; cfg.unlimited = 0;
        for (;;) {
            printf("  Disk for the volunteer cache [10G, 10.46G, 500M or 'all']: ");
            fflush(stdout);
            if (!fgets(inp, sizeof(inp), stdin)) return 1;
            inp[strcspn(inp, "\r\n")] = '\0';
            if (parse_limit(inp, &cfg.quota_bytes, &cfg.unlimited) == 0) break;
            printf("  " WARP_RED "Invalid size." WARP_RESET " Try 10G, 10.46G, 500M or all.\n");
        }

        printf("  How many packages at most [number, Enter = no limit]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        inp[strcspn(inp, "\r\n")] = '\0';
        cfg.max_packages = 0;
        if (inp[0] && parse_count(inp, &cfg.max_packages) != 0) {
            warp_warn("Not a number — no package limit");
            cfg.max_packages = 0;
        }

        printf("  Cheap SD-card mode? (volunteer cache in RAM /tmp, saves flash wear) [y/N]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        cfg.cheap_sd = (inp[0] == 'y' || inp[0] == 'Y');

        printf("  Serve files to peers? (slow internet: answer n) [Y/n]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        cfg.serve = !(inp[0] == 'n' || inp[0] == 'N');

        printf("  Monthly upload limit [e.g. 50G, Enter = unlimited]: ");
        fflush(stdout);
        if (!fgets(inp, sizeof(inp), stdin)) return 1;
        inp[strcspn(inp, "\r\n")] = '\0';
        cfg.monthly_limit_bytes = (inp[0] && inp[0] != '0') ? warp_parse_size(inp) : 0;

        time_t now = time(NULL);
        strftime(cfg.month_tag, sizeof(cfg.month_tag), "%Y-%m", localtime(&now));
        cfg.volunteer = 1;
    }

    if (!cfg.unlimited && cfg.quota_bytes == 0) {
        warp_err("No size limit set. Use --quota 10G (or 10.46G, or all) or run --setup.");
        return 1;
    }
    cfg.volunteer = 1;

    if (!cfg.consent_asked) stats_ask_consent(&cfg);

    if (apply_config(&cfg) != 0) return 1;
    warp_ok("Config saved: %s", WARP_SEED_CONF);

    if (p2p_node_signal(0) == WARP_OK) {
        poke_running_node();
        return 0;
    }
    if (no_start) return 0;      /* config only: the seed service starts the node */

    printf("\n  " WARP_BOLD "Starting volunteer mode" WARP_RESET "\n");
    char q[32];
    if (cfg.unlimited) snprintf(q, sizeof(q), "unlimited");
    else               warp_fmt_size(cfg.quota_bytes, q, sizeof(q));
    printf("  Disk:     %s\n", q);
    if (cfg.max_packages) printf("  Packages: up to %zu\n", cfg.max_packages);
    else                  printf("  Packages: no limit\n");
    printf("  Serving:  %s\n", cfg.serve ? "yes" : "no (cache only)");
    printf("  Cheap SD: %s\n\n", cfg.cheap_sd ? "yes" : "no");

    if (cfg.cheap_sd) seed_config_load(&cfg);   /* applies the RAM cache path */
    return run_node(&cfg, port);
}

/* ── warp stats [--what|--consent|--no-stats|--reset-id|--offline] ─ */
int cmd_stats(int argc, char **argv) {
    warp_seed_config_t cfg;
    int have_cfg = (seed_config_load(&cfg) == WARP_OK);
    int offline = 0;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--what") == 0) {
            stats_print_disclosure(&cfg);
            return 0;
        } else if (strcmp(a, "--consent") == 0) {
            if (!isatty(STDIN_FILENO)) {
                stats_print_disclosure(&cfg);
                warp_err("The question needs a terminal. Run 'warp stats --consent' there.");
                return 1;
            }
            stats_ask_consent(&cfg);
            if (apply_config(&cfg) != 0) return 1;
            poke_running_node();
            return 0;
        } else if (strcmp(a, "--no-stats") == 0) {
            cfg.stats_consent = 0;
            cfg.consent_asked = 1;
            if (apply_config(&cfg) != 0) return 1;
            warp_ok("Statistics are off — nothing is sent to the tracker");
            poke_running_node();
            return 0;
        } else if (strcmp(a, "--reset-id") == 0) {
            cfg.node_id[0] = '\0';
            stats_ensure_node_id(&cfg);
            if (apply_config(&cfg) != 0) return 1;
            warp_ok("New anonymous node id: %s", cfg.node_id);
            poke_running_node();
            return 0;
        } else if (strcmp(a, "--offline") == 0) {
            offline = 1;
        } else {
            warp_err("Unknown option: %s", a);
            return 1;
        }
    }

    (void)have_cfg;
    stats_print_local(&cfg);
    if (!offline && stats_fetch_network() != WARP_OK)
        warp_warn("Tracker unreachable — showing the last data we have");
    stats_print_network();
    return 0;
}

/* ── warp verify <file> --pubkey <hex> [--sig <file>] ──────────── */
int cmd_verify(int argc, char **argv) {
    const char *file = NULL, *sigfile = NULL, *pubhex = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--pubkey") == 0 && i + 1 < argc) pubhex = argv[++i];
        else if (strcmp(argv[i], "--sig") == 0 && i + 1 < argc) sigfile = argv[++i];
        else if (!file) file = argv[i];
        else { file = NULL; break; }
    }
    if (!file || !pubhex) {
        warp_err("Usage: warp verify <file> --pubkey <hex64> [--sig <file>]   (default signature file: <file>.sig)");
        return 1;
    }
    uint8_t pub[32];
    size_t plen = 0;
    if (warp_hex_decode(pubhex, pub, sizeof(pub), &plen) != WARP_OK || plen != 32) {
        warp_err("--pubkey must be 64 hex characters (Ed25519 public key)");
        return 1;
    }
    char sig_path[1024];
    if (!sigfile) { snprintf(sig_path, sizeof(sig_path), "%s.sig", file); sigfile = sig_path; }

    size_t dlen = 0, slen = 0;
    char *data = read_file(file, &dlen);
    char *sig = read_file(sigfile, &slen);
    if (!data || !sig) {
        warp_err("Cannot read %s", !data ? file : sigfile);
        free(data); free(sig);
        return 1;
    }
    /* the signed bytes are exactly the file, including any NUL it may contain */
    uint8_t raw[64];
    size_t rawlen = 0;
    int rc = (warp_base64_decode(sig, raw, &rawlen) == WARP_OK && rawlen == 64)
                 ? warp_ed25519_verify((const uint8_t *)data, dlen, raw, pub) : WARP_ERR_SIG;
    free(data); free(sig);
    if (rc != WARP_OK) { warp_err("Signature does NOT match: %s", file); return 1; }
    warp_ok("Signature OK: %s", file);
    return 0;
}

/* ── warp pubkey <private-key-file> ───────────────────────────── */
int cmd_pubkey(int argc, char **argv) {
    if (argc != 1) { warp_err("Usage: warp pubkey <private-key-file>"); return 1; }
    uint8_t pub[32];
    if (warp_pubkey_of(argv[0], pub) != WARP_OK) { warp_err("Cannot read a 32-byte hex private key from %s", argv[0]); return 1; }
    for (int i = 0; i < 32; i++) printf("%02x", pub[i]);
    printf("\n");
    return 0;
}

/* ── warp sha256 <file>... ────────────────────────────────────── */
int cmd_sha256(int argc, char **argv) {
    if (argc < 1) { warp_err("Usage: warp sha256 <file>..."); return 1; }
    int bad = 0;
    for (int i = 0; i < argc; i++) {
        char hex[WARP_SHA256_HEX];
        if (warp_sha256_file(argv[i], hex) != WARP_OK) { warp_err("Cannot read %s", argv[i]); bad = 1; continue; }
        printf("%s  %s\n", hex, argv[i]);
    }
    return bad;
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
    snprintf(out_name, sizeof(out_name), "%s-%s-%s.warp", pkg_name, pkg_ver, warp_archive_tag());

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
