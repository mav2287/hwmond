/*
 * ipmi_test.c - Write ALL Apple BMC parameters (v4)
 *
 * Writes every parameter the Apple BMC supports using real
 * system data from ESXi. Strings use encoding=0x01 (ASCII),
 * binary data uses encoding=0x00.
 *
 * NetFn=0x36, Cmd=0x01
 * Format: [param_id, set_selector, encoding, data_length, ...data...]
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
#include <poll.h>

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

static int ipmi_fd = -1;
static int64_t msg_seq = 1;

static int ipmi_cmd(uint8_t netfn, uint8_t cmd,
                    uint8_t *data, uint16_t data_len,
                    uint8_t *resp, uint16_t *resp_len)
{
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
    if (ioctl(ipmi_fd, IPMICTL_SEND_COMMAND, &req) < 0) return -1;
    struct pollfd pfd = { .fd = ipmi_fd, .events = POLLIN };
    if (poll(&pfd, 1, 5000) <= 0) return -1;
    struct ipmi_system_interface_addr raddr;
    struct ipmi_recv recv = {
        .addr = (uint8_t *)&raddr, .addr_len = sizeof(raddr),
        .msg = { .data = resp, .data_len = *resp_len }
    };
    if (ioctl(ipmi_fd, IPMICTL_RECEIVE_MSG_TRUNC, &recv) < 0) return -1;
    *resp_len = recv.msg.data_len;
    return 0;
}

static int apple_set(uint8_t param, uint8_t encoding,
                     uint8_t *data, int data_len)
{
    uint8_t buf[64];
    buf[0] = param;
    buf[1] = 0x00;          /* set_selector */
    buf[2] = encoding;
    buf[3] = (uint8_t)data_len;
    if (data_len > 0 && data_len <= 58)
        memcpy(buf + 4, data, data_len);
    uint8_t resp[32];
    uint16_t rlen = sizeof(resp);
    if (ipmi_cmd(0x36, 0x01, buf, 4 + data_len, resp, &rlen) != 0) return -1;
    return (rlen > 0 && resp[0] == 0x00) ? 0 : -(int)resp[0];
}

static int set_string(uint8_t param, const char *str)
{
    return apple_set(param, 0x01, (uint8_t *)str, strlen(str));
}

static int set_binary(uint8_t param, uint8_t *data, int len)
{
    return apple_set(param, 0x00, data, len);
}

static void report(const char *name, uint8_t param, int ret)
{
    printf("  [0x%02X] %-30s %s", param, name,
           ret == 0 ? "OK" : "FAIL");
    if (ret != 0) printf(" (cc=0x%02x)", -ret);
    printf("\n");
}

/* Helper to run popen and get first line */
static int popen_line(const char *cmd, char *buf, int bufsz)
{
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    buf[0] = '\0';
    fgets(buf, bufsz, fp);
    pclose(fp);
    /* Strip trailing newline */
    int len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return len;
}

/* Helper to extract value after colon from esxcli output */
static int popen_field(const char *cmd, const char *field, char *buf, int bufsz)
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
            while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r'))
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

