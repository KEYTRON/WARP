#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "warp.h"

const char *g_warp_mirrors[WARP_INDEX_MIRRORS] = {
    "https://github.com/KEYTRON/K1OS/raw/packages",
    "https://gitlab.com/KEYTRON/K1OS/-/raw/packages",
    "https://gitverse.ru/keytron46/K1OS/raw/branch/packages"
};

static void print_banner(void) {
    printf(
        "\n"
        WARP_BOLD "  ██╗    ██╗ █████╗ ██████╗ ██████╗ \n"
        "  ██║    ██║██╔══██╗██╔══██╗██╔══██╗\n"
        "  ██║ █╗ ██║███████║██████╔╝██████╔╝\n"
        "  ██║███╗██║██╔══██║██╔══██╗██╔═══╝ \n"
        "  ╚███╔███╔╝██║  ██║██║  ██║██║     \n"
        "   ╚══╝╚══╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝     " WARP_RESET
        WARP_CYAN " v" WARP_VERSION WARP_RESET "\n"
        "  K1OS Package Manager\n\n"
    );
}

static void print_help(void) {
    print_banner();
    printf(
        "  " WARP_BOLD "Usage:" WARP_RESET " warp <command> [args]\n\n"
        "  " WARP_BOLD "Commands:" WARP_RESET "\n"
        "    install    <pkg>[@ver] Install a package (or one specific version)\n"
        "    upgrade    [pkg...]    Upgrade installed packages (delta when available)\n"
        "    remove     <pkg>       Remove a package\n"
        "    list                   List installed packages\n"
        "    search     <query>     Search available packages\n"
        "    rollback   <pkg>       Go one version back (repeat to go further)\n"
        "    versions   <pkg>       Installed and published versions\n"
        "    switch     <pkg> <ver> Use any installed version (no download)\n"
        "    pin|unpin  <pkg> [ver] Keep a version: 'upgrade' leaves it alone\n"
        "    run        <pkg>[@ver] [args]  Run a version without switching\n"
        "    gc         [--keep N] [--dry-run]  Remove versions nobody uses\n"
        "    info       <pkg>       Show package details\n"
        "    update                 Refresh package indexes\n"
        "    repo       list|add|remove|enable|disable   Manage repositories\n"
        "      add <name> <url> --pubkey <hex> [--mirror <url>]...\n"
        "    keygen     [priv pub]  Generate Ed25519 signing keypair\n"
        "    sign       <file>      Sign a file, writing <file>.sig\n"
        "    verify     <file> --pubkey <hex>   Check <file>.sig (Ed25519)\n"
        "    pubkey     <private-key-file>      Print the matching public key\n"
        "    sha256     <file>...   Print SHA-256 sums (like sha256sum)\n"
        "    pack       <dir>       Create .warp from a directory\n"
        "    delta      <old> <new> <out>   Build a delta between two archives\n"
        "    seed                   Seed installed packages to peers (one node process)\n"
        "    volunteer              Volunteer mode: seed + cache rarely seeded packages\n"
        "      --setup              Re-run interactive setup\n"
        "      --enable|--disable   Turn volunteer mode on or off (seeding keeps running)\n"
        "      --quota <N|all>      Disk for the cache (10G, 10.46G, 500M, all)\n"
        "      --packages <N|all>   Max packages in the cache (46, all)\n"
        "      --reserve <N>        Disk space to keep free (default 1G)\n"
        "      --no-serve           Cache only, don't serve files\n"
        "      --monthly <N>        Monthly upload cap (e.g. 50G)\n"
        "      --status             Show this node's state\n"
        "      --no-start           Only save the settings, don't run the node\n"
        "    stats                  This node and the network: sent, packages, nodes, traffic\n"
        "      --what               Show exactly what anonymous statistics contain\n"
        "      --consent|--no-stats Agree to / stop sending anonymous statistics (default: off)\n"
        "      --reset-id           New random anonymous node id\n\n"
        "  " WARP_BOLD "Examples:" WARP_RESET "\n"
        "    warp search editor\n"
        "    warp install nano\n"
        "    warp upgrade\n"
        "    warp repo add lab https://mirror.example/lab --pubkey <hex>\n\n"
        "  Repositories: " WARP_REPOS_CONF " (default: k1os via GitHub, GitLab, GitVerse)\n"
        "  Store: " WARP_STORE_DIR "\n\n"
    );
    stats_print_help_line();
}

typedef struct {
    const char *name;
    int (*fn)(int argc, char **argv);
} cmd_t;

static const cmd_t commands[] = {
    { "install",  cmd_install  },
    { "upgrade",  cmd_upgrade  },
    { "repo",     cmd_repo     },
    { "delta",    cmd_delta    },
    { "delta-apply", cmd_delta_apply },
    { "remove",   cmd_remove   },
    { "rm",       cmd_remove   },
    { "list",     cmd_list     },
    { "ls",       cmd_list     },
    { "search",   cmd_search   },
    { "rollback", cmd_rollback },
    { "info",     cmd_info     },
    { "update",   cmd_update   },
    { "keygen",   cmd_keygen   },
    { "sign",     cmd_sign     },
    { "verify",   cmd_verify   },
    { "pubkey",   cmd_pubkey   },
    { "sha256",   cmd_sha256   },
    { "pack",      cmd_pack      },
    { "seed",      cmd_seed      },
    { "volunteer", cmd_volunteer },
    { "stats",     cmd_stats     },
    { "versions",  cmd_versions  },
    { "switch",    cmd_switch    },
    { "pin",       cmd_pin       },
    { "unpin",     cmd_unpin     },
    { "run",       cmd_run       },
    { "gc",        cmd_gc        },
    { NULL, NULL }
};

int main(int argc, char **argv) {
    /* Keep stdout and stderr in order when output goes to a file or a CI log. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 2) { print_help(); return 0; }

    if (strcmp(argv[1], "archive-tag") == 0) {      /* for build scripts */
        printf("%s\n", warp_archive_tag());
        return 0;
    }
    if (strcmp(argv[1], "platform") == 0) {         /* for build scripts and bug reports */
        if (argc > 2 && strcmp(argv[2], "--all") == 0) {    /* every platform whose builds this machine takes */
            const char *c[4];
            int n = warp_platform_candidates(c, 4);
            for (int i = 0; i < n; i++) printf("%s\n", c[i]);
            return 0;
        }
        printf("%s\n", warp_platform());
        return 0;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0) {
        printf("warp %s\n", WARP_VERSION);
        return 0;
    }
    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_help(); return 0;
    }

    /* Init curl globally */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char *cmd_name = argv[1];
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(commands[i].name, cmd_name) == 0) {
            int rc = commands[i].fn(argc - 2, argv + 2);
            curl_global_cleanup();
            return rc;
        }
    }

    warp_err("Unknown command: %s", cmd_name);
    printf("  Run 'warp --help' for usage.\n\n");
    curl_global_cleanup();
    return 1;
}
