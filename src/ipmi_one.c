/*
 * ipmi_one.c - Write a single IPMI param via /dev/ipmi0
 * Usage: ipmi_one <param_hex> <string>
 * e.g.:  ipmi_one C7       (writes uptime=1)
 *        ipmi_one 02 TEST  (writes "TEST" to hostname)
 *        ipmi_one none     (just opens /dev/ipmi0, no write)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>

#define IPMI_BMC_CHANNEL        0xf
#define IPMI_SYSTEM_INTERFACE_ADDR_TYPE 0x0c

struct ipmi_system_interface_addr {
    int32_t addr_type; int16_t channel; uint8_t lun;
};
struct ipmi_msg {
    uint8_t netfn; uint8_t cmd; uint16_t data_len; uint8_t *data;
};
struct ipmi_req {
    uint8_t *addr; uint32_t addr_len; int64_t msgid; struct ipmi_msg msg;
};
struct ipmi_recv {
    int32_t recv_type; uint8_t *addr; uint32_t addr_len;
    int64_t msgid; struct ipmi_msg msg;
};
#define IPMICTL_SEND_COMMAND       _IOR('i', 13, struct ipmi_req)
#define IPMICTL_RECEIVE_MSG_TRUNC  _IOWR('i', 11, struct ipmi_recv)

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: %s <param_hex|none|open> [string]\n", argv[0]);
        return 1;
    }

    printf("Opening /dev/ipmi0...\n");
    int fd = open("/dev/ipmi0", O_RDWR);
    if (fd < 0) { printf("Failed: %s\n", strerror(errno)); return 1; }
    printf("Opened (fd=%d)\n", fd);

    if (strcmp(argv[1], "open") == 0) {
        printf("Just opened, no write. Waiting 30 sec...\n");
        sleep(30);
        close(fd);
        printf("Closed.\n");
        return 0;
    }

    if (strcmp(argv[1], "none") == 0) {
        printf("No write. Closing immediately.\n");
        close(fd);
        return 0;
    }

    uint8_t param = (uint8_t)strtol(argv[1], NULL, 16);
    printf("Writing to param 0x%02X...\n", param);

    uint8_t buf[36];
    int blen;

    if (argc >= 3) {
        /* String write */
        int slen = strlen(argv[2]);
        buf[0] = param;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = (uint8_t)(slen & 0xFF);
        buf[4] = 0x00;
        memcpy(buf + 5, argv[2], slen);
        blen = 5 + slen;
    } else {
        /* Binary write (uptime=1) */
        uint32_t val = 1;
        buf[0] = param;
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = 0x04;
        buf[4] = 0x00;
        memcpy(buf + 5, &val, 4);
        blen = 9;
    }

    struct ipmi_system_interface_addr addr = {
        .addr_type = IPMI_SYSTEM_INTERFACE_ADDR_TYPE,
        .channel = IPMI_BMC_CHANNEL, .lun = 0
    };
    struct ipmi_req req = {
        .addr = (uint8_t *)&addr, .addr_len = sizeof(addr),
        .msgid = 1,
        .msg = { .netfn = 0x36, .cmd = 0x01,
                 .data = buf, .data_len = blen }
    };

    if (ioctl(fd, IPMICTL_SEND_COMMAND, &req) < 0) {
        printf("Send failed: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    poll(&pfd, 1, 5000);

    uint8_t resp[32];
    struct ipmi_system_interface_addr raddr;
    struct ipmi_recv recv = {
        .addr = (uint8_t *)&raddr, .addr_len = sizeof(raddr),
        .msg = { .data = resp, .data_len = sizeof(resp) }
    };
    ioctl(fd, IPMICTL_RECEIVE_MSG_TRUNC, &recv);

    printf("Response: cc=0x%02X\n", recv.msg.data_len > 0 ? resp[0] : 0xFF);

    close(fd);
    printf("Done.\n");
    return 0;
}