int main(void)
{
    printf("=== Apple BMC Full Data Population ===\n\n");

    ipmi_fd = open("/dev/ipmi0", O_RDWR);
    if (ipmi_fd < 0) {
        printf("Cannot open /dev/ipmi0: %s\n", strerror(errno));
        return 1;
    }

    char buf[256], buf2[256];
    int ret;

    /* ============================================ */
    printf("--- String Parameters ---\n");
    /* ============================================ */

    /* 0x01: Firmware Version (BootROM) */
    popen_field("vsish -e get /hardware/bios/biosInfo", "BIOS Version", buf, sizeof(buf));
    /* Trim leading spaces */
    char *fw = buf; while (*fw == ' ') fw++;
    report("FirmwareVersion", 0x01, set_string(0x01, fw));
    printf("       → \"%s\"\n", fw);

    /* 0x02: System Name (hostname) */
    popen_field("/bin/esxcli system hostname get", "Host Name", buf, sizeof(buf));
    report("SystemName", 0x02, set_string(0x02, buf));
    printf("       → \"%s\"\n", buf);

    /* 0x03: Primary OS */
    {
        char prod[64], ver[64], build[64];
        popen_field("/bin/esxcli system version get", "Product", prod, sizeof(prod));
        popen_field("/bin/esxcli system version get", "Version", ver, sizeof(ver));
        popen_field("/bin/esxcli system version get", "Build", build, sizeof(build));
        snprintf(buf, sizeof(buf), "%s %s", prod, ver);
        report("PrimaryOS", 0x03, set_string(0x03, buf));
        printf("       → \"%s\"\n", buf);
    }

    /* 0x04: Current OS (build detail) */
    {
        char ver[64], build[64], update[64], patch[64];
        popen_field("/bin/esxcli system version get", "Version", ver, sizeof(ver));
        popen_field("/bin/esxcli system version get", "Build", build, sizeof(build));
        popen_field("/bin/esxcli system version get", "Update", update, sizeof(update));
        popen_field("/bin/esxcli system version get", "Patch", patch, sizeof(patch));
        snprintf(buf, sizeof(buf), "%s Build %s Update %s Patch %s",
                 ver, build, update, patch);
        report("CurrentOS", 0x04, set_string(0x04, buf));
        printf("       → \"%s\"\n", buf);
    }

    /* 0xCB: Computer Name */
    popen_field("/bin/esxcli system hostname get", "Fully Qualified Domain Name", buf, sizeof(buf));
    report("ComputerName", 0xCB, set_string(0xCB, buf));
    printf("       → \"%s\"\n", buf);

    /* ============================================ */
    printf("\n--- Binary Parameters ---\n");
    /* ============================================ */

    /* 0xC0: Processor Info (12 bytes: 3x uint32) */
    {
        char model[128];
        int packages = 0, cores = 0, speed_mhz = 0;

        popen_line("vsish -e get /hardware/cpu/cpuModelName 2>/dev/null", model, sizeof(model));
        /* Trim */
        char *m = model; while (*m == ' ') m++;
        popen_field("/bin/esxcli hardware cpu global get", "CPU Packages", buf, sizeof(buf));
        packages = atoi(buf);
        popen_field("/bin/esxcli hardware cpu global get", "CPU Cores", buf, sizeof(buf));
        cores = atoi(buf);
        popen_field("/bin/esxcli hardware cpu list", "Core Speed", buf, sizeof(buf));
        long speed_hz = atol(buf);
        speed_mhz = (int)(speed_hz / 1000000);

        uint32_t pdata[3] = { (uint32_t)packages, (uint32_t)cores,
                              (uint32_t)speed_mhz };
        report("ProcessorInfo", 0xC0, set_binary(0xC0, (uint8_t *)pdata, 12));
        printf("       → %s, %d pkg, %d cores, %d MHz\n",
               m, packages, cores, speed_mhz);
    }

    /* 0xC1: Miscellaneous Info (serial + model) */
    {
        char serial[64], model[64];
        popen_field("/bin/esxcli hardware platform get", "Serial Number", serial, sizeof(serial));
        popen_field("/bin/esxcli hardware platform get", "Product Name", model, sizeof(model));
        /* Pack as: serial\0model\0 */
        int slen = strlen(serial);
        int mlen = strlen(model);
        uint8_t mdata[128];
        memcpy(mdata, serial, slen + 1);
        memcpy(mdata + slen + 1, model, mlen + 1);
        report("MiscInfo", 0xC1, set_binary(0xC1, mdata, slen + 1 + mlen + 1));
        printf("       → Serial: %s, Model: %s\n", serial, model);
    }

    /* 0xC2: Memory Info */
    {
        char mem[64];
        popen_field("/bin/esxcli hardware memory get", "Physical Memory", buf, sizeof(buf));
        long bytes = atol(buf);
        int gb = (int)(bytes / (1024L * 1024L * 1024L));
        snprintf(mem, sizeof(mem), "%d GB DDR3", gb);
        /* Send as string for now — exact binary format TBD */
        report("MemoryInfo", 0xC2, set_string(0xC2, mem));
        printf("       → %s\n", mem);
    }

    /* 0xC3: Drive Static Info (per-drive model/size) */
    {
        /* Collect drive info from esxcli */
        FILE *fp = popen("/bin/esxcli storage core device list 2>/dev/null", "r");
        if (fp) {
            char line[512];
            char model[64] = "", size_str[64] = "";
            int drive_idx = 0;
            char all_drives[512] = "";
            int all_len = 0;

            while (fgets(line, sizeof(line), fp)) {
                char *p;
                if ((p = strstr(line, "Model:")) != NULL) {
                    p += 6; while (*p == ' ') p++;
                    int l = strlen(p);
                    while (l > 0 && (p[l-1] == '\n' || p[l-1] == ' ')) p[--l] = '\0';
                    strncpy(model, p, sizeof(model) - 1);
                } else if ((p = strstr(line, "Size:")) != NULL &&
                           strstr(line, "Queue") == NULL &&
                           strstr(line, "Cache") == NULL) {
                    p += 5; while (*p == ' ') p++;
                    int l = strlen(p);
                    while (l > 0 && (p[l-1] == '\n' || p[l-1] == ' ')) p[--l] = '\0';
                    strncpy(size_str, p, sizeof(size_str) - 1);

                    if (model[0] && atol(size_str) > 0) {
                        long mb = atol(size_str);
                        int gb = (int)(mb / 1024);
                        char entry[128];
                        snprintf(entry, sizeof(entry), "%s (%dGB)", model, gb > 0 ? gb : (int)mb);
                        if (all_len > 0) {
                            all_drives[all_len++] = ',';
                        }
                        int elen = strlen(entry);
                        if (all_len + elen < (int)sizeof(all_drives) - 1) {
                            memcpy(all_drives + all_len, entry, elen);
                            all_len += elen;
                        }
                        drive_idx++;
                    }
                    model[0] = '\0';
                }
            }
            pclose(fp);
            all_drives[all_len] = '\0';

            if (all_len > 0) {
                /* Truncate if too long for IPMI (max ~58 bytes per block) */
                if (all_len > 56) all_drives[56] = '\0';
                report("DriveStaticInfo", 0xC3, set_string(0xC3, all_drives));
                printf("       → %s\n", all_drives);
            }
        }
    }

    /* 0xC4: Network Static Info (per-NIC MAC + model) */
    {
        char mac0[32] = "", mac1[32] = "";
        popen_field("/bin/esxcli network nic list", "00:24:36:f4:06:4e", buf, sizeof(buf));
        /* Just get MACs directly */
        FILE *fp = popen("/bin/esxcli network nic list 2>/dev/null", "r");
        if (fp) {
            char line[512];
            char nics[256] = "";
            int nlen = 0;
            while (fgets(line, sizeof(line), fp)) {
                /* Parse lines like: vmnic0  0000:05:00.0  ne1000  Up  Up  1000  Full  00:24:36:f4:06:4e ... */
                char name[16], pci[16], driver[16], admin[8], link[8], mac[24];
                int speed;
                if (sscanf(line, "%15s %15s %15s %7s %7s %d %*s %23s",
                           name, pci, driver, admin, link, &speed, mac) >= 7) {
                    if (strncmp(name, "vmnic", 5) == 0) {
                        char entry[128];
                        snprintf(entry, sizeof(entry), "%s:%s:%s:%dMbps",
                                 name, mac, link, speed);
                        if (nlen > 0) nics[nlen++] = ',';
                        int elen = strlen(entry);
                        if (nlen + elen < (int)sizeof(nics) - 1) {
                            memcpy(nics + nlen, entry, elen);
                            nlen += elen;
                        }
                    }
                }
            }
            pclose(fp);
            nics[nlen] = '\0';

            if (nlen > 0) {
                if (nlen > 56) nics[56] = '\0';
                report("NetworkStaticInfo", 0xC4, set_string(0xC4, nics));
                printf("       → %s\n", nics);
            }
        }
    }

    /* 0xC7: Uptime */
    {
        /* Get uptime from vsish */
        popen_field("vsish -e get /sched/globalStats/cpuStats", "uptime", buf, sizeof(buf));
        /* uptime is in usec, convert to seconds */
        long long usec = atoll(buf);
        uint32_t secs = (uint32_t)(usec / 1000000);
        report("UpTime", 0xC7, set_binary(0xC7, (uint8_t *)&secs, 4));
        printf("       → %u seconds (%u hours)\n", secs, secs / 3600);
    }

    /* 0xC6: Network Dynamic Info (IPs) */
    {
        char ip[64] = "";
        popen_field("/bin/esxcli network ip interface ipv4 get", "192.", buf, sizeof(buf));
        /* Parse the IP line */
        FILE *fp = popen("/bin/esxcli network ip interface ipv4 get 2>/dev/null", "r");
        if (fp) {
            char line[512];
            char net_info[128] = "";
            while (fgets(line, sizeof(line), fp)) {
                char iface[16], ipaddr[32], mask[32];
                if (sscanf(line, "%15s %31s %31s", iface, ipaddr, mask) >= 3) {
                    if (strncmp(iface, "vmk", 3) == 0) {
                        snprintf(net_info, sizeof(net_info), "%s:%s/%s",
                                 iface, ipaddr, mask);
                    }
                }
            }
            pclose(fp);
            if (net_info[0]) {
                report("NetworkDynamicInfo", 0xC6, set_string(0xC6, net_info));
                printf("       → %s\n", net_info);
            }
        }
    }

    printf("\n=== Summary ===\n");
    printf("All writes that returned OK are stored in the BMC.\n");
    printf("Connect Server Monitor to LOM IP to verify.\n");

    close(ipmi_fd);
    printf("\nDone.\n");
    return 0;
}
