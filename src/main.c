#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "warp.h"

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
        "  Standalone Package Manager\n\n"
    );
}

static void print_help(void) {
    print_banner();
    printf(
        "  " WARP_BOLD "Usage:" WARP_RESET " warp <command> [args]\n\n"
        "  " WARP_BOLD "Commands:" WARP_RESET "\n"
        "    install  <pkg>    Install a package\n"
        "    remove   <pkg>    Remove a package\n"
        "    list              List installed packages\n"
        "    search   <query>  Search available packages\n"
        "    rollback <pkg>    Revert to previous version\n"
        "    info     <pkg>    Show package details\n"
        "    update            Refresh package index\n"
        "    keygen            Generate Ed25519 signing keypair\n"
        "    pack     <dir>    Create .warp from a directory\n\n"
        "  " WARP_BOLD "Examples:" WARP_RESET "\n"
        "    warp search editor\n"
        "    warp install nano\n"
        "    warp rollback nano\n\n"
        "  Index: " WARP_INDEX_URL "\n"
        "  Store: " WARP_STORE_DIR "\n\n"
    );
}

typedef struct {
    const char *name;
    int (*fn)(int argc, char **argv);
} cmd_t;

static const cmd_t commands[] = {
    { "install",  cmd_install  },
    { "remove",   cmd_remove   },
    { "rm",       cmd_remove   },
    { "list",     cmd_list     },
    { "ls",       cmd_list     },
    { "search",   cmd_search   },
    { "rollback", cmd_rollback },
    { "info",     cmd_info     },
    { "update",   cmd_update   },
    { "keygen",   cmd_keygen   },
    { "pack",     cmd_pack     },
    { NULL, NULL }
};

int main(int argc, char **argv) {
    if (argc < 2) { print_help(); return 0; }

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
