/*
 * bmc.c - Apple BMC/IPMI data population for Xserve on ESXi
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
 * Strategy:
 *   - At startup: query all data via popen, cache it, send to BMC
 *   - Every 60 seconds: resend all cached data + fresh uptime
 *   - stat() drives/network dirs to detect changes (re-query only if needed)
 *   - Uptime comes free from the CPU thread's elapsed-time (zero cost)
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
        if (pos + 2 > MAX_PACKED_BUF) break;
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
/*  ESXi data collection helpers                                       */
/* ------------------------------------------------------------------ */

static int popen_field(const char *cmd, const char *field,
                       char *buf, int bufsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[512];
    buf[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, field);
        if (p) {
            p += strlen(field);
            while (*p == ' ' || *p == ':') p++;
            int len = strlen(p);
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'
                               || p[len-1] == ' '))
                p[--len] = '\0';
            strncpy(buf, p, bufsz - 1);
            buf[bufsz - 1] = '\0';
            pclose(fp);
            return strlen(buf);
        }
    }
    pclose(fp);
    return -1;
}

static int popen_line(const char *cmd, char *buf, int bufsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    buf[0] = '\0';
    if (fgets(buf, bufsz, fp)) {
        int len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
    }
    pclose(fp);
    return strlen(buf);
}

static void collapse_spaces(char *str)
{
    char *src = str, *dst = str;
    int prev_space = 1;
    while (*src) {
        if (*src == ' ') {
            if (!prev_space) *dst++ = ' ';
            prev_space = 1;
        } else {
            *dst++ = *src;
            prev_space = 0;
        }
        src++;
    }
    if (dst > str && *(dst-1) == ' ') dst--;
    *dst = '\0';
}

/* ------------------------------------------------------------------ */
/*  Cached BMC data                                                    */
/* ------------------------------------------------------------------ */

/* Max cached string size */
#define CACHE_SIZE 256

static char cache_firmware[CACHE_SIZE];
static char cache_hostname[CACHE_SIZE];
static char cache_fqdn[CACHE_SIZE];

/* 0x03/0x04 OS info — 3 strings each: [product, version, build] */
static char cache_os_product[CACHE_SIZE];  /* "VMware ESXi" */
static char cache_os_version[CACHE_SIZE];  /* "6.5.0" */
static char cache_os_update[CACHE_SIZE];   /* "Update 3" */
static char cache_os_build[CACHE_SIZE];    /* "Build 8294253" or "U3 P169" */

/* String caches */
static char cache_cpu_model[CACHE_SIZE]; /* CPU model name for 0xC0 string */
static char cache_serial[CACHE_SIZE];    /* Serial for 0xC1 string 2 */
static char cache_model[CACHE_SIZE];     /* Model for 0xC1 string 1 */
static uint32_t cache_total_ram_mb;      /* Total RAM in MB for 0xC1 binary */

/*
 * 0xC2 MemoryInfo per DIMM (reverse-engineered from Server Monitor):
 * Binary (6 bytes): [config_type, ecc_type, size_mb(uint32 LE)]
 *   config_type: 0x00 = populated (SM skips non-zero when firmware_supports_extra=false)
 *   ecc_type:    0x00 = no ECC display, 0x02 = show ECC error count
 * Strings (3): Slot name, Speed, Type
 *
 * 0xC9 MemoryDynamicInfo per DIMM:
 * Binary (16 bytes): 4 x uint32 LE [summary, parity_count, parity_baseline, reserved]
 *   Apple's hwmond writes this for ECC error reporting.
 */
#define MAX_DIMMS 16
struct dimm_info {
    uint8_t  config_type; /* 0x00 = populated */
    uint8_t  ecc_type;    /* 0x00 = no ECC display */
    uint32_t size_mb;
    char     slot_name[32]; /* e.g. "DIMM 1" */
    char     speed[16];     /* e.g. "1066 MHz" */
    char     type[16];      /* e.g. "DDR3 ECC" */
};
static struct dimm_info cache_dimms[MAX_DIMMS];
static int cache_dimm_count = 0;

