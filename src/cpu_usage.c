/*
 * cpu_usage.c - ESXi per-package CPU usage monitoring via vsish (popen)
 *
 * Directly pipes vsish commands via popen() to read per-PCPU kernel
 * counters. No companion script needed.
 *
 *   printf 'cat /sched/pcpus/0/stats\n...\n' | /bin/vsish 2>/dev/null
 *
 * CPU utilization is computed as:
 *   usage = delta(used-time) / delta(elapsed-time)
 *
 * This matches how Apple's hwmond computed CPU usage via Mach's
 * host_processor_info() -- delta of busy ticks / total ticks.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include "cpu_usage.h"

/* Previous sample's counters for delta computation */
static uint64_t prev_used[MAX_PCPUS];
static uint64_t prev_elapsed[MAX_PCPUS];
static int      have_prev = 0;

/* ------------------------------------------------------------------ */
/*  PCPU count detection                                               */
/* ------------------------------------------------------------------ */

/*
 * Detect the number of PCPUs by listing vsish nodes.
 *
 * Command: vsish -e ls /sched/pcpus/ 2>/dev/null
 * Output format:
 *   0/
 *   1/
 *   ...
 *   15/
 *
 * Returns the count (>0) on success, -1 on failure.
 */
static int detect_pcpu_count(void)
{
    FILE *fp = popen("/bin/vsish -e ls /sched/pcpus/ 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "cpu: popen(vsish ls pcpus) failed: %s\n",
                strerror(errno));
        return -1;
    }

    char line[256];
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* Each line is like "0/" or "15/" -- just count them */
        int val;
        if (sscanf(line, "%d/", &val) == 1) {
            count++;
        }
    }

    pclose(fp);

    if (count <= 0) {
        fprintf(stderr, "cpu: vsish returned no PCPUs\n");
        return -1;
    }

    return count;
}

/* ------------------------------------------------------------------ */
/*  Topology detection via popen                                       */
/* ------------------------------------------------------------------ */

/*
 * Detect CPU topology by running esxcli commands via popen().
 *
 * Step 1: /bin/esxcli hardware cpu global get
 *   Output format:
 *     CPU Packages: 2
 *     CPU Cores: 8
 *     CPU Threads: 16
 *
 * Step 2: /bin/esxcli hardware cpu list
 *   Output format (per CPU):
 *     CPU: 0
 *        ...
 *        Package Id: 0
 *        ...
 *     CPU: 1
 *        ...
 *        Package Id: 0
 *
 * Returns 0 on success, -1 on failure.
 */
static int detect_topology_popen(cpu_state_t *state)
{
    FILE *fp;
    char line[512];
    int found_global = 0;

    memset(state->pcpu_to_package, 0, sizeof(state->pcpu_to_package));

    /* Step 1: global CPU info */
    fp = popen("/bin/esxcli hardware cpu global get 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "cpu: popen(esxcli global) failed: %s\n",
                strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        int val;
        if (strstr(line, "CPU Packages:")) {
            if (sscanf(line, " CPU Packages: %d", &val) == 1) {
                state->num_packages = val;
                found_global++;
            }
        } else if (strstr(line, "CPU Cores:")) {
            if (sscanf(line, " CPU Cores: %d", &val) == 1) {
                state->num_cores = val;
                found_global++;
            }
        } else if (strstr(line, "CPU Threads:")) {
            if (sscanf(line, " CPU Threads: %d", &val) == 1) {
                state->num_threads = val;
                found_global++;
            }
        }
    }

    pclose(fp);

    if (found_global < 3 || state->num_threads <= 0) {
        fprintf(stderr, "cpu: esxcli global get incomplete "
                "(found %d/3 fields)\n", found_global);
        return -1;
    }

    if (state->num_packages > MAX_PACKAGES)
        state->num_packages = MAX_PACKAGES;
    if (state->num_threads > MAX_PCPUS)
        state->num_threads = MAX_PCPUS;

    /* Step 2: per-CPU package mapping */
    fp = popen("/bin/esxcli hardware cpu list 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "cpu: popen(esxcli cpu list) failed: %s\n",
                strerror(errno));
        /* Fall through to even-split fallback below */
        goto fallback_mapping;
    }

    int current_cpu = -1;
    int mapped = 0;

    while (fgets(line, sizeof(line), fp)) {
        int val;
        if (sscanf(line, " CPU: %d", &val) == 1) {
            current_cpu = val;
        } else if (sscanf(line, " Package Id: %d", &val) == 1 ||
                   sscanf(line, " Package: %d", &val) == 1) {
            if (current_cpu >= 0 && current_cpu < MAX_PCPUS) {
                int pkg = val;
                if (pkg >= MAX_PACKAGES) pkg = MAX_PACKAGES - 1;
                state->pcpu_to_package[current_cpu] = pkg;
                mapped++;
            }
        }
    }

    pclose(fp);

    if (mapped > 0)
        return 0;

