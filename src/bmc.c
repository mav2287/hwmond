/*
 * bmc.c - Apple BMC/IPMI data population for Xserve
 *
 * Populates the Xserve BMC with system information using Apple's
 * OEM IPMI protocol:
 *   NetFn = 0x36 (Apple OEM)
 *   Cmd   = 0x01 (Set System Info)
 *
 * Wire format (reverse-engineered from PlatformHardwareManagement):
 *   First block: [param, offset, block=0, len_lo, len_hi, data...]
 *   Next blocks: [param, offset, block=N, data...]
 *
 * Platform-independent: data collection is handled by collect_esxi.c
 * or collect_linux.c via the collect.h interface. This file only
 * handles IPMI protocol and BMC parameter formatting.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <poll.h>
#include "bmc.h"
#include "collect.h"

/* ------------------------------------------------------------------ */
/*  IPMI device interface                                              */
/* ------------------------------------------------------------------ */

#define IPMI_BMC_CHANNEL        0xf
#define IPMI_SYSTEM_INTERFACE_ADDR_TYPE 0x0c

struct ipmi_system_interface_addr {
    int32_t addr_type;
    int16_t channel;
    uint8_t lun;
};

struct ipmi_msg {
    uint8_t  netfn;
    uint8_t  cmd;
    uint16_t data_len;
    uint8_t  *data;
};

struct ipmi_req {
    uint8_t *addr;
    uint32_t addr_len;
    int64_t  msgid;
    struct ipmi_msg msg;
};

struct ipmi_recv {
    int32_t  recv_type;
    uint8_t  *addr;
    uint32_t addr_len;
    int64_t  msgid;
    struct ipmi_msg msg;
};

#define IPMI_IOC_MAGIC 'i'
#define IPMICTL_SEND_COMMAND       _IOR(IPMI_IOC_MAGIC, 13, struct ipmi_req)
#define IPMICTL_RECEIVE_MSG_TRUNC  _IOWR(IPMI_IOC_MAGIC, 11, struct ipmi_recv)

static int64_t msg_seq = 1;
static int bmc_available = 0;  /* set if /dev/ipmi0 exists at startup */

/*
 * Open /dev/ipmi0, send one command, receive response, close.
 * Never hold the fd open — ESXi's own IPMI stack needs access.
 */
