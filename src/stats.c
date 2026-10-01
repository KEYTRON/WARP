#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include "warp.h"

/* ══════════════════════════════════════════════════════════════
 *  Node statistics: what this node did, and what the network looks like.
 *
 *  Local counters never leave the machine. They are sent to the tracker only
 *  after the user has said yes (default: no) — see stats_ask_consent().
 * ══════════════════════════════════════════════════════════════ */

void stats_ensure_node_id(warp_seed_config_t *cfg) {
    if (strlen(cfg->node_id) == 32) return;
    unsigned char raw[16];
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(raw, 1, sizeof(raw), f) : 0;
    if (f) fclose(f);
    if (got != sizeof(raw)) {
        /* No entropy source: a weak id is still only an anonymous counter key. */
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)rand();
    }
    for (size_t i = 0; i < sizeof(raw); i++)
        snprintf(cfg->node_id + i * 2, 3, "%02x", raw[i]);
}

static size_t seedable_packages(void) {
    size_t n = volunteer_count();
    warp_installed_t *list = NULL;
    int count = 0;
    if (store_list(&list, &count) == WARP_OK) {
        n += (size_t)count;
        store_free_list(list, count);
    }
    return n;
}

static int build_report(const warp_seed_config_t *cfg, char *buf, size_t sz) {
    return snprintf(buf, sz,
        "{\"node_id\":\"%s\",\"version\":\"%s\",\"os\":\"%s\",\"arch\":\"%s\","
        "\"volunteer\":%s,\"uploaded_bytes\":%llu,\"served\":%llu,"
        "\"packages\":%zu}",
        cfg->node_id, WARP_VERSION, WARP_OS, WARP_ARCH,
        cfg->volunteer ? "true" : "false",
        cfg->uploaded_total, cfg->served_total, seedable_packages());
}

void stats_print_disclosure(const warp_seed_config_t *cfg) {
    warp_seed_config_t tmp = *cfg;
    stats_ensure_node_id(&tmp);
    char body[512];
    build_report(&tmp, body, sizeof(body));

    printf("\n  " WARP_BOLD "Anonymous statistics" WARP_RESET "\n\n");
    printf("  If you agree, this node sends the tracker " WARP_TRACKER_URL "/stats\n");
    printf("  a small report about every 5 minutes. Exactly this, nothing else:\n\n");
    printf("    %s\n\n", body);
    printf("  node_id          random number made on this machine; not tied to your\n"
           "                   name, hardware or account (new one: warp stats --reset-id)\n");
    printf("  os, arch         the operating system and CPU architecture warp runs on; they feed\n"
           "                   the public platform survey (percentages only, like a hardware survey)\n");
    printf("  uploaded_bytes   how much this node has sent to other peers in total\n");
    printf("  served           how many packages it has sent\n");
    printf("  packages         how many packages it can seed\n");
    printf("  volunteer        whether volunteer mode is on\n\n");
    printf("  Not sent in this report: file names, paths, package contents, who downloaded what.\n");
    printf("  Separately from this report, seeding itself (warp seed) tells the tracker\n"
           "  your address, port and the names of packages you seed — peers need that to\n"
           "  find you. That does not depend on this answer.\n\n");
    printf("  The tracker adds the numbers up for the network totals shown in 'warp stats'.\n\n");
}

int stats_ask_consent(warp_seed_config_t *cfg) {
    if (!isatty(STDIN_FILENO)) return 0;     /* services never ask */
    stats_print_disclosure(cfg);
    printf("  Send anonymous statistics? [y/N]: ");
    fflush(stdout);
    char inp[16] = "";
    if (!fgets(inp, sizeof(inp), stdin)) return 0;
    cfg->stats_consent = (inp[0] == 'y' || inp[0] == 'Y') ? 1 : 0;
    cfg->consent_asked = 1;
    if (cfg->stats_consent) stats_ensure_node_id(cfg);
    printf("\n  %s\n\n", cfg->stats_consent
        ? "Thank you. Change your mind any time: warp stats --no-stats"
        : "OK, nothing will be sent. Change your mind any time: warp stats --consent");
    return 1;
}

int stats_report(const warp_seed_config_t *cfg) {
    if (!cfg->stats_consent || strlen(cfg->node_id) != 32) return WARP_OK;

    char body[512];
    build_report(cfg, body, sizeof(body));

    CURL *curl = curl_easy_init();
    if (!curl) return WARP_ERR_NET;
    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL,        WARP_TRACKER_URL "/stats");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,  "warp/" WARP_VERSION);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,    10L);
    curl_easy_setopt(curl, CURLOPT_CAINFO,     "/etc/ssl/certs/ca-certificates.crt");
    FILE *devnull = fopen("/dev/null", "w");
    if (devnull) curl_easy_setopt(curl, CURLOPT_WRITEDATA, devnull);

    CURLcode res = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    if (devnull) fclose(devnull);
    return (res == CURLE_OK && code == 200) ? WARP_OK : WARP_ERR_NET;
}