/* 0xC3 DriveStaticInfo per drive: 8 bytes binary (uint64 capacity) */
#define MAX_DRIVES 8
struct drive_info {
    uint32_t capacity_mb;  /* SM reads bytes 0-3 as int32 via numberWithInt: */
    uint32_t reserved;     /* bytes 4-7: must be 0 (was being read as bay#) */
};
static struct drive_info cache_drive_binary[MAX_DRIVES];
static char cache_drive_model[MAX_DRIVES][32];
static char cache_drive_kind[MAX_DRIVES][8];      /* "SSD" or "HDD" */
static char cache_drive_iface[MAX_DRIVES][16];    /* "SATA", "SAS" */
static char cache_drive_vendor[MAX_DRIVES][32];   /* Manufacturer */
static char cache_drive_location[MAX_DRIVES][16]; /* "Bay 1", "Bay 2" */
static uint8_t cache_cpu[12];

/* Change detection */
static time_t network_mtime = 0;
static int    drive_count = 0;

/* Count entries in /vmfs/devices/disks/ — only changes on add/remove */
static int count_disk_devices(void)
{
    FILE *fp = popen("ls /vmfs/devices/disks/ 2>/dev/null | wc -l", "r");
    if (!fp) return -1;
    char buf[32];
    int count = 0;
    if (fgets(buf, sizeof(buf), fp))
        count = atoi(buf);
    pclose(fp);
    return count;
}

/* ------------------------------------------------------------------ */
/*  Data collection functions                                          */
/* ------------------------------------------------------------------ */

/*
 * Collect per-drive data and write each as a separate set_selector.
 * Format per drive (max 30 bytes):
 *   "Model SizeG Type"
 *   e.g. "Samsung SSD 970 1863G SSD"
 */
static char cache_drive_entries[MAX_DRIVES][32];
static int  cache_drive_count = 0;

