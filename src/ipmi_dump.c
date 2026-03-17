/*
 * ipmi_dump.c - Read ALL Apple BMC data
 *
 * Reads every Apple OEM parameter from the BMC and displays it.
 * Tries multiple Get formats to find what works.
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

static int ipmi_fd;
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

static const char *param_name(uint8_t p)
{
    switch (p) {
    case 0x01: return "FirmwareVersion";
    case 0x02: return "SystemName";
    case 0x03: return "PrimaryOS";
    case 0x04: return "CurrentOS";
    case 0xC0: return "ProcessorInfo";
    case 0xC1: return "MiscInfo";
    case 0xC2: return "MemoryInfo";
    case 0xC3: return "DriveStaticInfo";
    case 0xC4: return "NetworkStaticInfo";
    case 0xC5: return "DriveDynamicInfo";
    case 0xC6: return "NetworkDynamicInfo";
    case 0xC7: return "UpTime";
    case 0xC8: return "Unknown_C8";
    case 0xC9: return "MemoryDynamicInfo";
    case 0xCA: return "PowerSourceInfo";
    case 0xCB: return "ComputerName";
    default:   return "Unknown";
    }
}

static void try_read(uint8_t param, uint8_t get_cmd,
                     uint8_t *req_data, int req_len, const char *fmt_desc)
{
    uint8_t resp[64];
    uint16_t rlen = sizeof(resp);

    if (ipmi_cmd(0x36, get_cmd, req_data, req_len, resp, &rlen) != 0) {
        printf("    [%s] send failed\n", fmt_desc);
        return;
    }

    if (rlen == 0) {
        printf("    [%s] empty response\n", fmt_desc);
        return;
    }

    if (rlen == 1 && resp[0] != 0x00) {
        printf("    [%s] cc=0x%02X\n", fmt_desc, resp[0]);
        return;
    }

    printf("    [%s] (%d bytes) ", fmt_desc, rlen);

    /* Print raw hex */
    printf("hex:");
    for (int i = 0; i < rlen && i < 40; i++)
        printf(" %02x", resp[i]);

    /* Try to print as string (skip completion code if present) */
    int start = (resp[0] == 0x00) ? 1 : 0;
    int has_printable = 0;
    for (int i = start; i < rlen; i++) {
        if (resp[i] >= 0x20 && resp[i] < 0x7f) has_printable = 1;
    }

    if (has_printable) {
        printf("\n         str: \"");
        for (int i = start; i < rlen; i++) {
            if (resp[i] >= 0x20 && resp[i] < 0x7f)
                printf("%c", resp[i]);
            else if (resp[i] == 0x00 && i < rlen - 1)
                printf("|");
        }
        printf("\"");
    }

    /* Special: if this looks like uptime (4 bytes after cc) */
    if (param == 0xC7 && rlen >= 5 && resp[0] == 0x00) {
        uint32_t secs = resp[1] | (resp[2] << 8) |
                        (resp[3] << 16) | (resp[4] << 24);
        printf("\n         uptime: %u sec (%u hours)", secs, secs / 3600);
    }

    /* Special: if this is ProcessorInfo (12 bytes after cc) */
    if (param == 0xC0 && rlen >= 13 && resp[0] == 0x00) {
        uint32_t v1, v2, v3;
        memcpy(&v1, resp + 1, 4);
        memcpy(&v2, resp + 5, 4);
        memcpy(&v3, resp + 9, 4);
        printf("\n         cpu: %u pkg, %u cores, %u MHz", v1, v2, v3);
    }

    printf("\n");
}

int main(void)
{
    printf("=== Apple BMC Data Dump ===\n\n");

    ipmi_fd = open("/dev/ipmi0", O_RDWR);
    if (ipmi_fd < 0) {
        printf("Cannot open /dev/ipmi0: %s\n", strerror(errno));
        return 1;
    }

    /* BMC identity */
    {
        uint8_t resp[32];
        uint16_t len = sizeof(resp);
        if (ipmi_cmd(0x06, 0x01, NULL, 0, resp, &len) == 0 &&
            len > 5 && resp[0] == 0) {
            printf("BMC: Device ID=0x%02x, FW=%d.%02d, IPMI=%d.%d\n\n",
                   resp[1], resp[3] & 0x7f, resp[4],
                   resp[5] & 0xf, (resp[5] >> 4) & 0xf);
        }
    }

    /* Read all Apple params using every Get format we can think of */
    uint8_t params[] = {
        0x01, 0x02, 0x03, 0x04,
        0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5,
        0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB
    };

    for (int i = 0; i < (int)(sizeof(params)/sizeof(params[0])); i++) {
        uint8_t p = params[i];
        printf("Param 0x%02X (%s):\n", p, param_name(p));

        /* Format A: Cmd=0x02, data=[param] */
        { uint8_t d[] = {p};
          try_read(p, 0x02, d, 1, "cmd2 [p]"); }

        /* Format B: Cmd=0x02, data=[param, 0x00] */
        { uint8_t d[] = {p, 0x00};
          try_read(p, 0x02, d, 2, "cmd2 [p,0]"); }

        /* Format C: Cmd=0x02, data=[param, 0x00, 0x00] */
        { uint8_t d[] = {p, 0x00, 0x00};
          try_read(p, 0x02, d, 3, "cmd2 [p,0,0]"); }

        /* Format D: Cmd=0x02, data=[0x00, param, 0x00, 0x00] (IPMI standard) */
        { uint8_t d[] = {0x00, p, 0x00, 0x00};
          try_read(p, 0x02, d, 4, "cmd2 [0,p,0,0]"); }

        /* Format E: Cmd=0x01, data=[param, 0x00, 0x00] (use Set cmd as Get) */
        { uint8_t d[] = {p, 0x00, 0x00};
          try_read(p, 0x01, d, 3, "cmd1 [p,0,0]"); }

        /* Format F: Cmd=0x05, data=[param] (from command scan, 0x05 returned ok) */
        { uint8_t d[] = {p};
          try_read(p, 0x05, d, 1, "cmd5 [p]"); }

        /* Format G: Cmd=0x0A, data=[param] (from scan, 0x0A returned ok) */
        { uint8_t d[] = {p};
          try_read(p, 0x0A, d, 1, "cmdA [p]"); }

        printf("\n");
    }

    close(ipmi_fd);
    printf("Done.\n");
    return 0;
}
