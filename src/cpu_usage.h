/*
 * cpu_usage.h - ESXi per-package CPU usage monitoring
 *
 * Reads per-physical-CPU utilization from ESXi and aggregates
 * it per physical package (socket) for the Xserve LED bar graph.
 */

#ifndef CPU_USAGE_H
#define CPU_USAGE_H

#include <stdint.h>

#define MAX_PCPUS    64
#define MAX_PACKAGES 2

/* CPU topology and usage state */
typedef struct {
    /* Topology (detected at init) */
    int num_packages;
    int num_cores;
    int num_threads;
    int pcpu_to_package[MAX_PCPUS];

    /* Per-PCPU usage (0.0 - 1.0), updated each sample */
    float pcpu_usage[MAX_PCPUS];

    /* Per-package aggregated usage (0.0 - 1.0) */
    float package_usage[MAX_PACKAGES];

    /* Raw elapsed-time from PCPU 0 (microseconds since boot = uptime) */
    uint64_t uptime_usec;

    /* esxtop column mapping (parsed from header) */
    int   pcpu_columns[MAX_PCPUS];
    int   num_columns;
    int   header_parsed;
} cpu_state_t;

/*
 * Detect CPU topology and initialize monitoring.
 * Returns 0 on success, -1 on failure.
 */
int cpu_init(cpu_state_t *state);

/*
 * Read one sample of CPU usage. Blocks until data is available.
 * Updates state->package_usage[0..num_packages-1].
 * Returns 0 on success, -1 on failure.
 */
int cpu_sample(cpu_state_t *state);

/*
 * Stop monitoring and free resources.
 */
void cpu_shutdown(cpu_state_t *state);

#endif /* CPU_USAGE_H */