static void collect_drives(void)
{
    FILE *fp = popen("/bin/esxcli storage core device list 2>/dev/null", "r");
    if (!fp) return;

    char line[512];
    char dev_id[256] = "", model[64] = "", size_str[32] = "";
    char vendor[32] = "", is_ssd[8] = "", is_sas[8] = "";
    char dev_type[32] = "", display_name[128] = "";
    cache_drive_count = 0;

    /* Helper: save the currently-accumulated device as a drive entry */
    int have_device = 0;

    while (1) {
        int got_line = (fgets(line, sizeof(line), fp) != NULL);
        char *p;
        int l;

        /* At EOF or new device ID line: save previous device */
        int new_device = (!got_line) ||
            (line[0] != ' ' && line[0] != '\n' && line[0] != '-' &&
             strlen(line) > 3);

        if (new_device && have_device) {
            /* Save if it's a valid Direct-Access device with size */
            if (model[0] && atol(size_str) > 0 &&
                strstr(dev_type, "Direct") &&
                cache_drive_count < MAX_DRIVES) {
                long mb = atol(size_str);
                int idx = cache_drive_count;

                cache_drive_binary[idx].capacity_mb = (uint32_t)mb;
                cache_drive_binary[idx].reserved = 0;

                strncpy(cache_drive_kind[idx],
                        strstr(is_ssd, "true") ? "SSD" : "HDD", 7);
                strncpy(cache_drive_model[idx], model, 31);

                /* Manufacturer from multiple sources */
                cache_drive_vendor[idx][0] = '\0';
                if (vendor[0] && strcmp(vendor, "ATA") != 0 &&
                    strcmp(vendor, "NVMe") != 0)
                    strncpy(cache_drive_vendor[idx], vendor, 31);
                if (!cache_drive_vendor[idx][0] &&
                    strncmp(dev_id, "t10.", 4) == 0) {
                    char *s = dev_id + 4;
                    while (*s && *s != '_') s++;
                    while (*s == '_') s++;
                    if (*s) {
                        char *e = s;
                        while (*e && *e != '_' && *e != ' ') e++;
                        int wl = e - s;
                        if (wl >= 2 && wl < 30) {
                            strncpy(cache_drive_vendor[idx], s, wl);
                            cache_drive_vendor[idx][wl] = '\0';
                        }
                    }
                }
                if (!cache_drive_vendor[idx][0] && display_name[0] &&
                    strncmp(display_name, "Local", 5) != 0) {
                    char *sp = strchr(display_name, ' ');
                    if (sp && (sp - display_name) >= 2 &&
                        (sp - display_name) < 30) {
                        strncpy(cache_drive_vendor[idx], display_name,
                                sp - display_name);
                        cache_drive_vendor[idx][sp - display_name] = '\0';
                    }
                }
                if (!cache_drive_vendor[idx][0]) {
                    char *sp = strchr(model, ' ');
                    if (sp && (sp - model) >= 2 && (sp - model) < 20) {
                        strncpy(cache_drive_vendor[idx], model, sp - model);
                        cache_drive_vendor[idx][sp - model] = '\0';
                    }
                }

                /* Interconnect — now reads Is SAS correctly */
                if (strstr(dev_id, "NVMe") || strstr(dev_id, "nvme"))
                    strncpy(cache_drive_iface[idx], "NVMe", 15);
                else if (strstr(display_name, "Fibre Channel"))
                    strncpy(cache_drive_iface[idx], "Fibre Channel", 15);
                else if (strstr(is_sas, "true"))
                    strncpy(cache_drive_iface[idx], "SAS", 15);
                else
                    strncpy(cache_drive_iface[idx], "SATA", 15);

                snprintf(cache_drive_location[idx],
                         sizeof(cache_drive_location[idx]),
                         "Bay %d", idx + 1);

                int gb = (int)(mb / 1024);
                snprintf(cache_drive_entries[idx], 31,
                         "%s %dG %s", model, gb,
                         strstr(is_ssd, "true") ? "SSD" : "HDD");
                cache_drive_entries[idx][31] = '\0';
                cache_drive_count++;
            }

            /* Reset for next device */
            dev_id[0] = model[0] = size_str[0] = '\0';
            vendor[0] = is_ssd[0] = is_sas[0] = '\0';
            dev_type[0] = display_name[0] = '\0';
            have_device = 0;
        }

        if (!got_line) break;

        /* Parse current line */
        if (line[0] != ' ' && line[0] != '\n' && line[0] != '-') {
            l = strlen(line);
            while (l > 0 && (line[l-1] == '\n' || line[l-1] == ' '))
                line[--l] = '\0';
            strncpy(dev_id, line, sizeof(dev_id) - 1);
            have_device = 1;
        } else if ((p = strstr(line, "Display Name:")) != NULL &&
                   !strstr(line, "Settable")) {
            p += 13; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(display_name, p, sizeof(display_name) - 1);
        } else if ((p = strstr(line, "Model:")) != NULL &&
                   !strstr(line, "Multipath")) {
            p += 6; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(model, p, sizeof(model) - 1);
        } else if ((p = strstr(line, "Vendor:")) != NULL) {
            p += 7; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(vendor, p, sizeof(vendor) - 1);
        } else if ((p = strstr(line, "Device Type:")) != NULL) {
            p += 12; while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(dev_type, p, sizeof(dev_type) - 1);
        } else if (strstr(line, "  Size:") && !strstr(line, "Queue") &&
                   !strstr(line, "Cache") && !strstr(line, "Sample")) {
            p = strstr(line, "Size:") + 5;
            while (*p == ' ') p++;
            l = strlen(p);
            while (l > 0 && (p[l-1]=='\n'||p[l-1]==' ')) p[--l]='\0';
            strncpy(size_str, p, sizeof(size_str) - 1);
        } else if ((p = strstr(line, "Is SAS:")) != NULL) {
            p += 7; while (*p == ' ') p++;
            strncpy(is_sas, p, 5); is_sas[5] = '\0';
        } else if ((p = strstr(line, "Is SSD:")) != NULL) {
            p += 7; while (*p == ' ') p++;
            strncpy(is_ssd, p, 5); is_ssd[5] = '\0';
        }
    }
    pclose(fp);
}

/*
 * NIC data. Each NIC gets its own set_selector.
 * 0xC4 (NetworkStaticInfo):  0 binary, 3 strings: [name, MAC, driver]
 * 0xC6 (NetworkDynamicInfo): 20 binary, 5 strings: [ip, netmask, gateway, dns1, dns2]
 */
#define MAX_NICS 4

struct nic_static {
    char name[16];    /* e.g. "vmnic0" */
    char mac[24];     /* e.g. "00:24:36:f4:06:4e" */
    char driver[32];  /* e.g. "e1000e" */
};
static struct nic_static cache_nic_static[MAX_NICS];
static int cache_nic_count = 0;