fallback_mapping:
    /* Even-split: divide threads evenly across packages */
    {
        int per_pkg = state->num_threads / state->num_packages;
        if (per_pkg <= 0) per_pkg = 1;
        for (int i = 0; i < state->num_threads; i++) {
            int pkg = i / per_pkg;
            if (pkg >= state->num_packages)
                pkg = state->num_packages - 1;
            state->pcpu_to_package[i] = pkg;
        }
    }

    return 0;
}

/*
 * Fallback topology: try /proc/cpuinfo (unlikely on ESXi, but safe).
 */
static int detect_topology_cpuinfo(cpu_state_t *state)
{
    FILE *fp = fopen("/proc/cpuinfo", "r");
    if (!fp) return -1;

    char line[256];
    int max_proc = -1;

    while (fgets(line, sizeof(line), fp)) {
        int val;
        if (sscanf(line, "processor : %d", &val) == 1) {
            if (val > max_proc) max_proc = val;
        }
    }

    fclose(fp);
    if (max_proc < 0) return -1;

    state->num_threads = max_proc + 1;
    if (state->num_packages <= 0) state->num_packages = 2;
    if (state->num_cores <= 0) state->num_cores = state->num_threads / 2;

    if (state->num_packages > MAX_PACKAGES)
        state->num_packages = MAX_PACKAGES;
    if (state->num_threads > MAX_PCPUS)
        state->num_threads = MAX_PCPUS;

    int per_pkg = state->num_threads / state->num_packages;
    if (per_pkg <= 0) per_pkg = 1;
    for (int i = 0; i < state->num_threads; i++) {
        int pkg = i / per_pkg;
        if (pkg >= state->num_packages) pkg = state->num_packages - 1;
        state->pcpu_to_package[i] = pkg;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  vsish data sampling via popen                                      */
/* ------------------------------------------------------------------ */

/*
 * Build the popen command string for sampling all PCPUs.
 *
 * Produces:
 *   printf 'cat /sched/pcpus/0/stats\ncat /sched/pcpus/1/stats\n...\n'
 *     | /bin/vsish 2>/dev/null
 *
 * The command buffer must be at least CMD_BUF_SIZE bytes.
 */
#define CMD_BUF_SIZE 4096

static int build_vsish_command(char *cmd, size_t cmd_sz, int num_pcpus)
{
    int offset = 0;
    int ret;

    ret = snprintf(cmd + offset, cmd_sz - (size_t)offset, "printf '");
    if (ret < 0) return -1;
    offset += ret;

    for (int i = 0; i < num_pcpus; i++) {
        ret = snprintf(cmd + offset, cmd_sz - (size_t)offset,
                       "cat /sched/pcpus/%d/stats\\n", i);
        if (ret < 0 || (size_t)(offset + ret) >= cmd_sz) return -1;
        offset += ret;
    }

    ret = snprintf(cmd + offset, cmd_sz - (size_t)offset,
                   "' | /bin/vsish 2>/dev/null");
    if (ret < 0 || (size_t)(offset + ret) >= cmd_sz) return -1;

    return 0;
}

/*
 * Run a single vsish sample via popen, parsing used-time and elapsed-time
 * for each PCPU.
 *
 * vsish interactive output format:
 *
 *   /> cat /sched/pcpus/0/stats
 *   pcpu-info {
 *      used-time:289934179359 usec
 *      elapsed-time:11445778387688 usec
 *      ...
 *   }
 *   /> cat /sched/pcpus/1/stats
 *   ...
 *
 * Some PCPUs may NOT have the "/> cat" prefix echoed. We track by order:
 * the first block is PCPU 0, second is PCPU 1, etc. A new block starts
 * when we see "/> cat /sched/pcpus/" OR when we see "pcpu-info {" after
 * having already collected data for the current PCPU.
 *
 * Returns 0 on success (with delta computed), 1 if first sample (no delta
 * yet), -1 on error.
 */
static int sample_vsish(cpu_state_t *state)
{
    char cmd[CMD_BUF_SIZE];

    if (build_vsish_command(cmd, sizeof(cmd), state->num_threads) != 0) {
        fprintf(stderr, "cpu: command buffer too small for %d PCPUs\n",
                state->num_threads);
        return -1;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "cpu: popen(vsish) failed: %s\n", strerror(errno));
        return -1;
    }

    char line[512];
    uint64_t cur_used[MAX_PCPUS] = {0};
    uint64_t cur_elapsed[MAX_PCPUS] = {0};
    int current_pcpu = -1;
    int max_pcpu = -1;
    int got_used = 0;
    int got_elapsed = 0;

    while (fgets(line, sizeof(line), fp)) {
        int pcpu_id;
        uint64_t val;

        /* Explicit PCPU marker: "/> cat /sched/pcpus/N/stats" */
        if (sscanf(line, "/> cat /sched/pcpus/%d/", &pcpu_id) == 1) {
            current_pcpu = pcpu_id;
            if (pcpu_id > max_pcpu) max_pcpu = pcpu_id;
            got_used = 0;
            got_elapsed = 0;
            continue;
        }

        /*
         * Block header: "pcpu-info {" starts a new PCPU block.
         */
        if (strstr(line, "pcpu-info {") || strstr(line, "pcpu-info{")) {
            if (current_pcpu < 0) {
                current_pcpu = 0;
            } else if (got_used && got_elapsed) {
                current_pcpu++;
            } else if (current_pcpu < 0) {
                current_pcpu = 0;
            }
            if (current_pcpu > max_pcpu) max_pcpu = current_pcpu;
            got_used = 0;
            got_elapsed = 0;
            continue;
        }

        /* Parse data lines within a PCPU block */
        if (current_pcpu >= 0 && current_pcpu < MAX_PCPUS) {
            char *p;
            if (!got_used && (p = strstr(line, "used-time:")) != NULL) {
                if (sscanf(p, "used-time:%lu", &val) == 1) {
                    cur_used[current_pcpu] = val;
                    got_used = 1;
                }
            } else if (!got_elapsed &&
                       (p = strstr(line, "elapsed-time:")) != NULL) {
                if (sscanf(p, "elapsed-time:%lu", &val) == 1) {
                    cur_elapsed[current_pcpu] = val;
                    got_elapsed = 1;
                }
            }
        }
    }

    pclose(fp);

    if (max_pcpu < 0) {
        fprintf(stderr, "cpu: vsish returned no PCPU data\n");
        return -1;
    }

    /* Update thread count if we discovered more PCPUs */
    if (max_pcpu + 1 > state->num_threads) {
        state->num_threads = max_pcpu + 1;
        if (state->num_threads > MAX_PCPUS)
            state->num_threads = MAX_PCPUS;
    }

    /* Compute utilization: used-time / elapsed-time
     * With HT this maxes at ~50% per logical CPU, so we aggregate
     * per physical core (2 HT threads) and cap at 100%. */
    if (have_prev) {
        for (int i = 0; i < state->num_threads; i++) {
            uint64_t d_used = cur_used[i] - prev_used[i];
            uint64_t d_elapsed = cur_elapsed[i] - prev_elapsed[i];

            if (d_elapsed > 0) {
                state->pcpu_usage[i] = (float)d_used / (float)d_elapsed;
                if (state->pcpu_usage[i] > 1.0f)
                    state->pcpu_usage[i] = 1.0f;
            } else {
                state->pcpu_usage[i] = 0.0f;
            }
        }

        /*
         * Aggregate per package.
         * With HT, used-time is split between sibling threads — each
         * thread maxes at ~50% when both are busy. To get the true
         * package utilization, sum ALL thread usage and divide by
         * the number of PHYSICAL cores (threads/2), not logical threads.
         * This maps: all cores 100% busy → package_usage = 1.0
         */
        float sum[MAX_PACKAGES] = {0};
        int   count[MAX_PACKAGES] = {0};

        for (int i = 0; i < state->num_threads; i++) {
            int pkg = state->pcpu_to_package[i];
            if (pkg >= 0 && pkg < MAX_PACKAGES) {
                sum[pkg] += state->pcpu_usage[i];
                count[pkg]++;
            }
        }

        for (int p = 0; p < state->num_packages; p++) {
            if (count[p] > 0) {
                /* Divide by physical cores (count/2) instead of
                 * logical threads to compensate for HT splitting */
                int phys_cores = count[p] / 2;
                if (phys_cores <= 0) phys_cores = 1;
                state->package_usage[p] = sum[p] / (float)phys_cores;
                if (state->package_usage[p] > 1.0f)
                    state->package_usage[p] = 1.0f;
            } else {
                state->package_usage[p] = 0.0f;
            }
        }
    }

    /* Save current as previous for next delta */
    memcpy(prev_used, cur_used, sizeof(prev_used));
    memcpy(prev_elapsed, cur_elapsed, sizeof(prev_elapsed));

    /* Expose PCPU 0 elapsed-time as uptime (free, already have it) */
    state->uptime_usec = cur_elapsed[0];

    if (!have_prev) {
        have_prev = 1;
        return 1;  /* First sample, no delta yet */
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int cpu_init(cpu_state_t *state)
{
    memset(state, 0, sizeof(*state));
    have_prev = 0;

    fprintf(stderr, "cpu: detecting topology...\n");

    /* Detect PCPU count from vsish */
    int pcpu_count = detect_pcpu_count();
    if (pcpu_count > 0) {
        fprintf(stderr, "cpu: vsish reports %d PCPUs\n", pcpu_count);
    }

    /* Detect topology from esxcli */
    if (detect_topology_popen(state) == 0) {
        fprintf(stderr, "cpu: (esxcli) %d package(s), %d core(s), "
                "%d thread(s)\n",
                state->num_packages, state->num_cores, state->num_threads);
    } else if (detect_topology_cpuinfo(state) == 0) {
        fprintf(stderr, "cpu: (/proc/cpuinfo) %d package(s), %d core(s), "
                "%d thread(s)\n",
                state->num_packages, state->num_cores, state->num_threads);
    } else {
        /* Last resort: use vsish PCPU count if available, else defaults */
        if (pcpu_count > 0) {
            state->num_threads = pcpu_count;
            state->num_packages = 2;
            state->num_cores = pcpu_count / 2;
            if (state->num_cores <= 0) state->num_cores = pcpu_count;
            int per_pkg = state->num_threads / state->num_packages;
            if (per_pkg <= 0) per_pkg = 1;
            for (int i = 0; i < state->num_threads; i++)
                state->pcpu_to_package[i] = i / per_pkg;
            fprintf(stderr, "cpu: using vsish count + defaults: "
                    "2 pkg, %d threads\n", pcpu_count);
        } else {
            state->num_packages = 2;
            state->num_cores = 8;
            state->num_threads = 16;
            int per_pkg = 8;
            for (int i = 0; i < 16; i++)
                state->pcpu_to_package[i] = i / per_pkg;
            fprintf(stderr, "cpu: using Xserve defaults: 2 pkg, "
                    "16 threads\n");
        }
    }

    /* Override thread count with vsish count if it is higher */
    if (pcpu_count > 0 && pcpu_count > state->num_threads) {
        fprintf(stderr, "cpu: adjusting thread count from %d to %d "
                "(vsish)\n", state->num_threads, pcpu_count);
        int old = state->num_threads;
        state->num_threads = pcpu_count;
        if (state->num_threads > MAX_PCPUS)
            state->num_threads = MAX_PCPUS;
        /* Extend package mapping for new PCPUs */
        int per_pkg = state->num_threads / state->num_packages;
        if (per_pkg <= 0) per_pkg = 1;
        for (int i = old; i < state->num_threads; i++) {
            int pkg = i / per_pkg;
            if (pkg >= state->num_packages)
                pkg = state->num_packages - 1;
            state->pcpu_to_package[i] = pkg;
        }
    }

    /* Collect two samples to establish baseline delta */
    fprintf(stderr, "cpu: collecting initial vsish samples...\n");

    int got_first = 0;
    for (int attempt = 0; attempt < 15 && !got_first; attempt++) {
        int ret = sample_vsish(state);
        if (ret >= 0) {
            got_first = 1;
            fprintf(stderr, "cpu: first sample acquired\n");
        } else {
            fprintf(stderr, "cpu: sample attempt %d failed, retrying...\n",
                    attempt + 1);
            sleep(1);
        }
    }

    if (!got_first) {
        fprintf(stderr, "cpu: timeout waiting for vsish data\n");
        return -1;
    }

    /* Wait 1 second then take second sample for delta baseline */
    sleep(1);

    int got_delta = 0;
    for (int attempt = 0; attempt < 10 && !got_delta; attempt++) {
        int ret = sample_vsish(state);
        if (ret == 0) {
            got_delta = 1;
        } else if (ret == 1) {
            /* Still first sample somehow, retry */
            sleep(1);
        } else {
            fprintf(stderr, "cpu: delta sample attempt %d failed\n",
                    attempt + 1);
            sleep(1);
        }
    }

    if (got_delta) {
        fprintf(stderr, "cpu: monitoring active (vsish popen, %d PCPUs)\n",
                state->num_threads);
    } else {
        /* We have at least one sample; delta will be computed on next read */
        fprintf(stderr, "cpu: monitoring active (vsish popen, %d PCPUs, "
                "delta pending)\n", state->num_threads);
    }

    return 0;
}

int cpu_sample(cpu_state_t *state)
{
    /* Poll every 1 second (matching Apple hwmond's rate) */
    sleep(1);

    int ret = sample_vsish(state);
    if (ret == 1) {
        /* First sample (should not happen after init), treat as success */
        return 0;
    }
    return ret;
}

void cpu_shutdown(cpu_state_t *state)
{
    (void)state;
    have_prev = 0;
}
