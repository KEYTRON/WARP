#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <ctype.h>
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


/* ══════════════════════════════════════════════════════════════
 *  What the survey asks about this machine. Everything is coarse on purpose:
 *  the kernel as major.minor, memory rounded to a standard size, no host
 *  name, no serial numbers, no addresses. The user sees the exact values in
 *  `warp stats --what` before agreeing.
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    char kernel[16];          /* "6.18" */
    char distro[32];          /* /etc/os-release ID */
    char distro_version[24];  /* VERSION_ID, empty on rolling distributions */
    char cpu[72];             /* processor model, tidied */
    int  cores;               /* logical cores */
    int  ram_gb;              /* installed memory, standard size */
} warp_sysinfo_t;

/* Printable ASCII without the characters that would break the JSON. */
static void clean_text(char *s) {
    char *o = s;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 32 || c > 126) continue;
        *o++ = (c == '"' || c == '\\') ? ' ' : (char)c;
    }
    *o = '\0';
}

#if defined(__linux__)
/* /proc files report size 0, so they must be read until EOF (read_file trusts the size). */
static char *read_stream(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        size_t n = fread(buf + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0 || len + 1 < cap) break;
        cap *= 2;
        buf = realloc(buf, cap);
    }
    fclose(f);
    if (buf) buf[len] = '\0';
    return buf;
}

static int first_line_value(const char *path, const char *key, char *out, size_t sz) {
    char *s = read_stream(path);
    if (!s) return 0;
    size_t kl = strlen(key);
    int found = 0;
    for (char *line = s; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, key, kl) == 0) {
            const char *v = line + kl;
            while (*v == ' ' || *v == '\t' || *v == ':' || *v == '=') v++;
            snprintf(out, sz, "%s", v);
            found = 1;
            break;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(s);
    return found;
}

static void os_release_value(const char *key, char *out, size_t sz) {
    char buf[96] = "";
    if (!first_line_value("/etc/os-release", key, buf, sizeof(buf)))
        first_line_value("/usr/lib/os-release", key, buf, sizeof(buf));
    char *v = buf;
    if (*v == '"' || *v == '\'') { v++; v[strcspn(v, "\"'")] = '\0'; }
    snprintf(out, sz, "%s", v);
}

/* Intel(R) Core(TM) i5-3470 CPU @ 3.20GHz -> Intel Core i5-3470 */
static void tidy_cpu(char *cpu) {
    static const char *drop[] = { "(R)", "(TM)", "(tm)", "(r)", " CPU", "Processor", NULL };
    for (int i = 0; drop[i]; i++) {
        char *p;
        while ((p = strstr(cpu, drop[i])) != NULL) memmove(p, p + strlen(drop[i]), strlen(p + strlen(drop[i])) + 1);
    }
    char *at = strstr(cpu, " @");
    if (at) *at = '\0';
    /* collapse runs of blanks and trim */
    char *o = cpu; int blank = 1;
    for (char *p = cpu; *p; p++) {
        if (*p == ' ' || *p == '\t') { if (!blank) *o++ = ' '; blank = 1; }
        else { *o++ = *p; blank = 0; }
    }
    if (o > cpu && o[-1] == ' ') o--;
    *o = '\0';
}

static const char *arm_vendor(unsigned imp) {
    switch (imp) {
        case 0x41: return "ARM";      case 0x42: return "Broadcom";  case 0x43: return "Cavium";
        case 0x4e: return "NVIDIA";   case 0x51: return "Qualcomm";  case 0x61: return "Apple";
        case 0xc0: return "Ampere";   default: return "ARM";
    }
}
#endif