static void collect_nics(void)
{
    FILE *fp = popen("/bin/esxcli network nic list 2>/dev/null", "r");
    if (!fp) return;

    char line[512];
    cache_nic_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        char name[16], pci[16], driver[16], admin[8], link[8];
        char duplex[8], mac[24];
        int speed;
        if (sscanf(line, "%15s %15s %15s %7s %7s %d %7s %23s",
                   name, pci, driver, admin, link, &speed, duplex, mac) >= 8
            && strncmp(name, "vmnic", 5) == 0
            && cache_nic_count < MAX_NICS) {
            strncpy(cache_nic_static[cache_nic_count].name, name, 15);
            strncpy(cache_nic_static[cache_nic_count].mac, mac, 23);
            strncpy(cache_nic_static[cache_nic_count].driver, driver, 31);
            cache_nic_count++;
        }
    }
    pclose(fp);
}

/*
 * 0xC6 NetworkDynamicInfo (reverse-engineered from Server Monitor):
 * Binary (5 x uint32 LE): [PacketsIn, PacketsOut, BytesIn, BytesOut, reserved]
 *   = traffic counters, NOT link/speed/duplex!
 * Strings (5): [IPAddress, SubNetMask, Link, Mbps, DuplexMode]
 *   Link, speed, and duplex are STRINGS (e.g. "Up", "1000", "Full")
 */
struct net_dynamic {
    /* Binary: traffic counters */
    uint32_t packets_in;
    uint32_t packets_out;
    uint32_t bytes_in;
    uint32_t bytes_out;
    uint32_t reserved;
    /* Strings */
    char ipv4[20];        /* "192.168.1.100" */
    char netmask[20];     /* "255.255.255.0" */
    char link[16];        /* "CONNECTED" or "DISCONNECTED" */
    char mbps[16];        /* "1000" */
    char duplex[8];       /* "Full" or "Half" */
};

static struct net_dynamic cache_net_dynamic[MAX_NICS];