int stats_fetch_network(void) {
    char *body = warp_download_str(WARP_TRACKER_URL "/status");
    if (!body) return WARP_ERR_NET;
    json_t *j = json_parse(body);
    if (!j) { free(body); return WARP_ERR_JSON; }
    json_free(j);

    FILE *f = fopen(WARP_NET_STATS_CACHE, "w");
    if (f) { fputs(body, f); fclose(f); }
    free(body);
    return WARP_OK;
}

static void fmt_age(time_t secs, char *buf, size_t sz) {
    if (secs < 90)         snprintf(buf, sz, "just now");
    else if (secs < 5400)  snprintf(buf, sz, "%ld min ago", (long)(secs / 60));
    else if (secs < 172800) snprintf(buf, sz, "%ld h ago",  (long)(secs / 3600));
    else                   snprintf(buf, sz, "%ld days ago", (long)(secs / 86400));
}

/* Reads the cached tracker answer; returns 0 when there is none. */
static int load_network(json_t **out, time_t *age) {
    size_t len;
    char *s = read_file(WARP_NET_STATS_CACHE, &len);
    if (!s) return 0;
    json_t *j = json_parse(s);
    free(s);
    if (!j) return 0;
    struct stat st;
    *age = (stat(WARP_NET_STATS_CACHE, &st) == 0) ? time(NULL) - st.st_mtime : 0;
    *out = j;
    return 1;
}

void stats_print_local(const warp_seed_config_t *cfg) {
    char up[32], month[32], used[32], quota[32];
    warp_fmt_size((size_t)cfg->uploaded_total, up, sizeof(up));
    warp_fmt_size(cfg->monthly_used_bytes, month, sizeof(month));
    warp_fmt_size(volunteer_used_bytes(), used, sizeof(used));
    if (cfg->unlimited) snprintf(quota, sizeof(quota), "unlimited");
    else                warp_fmt_size(cfg->quota_bytes, quota, sizeof(quota));

    printf("\n  " WARP_BOLD "This node" WARP_RESET "\n\n");
    printf("  State:          %s\n", p2p_node_signal(0) == WARP_OK
           ? WARP_GREEN "running" WARP_RESET : WARP_YELLOW "not running" WARP_RESET);
    printf("  Mode:           %s\n", cfg->volunteer ? "volunteer (seed + rarely seeded packages)" : "seed");
    printf("  Serving:        %s\n", cfg->serve ? "yes" : "no");
    printf("  Packages:       %zu seedable (%zu in the volunteer cache)\n", seedable_packages(), volunteer_count());
    printf("  Volunteer cache:%s %s of %s", " ", used, quota);
    if (cfg->max_packages) printf(", up to %zu packages", cfg->max_packages);
    printf("\n");
    printf("  Sent in total:  %s in %llu package transfers\n", up, cfg->served_total);
    printf("  Sent this month:%s %s\n", " ", month);
    printf("  Statistics to tracker: %s\n", cfg->stats_consent ? WARP_GREEN "on" WARP_RESET : "off");
}

void stats_print_network(void) {
    json_t *j = NULL;
    time_t age = 0;
    printf("\n  " WARP_BOLD "Network" WARP_RESET " (%s)\n\n", WARP_TRACKER_URL);
    if (!load_network(&j, &age)) {
        printf("  No data yet (tracker not reached).\n\n");
        return;
    }
    char tot[32], ag[32];
    warp_fmt_size((size_t)json_num(j, "total_bytes", 0), tot, sizeof(tot));
    fmt_age(age, ag, sizeof(ag));
    printf("  Nodes online:   %d  (of %d ever seen, %d volunteers)\n",
           (int)json_num(j, "active_peers", 0), (int)json_num(j, "total_peers", 0),
           (int)json_num(j, "volunteers", 0));
    printf("  Packages:       %d in the network\n", (int)json_num(j, "packages", 0));
    printf("  Data moved:     %s in %d transfers\n", tot, (int)json_num(j, "total_served", 0));
    printf("  Data and transfers come from %d nodes that agreed to report;\n"
           "  the numbers are as reported by those nodes, not verified.\n",
           (int)json_num(j, "reporting_nodes", 0));
    printf("  Updated:        %s\n\n", ag);
    json_free(j);
}

/* One line for `warp --help`: local files only, never the network. */
void stats_print_help_line(void) {
    warp_seed_config_t cfg;
    if (seed_config_load(&cfg) != WARP_OK) return;

    char up[32];
    warp_fmt_size((size_t)cfg.uploaded_total, up, sizeof(up));
    printf("  " WARP_BOLD "Node:" WARP_RESET " %s, %s, sent %s, %zu packages",
           p2p_node_signal(0) == WARP_OK ? "running" : "stopped",
           cfg.volunteer ? "volunteer" : "seed", up, seedable_packages());

    json_t *j = NULL;
    time_t age = 0;
    if (load_network(&j, &age)) {
        char tot[32];
        warp_fmt_size((size_t)json_num(j, "total_bytes", 0), tot, sizeof(tot));
        printf(" · network: %d nodes online, %s moved",
               (int)json_num(j, "active_peers", 0), tot);
        json_free(j);
    }
    printf("   (warp stats)\n\n");
}