/* Memory, rounded to the standard size it was sold as: a 16 GB machine reports ~15.5 GiB. */
static int ram_bucket(double gib) {
    static const int sizes[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 1024 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
        if ((double)sizes[i] >= gib * 0.93) return sizes[i];
    return 1024;
}

static void sysinfo_collect(warp_sysinfo_t *si) {
    memset(si, 0, sizeof(*si));
#if defined(__linux__)
    char buf[160];

    {
        char *k = read_stream("/proc/sys/kernel/osrelease");
        int maj = 0, min = 0;
        if (k && sscanf(k, "%d.%d", &maj, &min) == 2 && maj > 0) snprintf(si->kernel, sizeof(si->kernel), "%d.%d", maj, min);
        free(k);
    }

    os_release_value("ID", si->distro, sizeof(si->distro));
    os_release_value("VERSION_ID", si->distro_version, sizeof(si->distro_version));
    for (char *p = si->distro; *p; p++) *p = (char)tolower((unsigned char)*p);
#if defined(__ANDROID__)
    if (!si->distro[0]) snprintf(si->distro, sizeof(si->distro), "termux");
#endif

    if (!first_line_value("/proc/cpuinfo", "model name", si->cpu, sizeof(si->cpu)) &&
        !first_line_value("/proc/cpuinfo", "Hardware", si->cpu, sizeof(si->cpu))) {
        char imp[24] = "", part[24] = "";
        if (first_line_value("/proc/cpuinfo", "CPU implementer", imp, sizeof(imp))) {
            first_line_value("/proc/cpuinfo", "CPU part", part, sizeof(part));
            snprintf(si->cpu, sizeof(si->cpu), "%s part %s", arm_vendor((unsigned)strtoul(imp, NULL, 0)), part[0] ? part : "?");
        }
    }
    clean_text(si->cpu);
    tidy_cpu(si->cpu);

    long mem_kb = 0;
    if (first_line_value("/proc/meminfo", "MemTotal", buf, sizeof(buf))) mem_kb = atol(buf);
    if (mem_kb > 0) si->ram_gb = ram_bucket((double)mem_kb / 1048576.0);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->cores = n > 0 ? (int)n : 0;
#endif
    clean_text(si->distro);
    clean_text(si->distro_version);
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
    warp_sysinfo_t si;
    sysinfo_collect(&si);
    return snprintf(buf, sz,
        "{\"node_id\":\"%s\",\"version\":\"%s\",\"os\":\"%s\",\"arch\":\"%s\","
        "\"kernel\":\"%s\",\"distro\":\"%s\",\"distro_version\":\"%s\","
        "\"cpu\":\"%s\",\"cores\":%d,\"ram_gb\":%d,"
        "\"volunteer\":%s,\"uploaded_bytes\":%llu,\"served\":%llu,"
        "\"packages\":%zu}",
        cfg->node_id, WARP_VERSION, WARP_OS, WARP_ARCH,
        si.kernel, si.distro, si.distro_version, si.cpu, si.cores, si.ram_gb,
        cfg->volunteer ? "true" : "false",
        cfg->uploaded_total, cfg->served_total, seedable_packages());
}

void stats_print_disclosure(const warp_seed_config_t *cfg) {
    warp_seed_config_t tmp = *cfg;
    stats_ensure_node_id(&tmp);
    char body[1024];
    build_report(&tmp, body, sizeof(body));

    printf("\n  " WARP_BOLD "Anonymous statistics" WARP_RESET "\n\n");
    printf("  If you agree, this node sends the tracker " WARP_TRACKER_URL "/stats\n");
    printf("  a small report about every 5 minutes. Exactly this, nothing else:\n\n");
    printf("    %s\n\n", body);
    printf("  node_id          random number made on this machine; not tied to your\n"
           "                   name, hardware or account (new one: warp stats --reset-id)\n");
    printf("  os, arch         the operating system and CPU architecture warp runs on; they feed\n"
           "                   the public platform survey (percentages only, like a hardware survey)\n");
    printf("  kernel           the kernel's major.minor version (for example 6.18), not the full string\n");
    printf("  distro           distribution id and version from /etc/os-release (for example alpine 3.20)\n");
    printf("  cpu, cores       the processor model as the system reports it, and its logical core count\n");
    printf("  ram_gb           installed memory rounded to a standard size (for example 16), not the exact amount\n");
    printf("  uploaded_bytes   how much this node has sent to other peers in total\n");
    printf("  served           how many packages it has sent\n");
    printf("  packages         how many packages it can seed\n");
    printf("  volunteer        whether volunteer mode is on\n\n");
    printf("  Not sent in this report: host name, user name, disks, network addresses, serial numbers,\n"
           "  file names, paths, package contents, who downloaded what.\n");
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
    cfg->consent_schema = cfg->stats_consent ? WARP_REPORT_SCHEMA : 0;
    if (cfg->stats_consent) stats_ensure_node_id(cfg);
    printf("\n  %s\n\n", cfg->stats_consent
        ? "Thank you. Change your mind any time: warp stats --no-stats"
        : "OK, nothing will be sent. Change your mind any time: warp stats --consent");
    return 1;
}

int stats_report(const warp_seed_config_t *cfg) {
    if (!cfg->stats_consent || strlen(cfg->node_id) != 32) return WARP_OK;
    /* The report has grown since the user agreed: send nothing until they have seen the new one. */
    if (cfg->consent_schema < WARP_REPORT_SCHEMA) {
        static int warned;
        if (!warned) {
            warned = 1;
            warp_warn("The statistics report now has more fields; nothing is sent until you review it: warp stats --consent");
        }
        return WARP_OK;
    }

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
    printf("  Statistics to tracker: %s\n", !cfg->stats_consent ? "off"
           : cfg->consent_schema < WARP_REPORT_SCHEMA ? WARP_YELLOW "paused: the report grew, review it with 'warp stats --consent'" WARP_RESET
           : WARP_GREEN "on" WARP_RESET);
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