static void collect_net_dynamic(void)
{
    /* Link state, speed, duplex from NIC list — these go in STRINGS */
    FILE *fp = popen("/bin/esxcli network nic list 2>/dev/null", "r");
    if (!fp) return;

    char line[512];
    int idx = 0;

    while (fgets(line, sizeof(line), fp)) {
        char name[16], pci[16], driver[16], admin[8], link[8];
        char duplex_str[8], mac[24];
        int speed;
        if (sscanf(line, "%15s %15s %15s %7s %7s %d %7s %23s",
                   name, pci, driver, admin, link, &speed, duplex_str, mac) >= 8
            && strncmp(name, "vmnic", 5) == 0
            && idx < MAX_NICS) {
            /* Binary = traffic counters (zero at startup) */
            memset(&cache_net_dynamic[idx], 0, sizeof(cache_net_dynamic[idx]));
            /* Strings */
            /* Server Monitor compares Link string against "active" for LINK-UP */
            strncpy(cache_net_dynamic[idx].link,
                    (strcmp(link, "Up") == 0) ? "active" : "inactive",
                    sizeof(cache_net_dynamic[idx].link) - 1);
            snprintf(cache_net_dynamic[idx].mbps, sizeof(cache_net_dynamic[idx].mbps),
                     "%d", speed);
            strncpy(cache_net_dynamic[idx].duplex, duplex_str, 7);
            cache_net_dynamic[idx].ipv4[0] = '\0';
            cache_net_dynamic[idx].netmask[0] = '\0';
            idx++;
        }
    }
    pclose(fp);

    /* Get IP addresses from vmkernel interfaces.
     * Map vmk* IPs to the first NIC for now — ESXi ties vmk0 to vmnic0. */
    /* Get IP addresses from vmkernel interfaces */
    fp = popen("/bin/esxcli network ip interface ipv4 get 2>/dev/null", "r");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            char iface[16], ip[20], mask[20];
            if (sscanf(line, "%15s %19s %19s", iface, ip, mask) >= 3) {
                if (strncmp(iface, "vmk", 3) == 0) {
                    int vmk_idx = atoi(iface + 3);
                    if (vmk_idx >= 0 && vmk_idx < cache_nic_count) {
                        strncpy(cache_net_dynamic[vmk_idx].ipv4, ip, 19);
                        strncpy(cache_net_dynamic[vmk_idx].netmask, mask, 19);
                    }
                }
            }
        }
        pclose(fp);
    }
}

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
    if (cache_firmware[0]) {
        const char *s[1] = { cache_firmware };
        apple_set_packed(0x01, 0x00, NULL, 0, s, 1);
    }
    {
        const char *os3[3] = { cache_os_product, cache_os_version, cache_os_update };
        apple_set_packed(0x03, 0x00, NULL, 0, os3, 3);
    }
    {
        const char *os4[3] = { cache_os_product, cache_os_version, cache_os_build };
        apple_set_packed(0x04, 0x00, NULL, 0, os4, 3);
    }

    /* 0xC0 ProcessorInfo: 12 binary + 1 packed string (CPU model) */
    {
        const char *cpu_strs[1] = { cache_cpu_model };
        apple_set_packed(0xC0, 0x00, cache_cpu, 12, cpu_strs, 1);
    }

    /* 0xC1 MiscellaneousInfo: 4 binary (total_ram_mb) + 2 packed strings (Model, Serial)
     * The "RAM" key in Server Monitor reads the uint32 from the binary bytes.
     * Previously we sent raw ASCII here which caused the 803 TB display. */
    {
        const char *misc_strs[2] = { cache_model, cache_serial };
        apple_set_packed(0xC1, 0x00,
                         (const uint8_t *)&cache_total_ram_mb, 4,
                         misc_strs, 2);
    }

    /* Additional string params — also packed format */
    if (cache_hostname[0]) {
        const char *s[1] = { cache_hostname };
        apple_set_packed(0x02, 0x00, NULL, 0, s, 1);
    }
    if (cache_fqdn[0]) {
        const char *s[1] = { cache_fqdn };
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

    /* Per-drive: 0xC3 = 8 binary (Capacity uint64) + 5 strings
     * Strings: [Kind, Manufacturer, Model, Interconnect, Location] */
    for (int i = 0; i < cache_drive_count; i++) {
        const char *dstrs[5] = {
            cache_drive_kind[i],      /* Kind: "SSD" or "HDD" */
            cache_drive_vendor[i][0] ? cache_drive_vendor[i] : NULL,
            cache_drive_model[i],     /* Model */
            cache_drive_iface[i],     /* Interconnect: "SAS", "SATA" */
            cache_drive_location[i]   /* Location: "Bay 1" */
        };
        apple_set_packed(0xC3, (uint8_t)i,
                         (const uint8_t *)&cache_drive_binary[i],
                         8, dstrs, 5);
    }

    /* Per-drive: 0xC5 DriveDynamicInfo2 = 36 binary + 2 strings
     * Binary (36 bytes): [BytesRead(8), BytesWritten(8), ... stats]
     * Strings: [SMARTMessage, RaidLevel]
     * Server Monitor needs both 0xC3 and 0xC5 to show drives.
     * Send zeros for stats, empty strings for SMART/RAID. */
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
        const char *ip = cache_net_dynamic[i].ipv4;
        const char *mask = cache_net_dynamic[i].netmask;
        /* For NIC without IP, use empty so they take minimal space */
        const char *strs[5] = {
            (ip[0]) ? ip : NULL,
            (mask[0]) ? mask : NULL,
            cache_net_dynamic[i].link,
            cache_net_dynamic[i].mbps,
            cache_net_dynamic[i].duplex
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
    char buf[256];

    /* Firmware */
    if (popen_field("vsish -e get /hardware/bios/biosInfo",
                    "BIOS Version", buf, sizeof(buf)) > 0) {
        char *fw = buf; while (*fw == ' ') fw++;
        strncpy(cache_firmware, fw, CACHE_SIZE - 1);
    }

    /* Hostname */
    popen_field("/bin/esxcli system hostname get", "Host Name",
                cache_hostname, CACHE_SIZE);

    /* OS info — 0x03 and 0x04 each take 3 strings */
    {
        popen_field("/bin/esxcli system version get", "Product",
                    cache_os_product, CACHE_SIZE);
        popen_field("/bin/esxcli system version get", "Version",
                    cache_os_version, CACHE_SIZE);

        char update[32] = "", patch[32] = "", build[64] = "";
        popen_field("/bin/esxcli system version get", "Update",
                    update, sizeof(update));
        popen_field("/bin/esxcli system version get", "Patch",
                    patch, sizeof(patch));
        popen_field("/bin/esxcli system version get", "Build",
                    build, sizeof(build));

        snprintf(cache_os_update, CACHE_SIZE, "Update %s", update);
        snprintf(cache_os_build, CACHE_SIZE, "Build %s", build);

        fprintf(stderr, "bmc: os: %s %s %s %s\n",
                cache_os_product, cache_os_version,
                cache_os_update, cache_os_build);
    }

    /* FQDN */
    popen_field("/bin/esxcli system hostname get",
                "Fully Qualified Domain Name", cache_fqdn, CACHE_SIZE);

    /* CPU */
    {
        int pkgs = 0, cores = 0, speed = 0;
        popen_field("/bin/esxcli hardware cpu global get",
                    "CPU Packages", buf, sizeof(buf));
        pkgs = atoi(buf);
        popen_field("/bin/esxcli hardware cpu global get",
                    "CPU Cores", buf, sizeof(buf));
        cores = atoi(buf);
        popen_field("/bin/esxcli hardware cpu list",
                    "Core Speed", buf, sizeof(buf));
        /* Round to nearest 10 MHz for clean display (3324→3330) */
        speed = (int)((atol(buf) + 5000000) / 10000000) * 10;
        int cores_per_pkg = (pkgs > 0) ? (cores / pkgs) : cores;
        /* Apple format: [packages, speed_mhz, cores_per_package]
         * SM divides field2 by 1000 to get GHz.
         * Must be MHz (not Hz) — Hz overflows signed int32 above 2.1 GHz */
        uint32_t pdata[3] = { (uint32_t)pkgs, (uint32_t)speed, (uint32_t)cores_per_pkg };
        memcpy(cache_cpu, pdata, 12);

        char cpu_model[128];
        popen_line("vsish -e get /hardware/cpu/cpuModelName",
                   cpu_model, sizeof(cpu_model));
        char *m = cpu_model; while (*m == ' ') m++;
        collapse_spaces(m);
        /* Strip "@ X.XXGHz" suffix — SM already shows speed from binary data */
        char *at = strstr(m, " @");
        if (!at) at = strstr(m, " @ ");
        if (at) *at = '\0';
        /* Also strip trailing spaces after truncation */
        int ml = strlen(m);
        while (ml > 0 && m[ml-1] == ' ') m[--ml] = '\0';
        strncpy(cache_cpu_model, m, CACHE_SIZE - 1);
        fprintf(stderr, "bmc: cpu: %s (%d sockets, %d cores, %d MHz)\n",
                m, pkgs, cores, speed);
    }

    /* 0xC1 MiscellaneousInfo:
     * 4 binary bytes: uint32 total_ram_mb (the "RAM" key in Server Monitor)
     * 2 packed strings: [Model, Serial]
     * THIS IS WHERE SERVER MONITOR READS TOTAL MEMORY! */
    {
        popen_field("/bin/esxcli hardware platform get",
                    "Serial Number", cache_serial, CACHE_SIZE);
        popen_field("/bin/esxcli hardware platform get",
                    "Product Name", cache_model, CACHE_SIZE);

        /* Get total physical memory in MB */
        popen_field("/bin/esxcli hardware memory get",
                    "Physical Memory", buf, sizeof(buf));
        long long total_bytes = atoll(buf);
        /* Round to nearest GB boundary — ESXi reports slightly less
         * than installed RAM due to kernel reservations */
        cache_total_ram_mb = (uint32_t)(
            ((total_bytes + (512LL * 1024 * 1024)) /
             (1024LL * 1024 * 1024)) * 1024);

        fprintf(stderr, "bmc: misc: model=%s serial=%s ram=%u MB\n",
                cache_model, cache_serial, cache_total_ram_mb);
    }

    /* 0xC2 Memory: populate per-DIMM structs from SMBIOS Type 17.
     *
     * ESXi smbiosDump format for Type 17 (Memory Device):
     *   Header:  "Type 17" in the section header line
     *   Fields:  "   Device Locator: DIMM_A1"
     *            "   Size: 16384 MB"
     *            "   Memory Type: 0x18 (DDR3)"
     *            "   Speed: 1066"
     *
     * We ONLY parse sections containing "Type 17" in the header.
     * Fields use exact names to avoid matching other SMBIOS types.
     * Hex prefixes like "0x18 (DDR3)" are cleaned to "DDR3".
     */
    {
        cache_dimm_count = 0;

        /*
         * Parse smbiosDump output. Actual ESXi format:
         *
         *   Memory Device: #7            ← section header (2-space indent)
         *     Location: "A1"             ← field (4-space indent)
         *     Bank: "CPUA"
         *     Type: 0x18 (DDR3)
         *     Size: 16 GB
         *     Speed: 1066 MHz
         *   Type 130 Record: #8          ← next section (ends Memory Device)
         *   64bit-Memory Error Info: #9  ← another section type
         *
         * Section headers: 2-space indent, name followed by ": #N"
         * Fields: 4-space indent, "Key: Value"
         * No blank lines between sections.
         * Empty slots have "Size: No Memory Installed" and "Type: 0x02 (Unknown)"
         */
        FILE *fp = popen("smbiosDump 2>/dev/null", "r");
        if (fp) {
            char line[512];
            int in_memdev = 0;
            char loc[64] = "", spd[32] = "", typ[32] = "";
            int size_mb_val = 0;

            while (fgets(line, sizeof(line), fp)) {
                /* Trim trailing whitespace/newlines */
                int ll = strlen(line);
                while (ll > 0 && (line[ll-1]=='\n'||line[ll-1]=='\r'
                                  ||line[ll-1]==' '))
                    line[--ll] = '\0';

                /*
                 * Section header detection:
                 * Lines with exactly 2-space indent followed by a word
                 * and ": #" are section headers.
                 * e.g. "  Memory Device: #7"
                 *      "  Type 130 Record: #8"
                 *      "  64bit-Memory Error Info: #9"
                 */
                if (ll > 4 && line[0] == ' ' && line[1] == ' ' &&
                    line[2] != ' ' && strstr(line, ": #")) {

                    /* Save previous Memory Device entry if valid */
                    if (in_memdev && size_mb_val > 0 &&
                        cache_dimm_count < MAX_DIMMS) {
                        struct dimm_info *d = &cache_dimms[cache_dimm_count];
                        d->config_type = 0x00;
                        d->ecc_type = 0x00;
                        d->size_mb = (uint32_t)size_mb_val;
                        strncpy(d->slot_name, loc[0] ? loc : "DIMM",
                                sizeof(d->slot_name) - 1);
                        strncpy(d->speed, spd[0] ? spd : "Unknown",
                                sizeof(d->speed) - 1);
                        strncpy(d->type, typ[0] ? typ : "DDR3 ECC",
                                sizeof(d->type) - 1);
                        cache_dimm_count++;
                    }

                    /* Check if this new section is a Memory Device */
                    in_memdev = (strstr(line, "Memory Device") != NULL &&
                                 strstr(line, "Mapped") == NULL) ? 1 : 0;
                    loc[0] = spd[0] = typ[0] = '\0';
                    size_mb_val = 0;
                    continue;
                }

                if (!in_memdev) continue;

                /* Field lines: 4-space indent, "Key: Value" */
                if (line[0] != ' ' || line[1] != ' ' ||
                    line[2] != ' ' || line[3] != ' ')
                    continue;

                char *field = line + 4;  /* skip 4-space indent */
                char *p;

                /* Location: "A1" — strip quotes */
                if (strncmp(field, "Location:", 9) == 0) {
                    p = field + 9;
                    while (*p == ' ') p++;
                    /* Strip surrounding quotes */
                    if (*p == '"') p++;
                    int sl = strlen(p);
                    if (sl > 0 && p[sl-1] == '"') p[--sl] = '\0';
                    strncpy(loc, p, sizeof(loc) - 1);
                }
                /* Size: 16 GB  or  Size: 2 GB  or  Size: No Memory Installed */
                else if (strncmp(field, "Size:", 5) == 0 &&
                         !strstr(field, "Max")) {
                    p = field + 5;
                    while (*p == ' ') p++;
                    if (strstr(p, "No Memory") || strstr(p, "Not")) {
                        size_mb_val = 0;
                    } else {
                        size_mb_val = atoi(p);
                        if (strstr(p, "GB"))
                            size_mb_val *= 1024;
                        else if (strstr(p, "KB"))
                            size_mb_val /= 1024;
                        /* "MB" = already in MB, no conversion */
                    }
                }
                /* Type: 0x18 (DDR3) — extract description from parens */
                else if (strncmp(field, "Type:", 5) == 0 &&
                         !strstr(field, "Detail")) {
                    p = field + 5;
                    while (*p == ' ') p++;
                    char *paren = strchr(p, '(');
                    if (paren) {
                        paren++;
                        char *end = strchr(paren, ')');
                        if (end) {
                            int tl = end - paren;
                            if (tl > (int)sizeof(typ) - 1)
                                tl = sizeof(typ) - 1;
                            strncpy(typ, paren, tl);
                            typ[tl] = '\0';
                            /* Skip "Unknown" — empty slots have this */
                            if (strcmp(typ, "Unknown") == 0)
                                typ[0] = '\0';
                        }
                    }
                }
                /* Speed: 1066 MHz */
                else if (strncmp(field, "Speed:", 6) == 0) {
                    p = field + 6;
                    while (*p == ' ') p++;
                    int mhz = atoi(p);
                    if (mhz > 0)
                        snprintf(spd, sizeof(spd), "%d MHz", mhz);
                }
            }

            /* Save last entry */
            if (in_memdev && size_mb_val > 0 &&
                cache_dimm_count < MAX_DIMMS) {
                struct dimm_info *d = &cache_dimms[cache_dimm_count];
                d->config_type = 0x00;
                d->ecc_type = 0x00;
                d->size_mb = (uint32_t)size_mb_val;
                strncpy(d->slot_name, loc[0] ? loc : "DIMM",
                        sizeof(d->slot_name) - 1);
                strncpy(d->speed, spd[0] ? spd : "Unknown",
                        sizeof(d->speed) - 1);
                strncpy(d->type, typ[0] ? typ : "DDR3",
                        sizeof(d->type) - 1);
                cache_dimm_count++;
            }
            pclose(fp);
        }

        /* Log what we found */
        uint64_t total_mb = 0;
        for (int i = 0; i < cache_dimm_count; i++)
            total_mb += cache_dimms[i].size_mb;
        fprintf(stderr, "bmc: memory: %llu MB total, %d DIMMs\n",
                (unsigned long long)total_mb, cache_dimm_count);
        for (int i = 0; i < cache_dimm_count; i++) {
            fprintf(stderr, "bmc:   [%d] %s: %u MB, %s, %s\n",
                    i, cache_dimms[i].slot_name,
                    cache_dimms[i].size_mb,
                    cache_dimms[i].speed, cache_dimms[i].type);
        }
    }

    /* Drives (per-device, cached at startup only) */
    collect_drives();
    fprintf(stderr, "bmc: drives: %d devices\n", cache_drive_count);
    for (int i = 0; i < cache_drive_count; i++)
        fprintf(stderr, "bmc:   [%d] %s\n", i, cache_drive_entries[i]);

    /* NICs */
    collect_nics();
    collect_net_dynamic();
    fprintf(stderr, "bmc: nics: %d devices\n", cache_nic_count);
    for (int i = 0; i < cache_nic_count; i++)
        fprintf(stderr, "bmc:   [%d] %s %s %s\n", i,
                cache_nic_static[i].name,
                cache_nic_static[i].mac,
                cache_net_dynamic[i].ipv4);

    /* Record initial state for change detection */
    drive_count = count_disk_devices();
    struct stat st;
    if (stat("/etc/vmware/esx.conf", &st) == 0)
        network_mtime = st.st_mtime;

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

    /* Check for drive changes (count devices — immune to I/O noise) */
    int cur_count = count_disk_devices();
    if (cur_count >= 0 && cur_count != drive_count) {
        fprintf(stderr, "bmc: drive change detected (%d -> %d)\n",
                drive_count, cur_count);
        collect_drives();
        drive_count = cur_count;
    }

    /* Check for network config changes (stat mtime) */
    struct stat st;
    if (stat("/etc/vmware/esx.conf", &st) == 0 && st.st_mtime != network_mtime) {
        fprintf(stderr, "bmc: network change detected\n");
        collect_net_dynamic();
        network_mtime = st.st_mtime;
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
