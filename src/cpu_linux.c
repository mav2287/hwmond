/*
 * cpu_linux.c — Linux per-package CPU usage monitoring via /proc/stat
 *
 * Reads /proc/stat for per-CPU kernel counters, computes delta-based
 * utilization, and aggregates per physical package using /proc/cpuinfo
 * topology. Implements the same cpu_usage.h interface as cpu_esxi.c.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "cpu_usage.h"

/* Per-CPU previous counters for delta computation */
static uint64_t prev_used[MAX_PCPUS];
static uint64_t prev_total[MAX_PCPUS];
static int pcpu_to_package[MAX_PCPUS];
static int num_pcpus = 0;
static int num_packages = 0;
static uint64_t boot_time_sec = 0;

/* ------------------------------------------------------------------ */
/*  Topology detection via /proc/cpuinfo                               */
/* ------------------------------------------------------------------ */

static int detect_topology(cpu_state_t *state)
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) {
        fprintf(stderr, "cpu: cannot open /proc/cpuinfo\n");
        return -1;
    }

    char line[256];
    int current_cpu = -1;
    int max_pkg = -1;
    int cores_per_pkg = 0;
    memset(pcpu_to_package, 0, sizeof(pcpu_to_package));

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "processor", 9) == 0) {
            char *p = strchr(line, ':');
            if (p) current_cpu = atoi(p + 1);
            if (current_cpu >= MAX_PCPUS) current_cpu = -1;
            num_pcpus = current_cpu + 1;
        }
        else if (strncmp(line, "physical id", 11) == 0 && current_cpu >= 0) {
            char *p = strchr(line, ':');
            if (p) {
                int pkg = atoi(p + 1);
                pcpu_to_package[current_cpu] = pkg;
                if (pkg > max_pkg) max_pkg = pkg;
            }
        }
        else if (strncmp(line, "cpu cores", 9) == 0 && cores_per_pkg == 0) {
            char *p = strchr(line, ':');
            if (p) cores_per_pkg = atoi(p + 1);
        }
    }
    fclose(fp);

    num_packages = max_pkg + 1;
    if (num_packages <= 0) num_packages = 1;
    if (num_pcpus <= 0) num_pcpus = 1;

    state->num_packages = num_packages;
    state->num_threads = num_pcpus;

    /* Map PCPUs to packages */
    for (int i = 0; i < num_pcpus && i < MAX_PCPUS; i++)
        state->pcpu_to_package[i] = pcpu_to_package[i];

    fprintf(stderr, "cpu: /proc/cpuinfo: %d PCPUs, %d packages, %d cores/pkg\n",
            num_pcpus, num_packages, cores_per_pkg);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  CPU sampling via /proc/stat                                        */
/* ------------------------------------------------------------------ */

/*
 * /proc/stat format:
 *   cpu  user nice system idle iowait irq softirq steal guest guest_nice
 *   cpu0 user nice system idle iowait irq softirq steal guest guest_nice
 *   ...
 *
 * used = user + nice + system + irq + softirq
 * total = used + idle + iowait
 */
static int read_proc_stat(cpu_state_t *state)
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return -1;

    char line[512];
    int cpu_idx;

    /* Reset per-package accumulators */
    float pkg_used[MAX_PACKAGES] = {0};
    int pkg_count[MAX_PACKAGES] = {0};

    while (fgets(line, sizeof(line), fp)) {
        /* Skip the aggregate "cpu " line (no number) */
        if (strncmp(line, "cpu", 3) != 0 || line[3] == ' ') {
            /* Check for btime (boot time) */
            if (strncmp(line, "btime ", 6) == 0)
                boot_time_sec = strtoull(line + 6, NULL, 10);
            continue;
        }

        /* Parse "cpuN ..." */
        uint64_t user, nice, system, idle, iowait, irq, softirq;
        if (sscanf(line, "cpu%d %lu %lu %lu %lu %lu %lu %lu",
                   &cpu_idx, &user, &nice, &system, &idle,
                   &iowait, &irq, &softirq) < 8)
            continue;

        if (cpu_idx < 0 || cpu_idx >= MAX_PCPUS || cpu_idx >= num_pcpus)
            continue;

        uint64_t used = user + nice + system + irq + softirq;
        uint64_t total = used + idle + iowait;

        /* Delta-based utilization */
        float usage = 0.0f;
        if (prev_total[cpu_idx] > 0) {
            uint64_t d_used = used - prev_used[cpu_idx];
            uint64_t d_total = total - prev_total[cpu_idx];
            if (d_total > 0)
                usage = (float)d_used / (float)d_total;
        }

        prev_used[cpu_idx] = used;
        prev_total[cpu_idx] = total;

        state->pcpu_usage[cpu_idx] = usage;

        /* Aggregate per package */
        int pkg = pcpu_to_package[cpu_idx];
        if (pkg >= 0 && pkg < MAX_PACKAGES) {
            pkg_used[pkg] += usage;
            pkg_count[pkg]++;
        }
    }
    fclose(fp);

    /* Compute per-package average */
    for (int i = 0; i < num_packages && i < MAX_PACKAGES; i++) {
        if (pkg_count[i] > 0)
            state->package_usage[i] = pkg_used[i] / (float)pkg_count[i];
        else
            state->package_usage[i] = 0.0f;
    }

    /* Uptime from /proc/uptime */
    fp = fopen("/proc/uptime", "r");
    if (fp) {
        double up = 0;
        if (fscanf(fp, "%lf", &up) == 1)
            state->uptime_usec = (uint64_t)(up * 1000000.0);
        fclose(fp);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API (matches cpu_usage.h)                                   */
/* ------------------------------------------------------------------ */

int cpu_init(cpu_state_t *state)
{
    memset(state, 0, sizeof(*state));
    memset(prev_used, 0, sizeof(prev_used));
    memset(prev_total, 0, sizeof(prev_total));

    fprintf(stderr, "cpu: detecting topology...\n");

    if (detect_topology(state) < 0)
        return -1;

    fprintf(stderr, "cpu: collecting initial sample...\n");
    read_proc_stat(state);  /* First sample (no deltas yet) */

    fprintf(stderr, "cpu: monitoring active (/proc/stat, %d PCPUs)\n",
            num_pcpus);
    return 0;
}

int cpu_sample(cpu_state_t *state)
{
    sleep(1);
    return read_proc_stat(state);
}

void cpu_shutdown(cpu_state_t *state)
{
    (void)state;
    /* Nothing to clean up on Linux — /proc/stat doesn't need closing */
}
