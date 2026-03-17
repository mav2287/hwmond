/*
 * bmc.h - Apple BMC/IPMI data population for Xserve
 *
 * Sends system information to the Xserve's BMC via Apple OEM
 * IPMI commands (NetFn=0x36, Cmd=0x01). This data is visible
 * through the LOM (Lights-Out Management) interface and can be
 * read by Apple Server Monitor.
 */

#ifndef BMC_H
#define BMC_H

#include <stdint.h>

/*
 * Initialize BMC communication and query all system info.
 * Called once at startup. Caches all data for periodic resend.
 * Safe to call even if /dev/ipmi0 doesn't exist.
 */
int bmc_init(void);

/*
 * Resend all cached BMC data + fresh uptime.
 * Called every 60 seconds from CPU thread.
 * Detects drive/network changes via stat() and re-queries only if needed.
 *
 * uptime_usec: current elapsed-time from PCPU 0 (already available
 *              in the CPU thread, zero additional cost).
 */
int bmc_update(uint64_t uptime_usec);

/*
 * Close BMC device. Called at shutdown.
 */
void bmc_shutdown(void);

#endif /* BMC_H */