static int ipmi_cmd(uint8_t netfn, uint8_t cmd,
                    uint8_t *data, uint16_t data_len)
{
    int fd = open("/dev/ipmi0", O_RDWR);
    if (fd < 0) return -1;

    struct ipmi_system_interface_addr addr = {
        .addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE,
        .channel = IPMI_BMC_CHANNEL, .lun = 0
    };
    struct ipmi_req req = {
        .addr = (uint8_t *)&addr, .addr_len = sizeof(addr),
        .msgid = msg_seq++,
        .msg = { .netfn = netfn, .cmd = cmd,
                 .data = data, .data_len = data_len }
    };

    if (ioctl(fd, IPMICTL_SEND_COMMAND, &req) < 0) {
        close(fd);
        return -1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 5000) <= 0) {
        close(fd);
        return -1;
    }

    uint8_t resp_buf[32];
    struct ipmi_system_interface_addr raddr;
    struct ipmi_recv recv = {
        .addr = (uint8_t *)&raddr, .addr_len = sizeof(raddr),
        .msg = { .data = resp_buf, .data_len = sizeof(resp_buf) }
    };

    int ret = ioctl(fd, IPMICTL_RECEIVE_MSG_TRUNC, &recv);
    close(fd);

    if (ret < 0) return -1;
    if (recv.msg.data_len > 0 && resp_buf[0] != 0x00) {
        fprintf(stderr, "bmc: IPMI cc=0x%02X (nf=0x%02X cmd=0x%02X"
                " param=0x%02X)\n",
                resp_buf[0], netfn, cmd,
                data_len > 0 ? data[0] : 0);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Apple OEM IPMI write with multi-block support                      */
/* ------------------------------------------------------------------ */

/*
 * Multi-block write for Apple BMC parameters.
 * Block 0: [param, set_sel, 0, total_len_lo, total_len_hi, data(≤30)]
 * Block N: [param, set_sel, N, data(≤32)]
 * This matches IPMISetAppleSystemInfoParameters exactly.
 */
static int apple_set_multiblock(uint8_t param, uint8_t set_sel,
                                const uint8_t *data, int data_len)
{
    if (!bmc_available) return -1;

    int block = 0, sent = 0;
    while (sent < data_len) {
        uint8_t buf[36]; /* 3 header + up to 32 data + 2 length = 37 max */
        buf[0] = param;
        buf[1] = set_sel;
        buf[2] = (uint8_t)block;

        int chunk, msg_len;
        if (block == 0) {
            buf[3] = (uint8_t)(data_len & 0xFF);
            buf[4] = (uint8_t)((data_len >> 8) & 0xFF);
            chunk = data_len - sent;
            if (chunk > 30) chunk = 30;
            if (chunk > 0) memcpy(buf + 5, data + sent, chunk);
            msg_len = 5 + chunk;
        } else {
            chunk = data_len - sent;
            if (chunk > 32) chunk = 32;
            if (chunk > 0) memcpy(buf + 3, data + sent, chunk);
            msg_len = 3 + chunk;
        }

        int ret = ipmi_cmd(0x36, 0x01, buf, msg_len);
        if (ret < 0) {
            fprintf(stderr, "bmc: multiblock FAILED param=0x%02X sel=%d"
                    " block=%d (sent=%d/%d)\n",
                    param, set_sel, block, sent, data_len);
            return -1;
        }

        sent += chunk;
        block++;
    }
    return 0;
}

/* Single-block write (convenience wrapper for data ≤ 30 bytes) */
static int apple_set_sel(uint8_t param, uint8_t set_sel,
                         const uint8_t *data, int data_len)
{
    return apple_set_multiblock(param, set_sel, data, data_len);
}

/*
 * Clear an Apple BMC parameter for a given set_selector.
 * Sends zeroed block (like IPMIClearAppleSystemParameter).
 */
static int apple_clear(uint8_t param, uint8_t set_sel)
{
    if (!bmc_available) return -1;
    uint8_t buf[35];
    buf[0] = param;
    buf[1] = set_sel;
    buf[2] = 0x00;
    memset(buf + 3, 0, 32);
    return ipmi_cmd(0x36, 0x01, buf, 35);
}

/*
 * Pack binary + N strings into Apple's wire format, then multi-block write.
 * Wire format (reverse-engineered from PlatformHardwareManagementApp):
 *   [binary_data(bin_len)] [0x01 encoding=UTF-8]
 *   [str1_len, str1_data...] [str2_len, str2_data...] ...
 * For null/empty strings: [0x01, 0x00] (length=1, null byte)
 */
#define MAX_PACKED_BUF 128

static int apple_set_packed(uint8_t param, uint8_t set_sel,
                            const uint8_t *binary, int bin_len,
                            const char **strings, int str_count)
{
    uint8_t payload[MAX_PACKED_BUF];
    int pos = 0;

    /* Binary data */
    if (bin_len > 0 && binary) {
        if (bin_len > MAX_PACKED_BUF - 1) bin_len = MAX_PACKED_BUF - 1;
        memcpy(payload, binary, bin_len);
        pos = bin_len;
    }

    /* Encoding marker: 0x01 = UTF-8 */
    payload[pos++] = 0x01;

    /* Pack each string in Apple's wire format:
     * [len = strlen + 2] [string_bytes] [\0] [pad_byte]
     * The +2 accounts for null terminator + 1 padding byte.
     * This matches PlatformHardwareManagement's string conversion at 0xc978
     * which returns usedBufLen + 2 as the length field.
     * For null/empty strings: [0x01, 0x00] (length=1, null byte) */
    for (int i = 0; i < str_count; i++) {
        if (pos + 4 > MAX_PACKED_BUF) break;  /* need room for len + null + pad + margin */
        if (strings[i] && strings[i][0]) {
            int slen = strlen(strings[i]);
            int wire_len = slen + 2;  /* Apple's format: strlen + 2 */
            int avail = MAX_PACKED_BUF - pos - 1;
            if (wire_len > avail) {
                slen = avail - 2;
                if (slen < 0) slen = 0;
                wire_len = slen + 2;
            }
            payload[pos++] = (uint8_t)wire_len;
            memcpy(payload + pos, strings[i], slen);
            pos += slen;
            payload[pos++] = 0x00;  /* null terminator */
            payload[pos++] = 0x00;  /* padding byte */
        } else {
            payload[pos++] = 0x01;
            payload[pos++] = 0x00;
        }
    }

    return apple_set_multiblock(param, set_sel, payload, pos);
}

static int set_binary(uint8_t param, const uint8_t *data, int len)
{
    return apple_set_sel(param, 0x00, data, len);
}

/*
 * Write a parameter that carries only packed strings (no binary).
 * E.g. 0xC4 NetworkStaticInfo (3 strings), 0xC3 DriveStaticInfo.
 */
static int set_strings(uint8_t param, uint8_t set_sel,
                       const char **strings, int count)
{
    return apple_set_packed(param, set_sel, NULL, 0, strings, count);
}

/* ------------------------------------------------------------------ */
/*  Cached data from platform-specific collection                      */
/* ------------------------------------------------------------------ */

static system_info_t cache_sysinfo;
static dimm_info_t   cache_dimms[MAX_DIMMS];
static int           cache_dimm_count = 0;
static drive_info_t  cache_drives[MAX_DRIVES];
static int           cache_drive_count = 0;
static nic_static_t  cache_nic_static[MAX_NICS];
static int           cache_nic_count = 0;
static nic_dynamic_t cache_nic_dynamic[MAX_NICS];
static uint8_t       cache_cpu[12];

/* ------------------------------------------------------------------ */
/*  send_all_cached — write all parameters to BMC                      */
/* ------------------------------------------------------------------ */

static void send_all_cached(uint32_t uptime_secs)
{
    if (!bmc_available) return;

    /* Apple's original 5 — ALL use packed format with encoding marker.
     * String params: 0 binary + N packed strings via set helper.
     * 0x01 FirmwareVersion: 1 string
     * 0x02 SystemName: 1 string
     * 0x03 PrimaryOS: 3 strings [product, version, update]
     * 0x04 CurrentOS: 3 strings [product, version, build]
     * 0xCB ComputerName: 1 string */
    /* System info strings — all from cache_sysinfo (collect.h) */
    if (cache_sysinfo.firmware[0]) {
        const char *s[1] = { cache_sysinfo.firmware };
        apple_set_packed(0x01, 0x00, NULL, 0, s, 1);
    }
    {
        const char *os3[3] = { cache_sysinfo.os_product, cache_sysinfo.os_version, cache_sysinfo.os_update };
        apple_set_packed(0x03, 0x00, NULL, 0, os3, 3);
    }
    {
        const char *os4[3] = { cache_sysinfo.os_product, cache_sysinfo.os_version, cache_sysinfo.os_build };
        apple_set_packed(0x04, 0x00, NULL, 0, os4, 3);
    }

    /* 0xC0 ProcessorInfo: 12 binary + 1 packed string (CPU model) */
    {
        const char *cpu_strs[1] = { cache_sysinfo.cpu_model };
        apple_set_packed(0xC0, 0x00, cache_cpu, 12, cpu_strs, 1);
    }

    /* 0xC1 MiscellaneousInfo: 4 binary (total_ram_mb) + 2 packed strings */
    {
        const char *misc_strs[2] = { cache_sysinfo.model, cache_sysinfo.serial };
        apple_set_packed(0xC1, 0x00,
                         (const uint8_t *)&cache_sysinfo.total_ram_mb, 4,
                         misc_strs, 2);
    }

    if (cache_sysinfo.hostname[0]) {
        const char *s[1] = { cache_sysinfo.hostname };
        apple_set_packed(0x02, 0x00, NULL, 0, s, 1);
    }
    if (cache_sysinfo.fqdn[0]) {
        const char *s[1] = { cache_sysinfo.fqdn };
        apple_set_packed(0xCB, 0x00, NULL, 0, s, 1);
    }

    /* Clear per-device parameters before writing fresh data.
     * Matches original Apple hwmond behavior — prevents ghost devices
     * from showing in Server Monitor when drives/NICs are removed.
     * Original cleared: 0xC3, 0xC4, 0xC5, 0xC6, 0xCA
     * We also clear: 0xC2, 0xC9 (memory) */
    {
        uint8_t clear_params[] = { 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC9 };
        for (int p = 0; p < (int)(sizeof(clear_params)/sizeof(clear_params[0])); p++)
            for (int i = 0; i < MAX_DIMMS; i++)
                apple_clear(clear_params[p], (uint8_t)i);
    }

    for (int i = 0; i < cache_dimm_count; i++) {
        uint8_t bin[6];
        bin[0] = cache_dimms[i].config_type;
        bin[1] = cache_dimms[i].ecc_type;
        memcpy(bin + 2, &cache_dimms[i].size_mb, 4);
        const char *strs[3] = {
            cache_dimms[i].slot_name,
            cache_dimms[i].speed,
            cache_dimms[i].type
        };
        int ret = apple_set_packed(0xC2, (uint8_t)i, bin, 6, strs, 3);
        if (ret < 0)
            fprintf(stderr, "bmc: 0xC2 set_sel=%d FAILED\n", i);
    }

    /* 0xC9 MemoryDynamicInfo: per-DIMM binary (16 bytes)
     * [summary(4), parity_count(4), parity_baseline(4), reserved(4)]
     * Apple hwmond writes this for ECC error reporting. Zeroes = no errors. */
    for (int i = 0; i < cache_dimm_count; i++) {
        uint8_t dyn[16];
        memset(dyn, 0, 16);
        apple_set_sel(0xC9, (uint8_t)i, dyn, 16);
    }

    /* Per-drive: 0xC3 + 0xC5 */
    for (int i = 0; i < cache_drive_count; i++) {
        const char *dstrs[5] = {
            cache_drives[i].kind,
            cache_drives[i].vendor[0] ? cache_drives[i].vendor : NULL,
            cache_drives[i].model,
            cache_drives[i].iface,
            cache_drives[i].location
        };
        apple_set_packed(0xC3, (uint8_t)i,
                         (const uint8_t *)&cache_drives[i].capacity_mb,
                         8, dstrs, 5);
    }
    for (int i = 0; i < cache_drive_count; i++) {
        uint8_t dyn[36];
        memset(dyn, 0, 36);
        const char *dstrs2[2] = { NULL, NULL };
        apple_set_packed(0xC5, (uint8_t)i, dyn, 36, dstrs2, 2);
    }

    /* Per-NIC static data (0xC4 = 0 binary + 3 strings)
     * Order: [HWAddress, UserDefinedName, Name] */
    for (int i = 0; i < cache_nic_count; i++) {
        const char *strs[3] = {
            cache_nic_static[i].mac,     /* HWAddress */
            cache_nic_static[i].name,    /* UserDefinedName */
            cache_nic_static[i].name     /* Name */
        };
        set_strings(0xC4, (uint8_t)i, strs, 3);
    }

    /* Per-NIC dynamic data (0xC6 = 20 binary + 5 strings)
     * Binary: [PacketsIn, PacketsOut, BytesIn, BytesOut, reserved]
     * Strings: [IPAddress, SubNetMask, Link, Mbps, DuplexMode]
     *
     * PROBLEM: The Xserve BMC may not support multi-block writes for 0xC6.
     * The strings beyond block 0 (>30 bytes) would be lost.
     * Block 0 fits: 20 binary + 1 encoding + 9 bytes of strings = 30.
     * That's only enough for ~2 short strings.
     *
     * WORKAROUND: Reorder so Link/Speed/Duplex come FIRST (they're short),
     * then IP/Mask (longer). If block 1 fails, we still get the important
     * status strings. Also try with explicit block-by-block logging. */
    for (int i = 0; i < cache_nic_count; i++) {
        uint8_t bin[20];
        memset(bin, 0, 20);  /* packet counters = 0 */
        /* Reorder strings: put short critical ones first.
         * SM reads by key name not position — the framework maps
         * string index to keys. So order must match the original:
         * [IPAddress, SubNetMask, Link, Mbps, DuplexMode]
         * We can't reorder. Instead, make IP/mask empty when not
         * available so they take only 2 bytes each, leaving room
         * for Link/Speed/Duplex in block 0. */
        const char *ip = cache_nic_dynamic[i].ipv4;
        const char *mask = cache_nic_dynamic[i].netmask;
        /* For NIC without IP, use empty so they take minimal space */
        const char *strs[5] = {
            (ip[0]) ? ip : NULL,
            (mask[0]) ? mask : NULL,
            cache_nic_dynamic[i].link,
            cache_nic_dynamic[i].mbps,
            cache_nic_dynamic[i].duplex
        };
        int ret = apple_set_packed(0xC6, (uint8_t)i, bin, 20, strs, 5);
        if (ret < 0)
            fprintf(stderr, "bmc: 0xC6 NIC %d write FAILED\n", i);
    }

    set_binary(0xC7, (uint8_t *)&uptime_secs, 4);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int bmc_init(void)
{
    /* Verify this is Apple Xserve hardware before touching the BMC.
     * Apple OEM IPMI commands (NetFn 0x36) and the KCS port config
     * could damage non-Apple BMCs (Dell iDRAC, HP iLO, etc.). */
    {
        char model[128] = "";
        FILE *fp = fopen("/sys/class/dmi/id/product_name", "r");
        if (fp) {
            if (fgets(model, sizeof(model), fp)) {
                int l = strlen(model);
                while (l > 0 && (model[l-1]=='\n'||model[l-1]==' '))
                    model[--l] = '\0';
            }
            fclose(fp);
        }
        if (strstr(model, "Xserve") == NULL) {
            fprintf(stderr, "bmc: not Apple Xserve hardware (model: %s)\n",
                    model[0] ? model : "unknown");
            fprintf(stderr, "bmc: BMC data population disabled "
                    "(Apple OEM IPMI is Xserve-only)\n");
            return -1;
        }
        fprintf(stderr, "bmc: hardware verified: %s\n", model);
    }

    /* Check if IPMI is available (don't hold fd open) */
    int test_fd = open("/dev/ipmi0", O_RDWR);
    if (test_fd < 0) {
        fprintf(stderr, "bmc: /dev/ipmi0 not available: %s\n",
                strerror(errno));
        fprintf(stderr, "bmc: BMC data population disabled\n");
        return -1;
    }
    close(test_fd);
    bmc_available = 1;

    fprintf(stderr, "bmc: collecting system data...\n");

    /* Collect system info via platform-specific implementation */
    collect_system_info(&cache_sysinfo);
    fprintf(stderr, "bmc: os: %s %s %s %s\n",
            cache_sysinfo.os_product, cache_sysinfo.os_version,
            cache_sysinfo.os_update, cache_sysinfo.os_build);
    fprintf(stderr, "bmc: cpu: %s (%d sockets, %d cores, %d MHz)\n",
            cache_sysinfo.cpu_model, cache_sysinfo.cpu_packages,
            cache_sysinfo.cpu_cores, cache_sysinfo.cpu_speed_mhz);
    fprintf(stderr, "bmc: misc: model=%s serial=%s ram=%u MB\n",
            cache_sysinfo.model, cache_sysinfo.serial,
            cache_sysinfo.total_ram_mb);

    /* Build CPU binary: [packages, speed_mhz, cores_per_package] */
    {
        int cores_per_pkg = (cache_sysinfo.cpu_packages > 0) ?
            (cache_sysinfo.cpu_cores / cache_sysinfo.cpu_packages) :
            cache_sysinfo.cpu_cores;
        uint32_t pdata[3] = {
            (uint32_t)cache_sysinfo.cpu_packages,
            (uint32_t)cache_sysinfo.cpu_speed_mhz,
            (uint32_t)cores_per_pkg
        };
        memcpy(cache_cpu, pdata, 12);
    }

    /* Collect per-DIMM memory info via platform-specific implementation */
    cache_dimm_count = collect_dimm_info(cache_dimms, MAX_DIMMS);
    {
        uint64_t total_mb = 0;
        for (int i = 0; i < cache_dimm_count; i++)
            total_mb += cache_dimms[i].size_mb;
        fprintf(stderr, "bmc: memory: %llu MB total, %d DIMMs\n",
                (unsigned long long)total_mb, cache_dimm_count);
        for (int i = 0; i < cache_dimm_count; i++)
            fprintf(stderr, "bmc:   [%d] %s: %u MB, %s, %s\n",
                    i, cache_dimms[i].slot_name,
                    cache_dimms[i].size_mb,
                    cache_dimms[i].speed, cache_dimms[i].type);
    }

    /* Collect drives */
    cache_drive_count = collect_drive_info(cache_drives, MAX_DRIVES);
    fprintf(stderr, "bmc: drives: %d devices\n", cache_drive_count);
    for (int i = 0; i < cache_drive_count; i++)
        fprintf(stderr, "bmc:   [%d] %s\n", i, cache_drives[i].display);

    /* Collect NICs */
    cache_nic_count = collect_nic_static(cache_nic_static, MAX_NICS);
    collect_nic_dynamic(cache_nic_dynamic, cache_nic_static, cache_nic_count);
    fprintf(stderr, "bmc: nics: %d devices\n", cache_nic_count);
    for (int i = 0; i < cache_nic_count; i++)
        fprintf(stderr, "bmc:   [%d] %s %s %s\n", i,
                cache_nic_static[i].name, cache_nic_static[i].mac,
                cache_nic_dynamic[i].ipv4);

    /* Initialize change detection */
    detect_drive_changes();
    detect_network_changes();

    /* Get system uptime for initial send.
     * Try multiple sources — /proc/uptime may not exist on ESXi. */
    uint32_t init_uptime = 0;
    {
        FILE *uf;
        /* Try /proc/uptime first (Linux compat) */
        uf = popen("cat /proc/uptime 2>/dev/null", "r");
        if (uf) {
            double up = 0;
            if (fscanf(uf, "%lf", &up) == 1 && up > 0)
                init_uptime = (uint32_t)up;
            pclose(uf);
        }
        /* Fallback: parse 'uptime' command output */
        if (init_uptime == 0) {
            uf = popen("uptime 2>/dev/null", "r");
            if (uf) {
                char ubuf[256];
                if (fgets(ubuf, sizeof(ubuf), uf)) {
                    /* ESXi format: " HH:MM:MM up N days, HH:MM, ..." */
                    char *up = strstr(ubuf, "up ");
                    if (up) {
                        up += 3;
                        int days = 0, hrs = 0, mins = 0;
                        if (strstr(up, "day")) {
                            sscanf(up, "%d day", &days);
                            char *comma = strchr(up, ',');
                            if (comma) sscanf(comma + 1, "%d:%d", &hrs, &mins);
                        } else {
                            sscanf(up, "%d:%d", &hrs, &mins);
                        }
                        init_uptime = days * 86400 + hrs * 3600 + mins * 60;
                    }
                }
                pclose(uf);
            }
        }
        if (init_uptime > 0)
            fprintf(stderr, "bmc: initial uptime: %u sec\n", init_uptime);
    }

    /* Sync BMC clock — same as original Apple hwmond (once at startup).
     * Standard IPMI: NetFn=0x0A (Storage), Cmd=0x49 (Set SEL Time).
     * Sends 4-byte Unix timestamp so BMC event log has correct times. */
    {
        time_t now = time(NULL);
        uint32_t ts = (uint32_t)now;
        int ret = ipmi_cmd(0x0A, 0x49, (uint8_t *)&ts, 4);
        if (ret == 0)
            fprintf(stderr, "bmc: SEL time synced to %u\n", ts);
        else
            fprintf(stderr, "bmc: SEL time sync failed\n");
    }

    /* Send everything to BMC */
    send_all_cached(init_uptime);

    fprintf(stderr, "bmc: all data sent to BMC\n");
    return 0;
}

int bmc_update(uint64_t uptime_usec)
{
    if (!bmc_available) return -1;

    /* Check for drive changes via platform-specific detection */
    if (detect_drive_changes() > 0) {
        fprintf(stderr, "bmc: drive change detected\n");
        cache_drive_count = collect_drive_info(cache_drives, MAX_DRIVES);
    }

    /* Check for network changes via platform-specific detection */
    if (detect_network_changes() > 0) {
        fprintf(stderr, "bmc: network change detected\n");
        collect_nic_dynamic(cache_nic_dynamic, cache_nic_static, cache_nic_count);
    }

    /* Resend everything with fresh uptime */
    uint32_t secs = (uint32_t)(uptime_usec / 1000000);
    send_all_cached(secs);

    return 0;
}

/*
 * Graceful shutdown/restart from Apple Server Monitor:
 * NOT handled by hwmond. ESXi's own hostd/IPMI stack receives the
 * BMC message (0x36 0x04 [0x01/0x02]) and handles it natively:
 *   - Runs Auto Start Power Off for all VMs
 *   - Gracefully shuts down/restarts the host
 * Confirmed by live testing: hostd processes the request without
 * any assistance from hwmond.
 *
 * Apple OEM graceful control commands (documented for reference):
 *   0x36 0x04 [0x01] = graceful shutdown (remote app → BMC)
 *   0x36 0x04 [0x02] = graceful restart  (remote app → BMC)
 *   0x36 0x05 [0x01] = shutdown acknowledged (OS agent → BMC)
 *   0x36 0x05 [0x02] = restart acknowledged (OS agent → BMC)
 */

void bmc_shutdown(void)
{
    bmc_available = 0;
}
