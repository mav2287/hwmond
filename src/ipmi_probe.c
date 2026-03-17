/*
 * ipmi_probe.c - Probe exactly what BMC data format works
 * Try every combination to find what persists
 */
#define _GNU_SOURCE
#include <stdio.h>
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

static int fd;
static int64_t seq = 1;

static int cmd(uint8_t netfn, uint8_t c, uint8_t *d, int dl)
{
    struct ipmi_system_interface_addr a = {
        .addr_type=IPMI_SYSTEM_INTERFACE_ADDR_TYPE,
        .channel=IPMI_BMC_CHANNEL };
    struct ipmi_req r = { .addr=(uint8_t*)&a, .addr_len=sizeof(a),
        .msgid=seq++, .msg={.netfn=netfn,.cmd=c,.data=d,.data_len=dl}};
    if (ioctl(fd, IPMICTL_SEND_COMMAND, &r)<0) return -1;
    struct pollfd p = {.fd=fd,.events=POLLIN};
    if (poll(&p,1,5000)<=0) return -1;
    uint8_t rb[64]; struct ipmi_system_interface_addr ra;
    struct ipmi_recv rv = {.addr=(uint8_t*)&ra,.addr_len=sizeof(ra),
        .msg={.data=rb,.data_len=sizeof(rb)}};
    if (ioctl(fd, IPMICTL_RECEIVE_MSG_TRUNC, &rv)<0) return -1;
    return (rv.msg.data_len>0 && rb[0]==0) ? 0 : -1;
}

int main(void)
{
    fd = open("/dev/ipmi0", O_RDWR);
    if (fd<0) { printf("no ipmi\n"); return 1; }

    printf("=== BMC Write Format Probe ===\n\n");
    printf("Writing to param 0x02 (SystemName) with different formats.\n");
    printf("Check from Mac after each: raw 0x36 0x02 0x00 0x02 0x00 0x00\n\n");

    /* Test A: raw data, no header */
    printf("A: [param, sel=0, 'TEST']...");
    { uint8_t d[] = {0x02, 0x00, 'T','E','S','T'};
      printf(" %s\n", cmd(0x36,0x01,d,6)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test B: with encoding=0x00, length */
    printf("B: [param, sel=0, enc=0, len=4, 'TEST']...");
    { uint8_t d[] = {0x02, 0x00, 0x00, 0x04, 'T','E','S','T'};
      printf(" %s\n", cmd(0x36,0x01,d,8)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test C: with encoding=0x01, length */
    printf("C: [param, sel=0, enc=1, len=4, 'TEST']...");
    { uint8_t d[] = {0x02, 0x00, 0x01, 0x04, 'T','E','S','T'};
      printf(" %s\n", cmd(0x36,0x01,d,8)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test D: IPMI standard format (encoding in high nibble) */
    printf("D: [param, sel=0, 0x04(len), 'TEST']...");
    { uint8_t d[] = {0x02, 0x00, 0x04, 'T','E','S','T'};
      printf(" %s\n", cmd(0x36,0x01,d,7)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test E: just 4 bytes like uptime */
    printf("E: [param, sel=0, 4 bytes binary 0x41424344]...");
    { uint8_t d[] = {0x02, 0x00, 0x41, 0x42, 0x43, 0x44};
      printf(" %s\n", cmd(0x36,0x01,d,6)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test F: no set_selector, just param + data */
    printf("F: [param, 'TEST']...");
    { uint8_t d[] = {0x02, 'T','E','S','T'};
      printf(" %s\n", cmd(0x36,0x01,d,5)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test G: 1 byte only */
    printf("G: [param, sel=0, 'X']...");
    { uint8_t d[] = {0x02, 0x00, 'X'};
      printf(" %s\n", cmd(0x36,0x01,d,3)==0?"OK":"FAIL"); }
    sleep(1);

    /* Test H: exactly like uptime format but on param 0x02 */
    printf("H: [param, sel=0, enc=0, len=4, 0x54455354]...");
    { uint8_t d[] = {0x02, 0x00, 0x00, 0x04, 0x54, 0x45, 0x53, 0x54};
      printf(" %s\n", cmd(0x36,0x01,d,8)==0?"OK":"FAIL"); }

    printf("\nNow check from Mac which format left data:\n");
    printf("  ipmitool raw 0x36 0x02 0x00 0x02 0x00 0x00\n");
    printf("  Look for non-zero bytes after '11 xx'\n");

    close(fd);
    return 0;
}
