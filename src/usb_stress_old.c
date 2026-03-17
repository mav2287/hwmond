/*
 * usb_stress_old.c — Stress test using the OLD BUGGY ioctl
 *
 * Uses REAPURBNDELAY (0x4008550D) — the Linux ioctl that falls through
 * to vmkusb's buggy udev_reapurb_sub. This SHOULD crash to prove the
 * new ioctl (0xC0105512) is using a different code path.
 *
 * WARNING: This WILL likely PSOD the host. Only run on a test system!
 *
 * Build: zig cc -target x86_64-linux-gnu.2.12 -O2 -o build/usb_stress_old src/usb_stress_old.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>

struct usbdevfs_urb {
    unsigned char type;
    unsigned char endpoint;
    int           status;
    unsigned int  flags;
    void         *buffer;
    int           buffer_length;
    int           actual_length;
    int           start_frame;
    int           number_of_packets;
    int           error_count;
    unsigned int  signr;
    void         *usercontext;
};

#define USBDEVFS_URB_TYPE_BULK 3

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#define USBDEVFS_SUBMITURB        _IOR('U', 10, struct usbdevfs_urb)
#define USBDEVFS_REAPURBNDELAY    _IOW('U', 13, void *)
#define USBDEVFS_CLAIMINTERFACE   _IOR('U', 15, unsigned int)
#pragma GCC diagnostic pop

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <device> [iterations]\n", argv[0]);
        return 1;
    }

    const char *devpath = argv[1];
    int iterations = argc > 2 ? atoi(argv[2]) : 100000;
    uint8_t data[32];
    memset(data, 0, sizeof(data));

    printf("USB stress test: OLD BUGGY IOCTL (0x4008550D → udev_reapurb_sub)\n");
    printf("Device: %s, iterations: %d\n", devpath, iterations);
    printf("WARNING: This WILL likely PSOD the host!\n");
    printf("Starting in 5 seconds... Ctrl+C to cancel.\n");
    sleep(5);

    int fd = open(devpath, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    unsigned int iface = 0;
    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);

    int success = 0;
    for (int i = 0; i < iterations; i++) {
        struct usbdevfs_urb urb;
        memset(&urb, 0, sizeof(urb));
        urb.type = USBDEVFS_URB_TYPE_BULK;
        urb.endpoint = 0x01;
        urb.buffer = data;
        urb.buffer_length = 32;

        if (ioctl(fd, USBDEVFS_SUBMITURB, &urb) < 0) continue;

        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        if (poll(&pfd, 1, 5000) <= 0) continue;

        /* THE BUGGY IOCTL — falls through to udev_reapurb_sub */
        void *reap_ptr = NULL;
        ioctl(fd, USBDEVFS_REAPURBNDELAY, &reap_ptr);
        success++;

        if ((i+1) % 10000 == 0)
            printf("  [%d/%d] ok=%d\n", i+1, iterations, success);
    }

    printf("\nIf you're reading this, the old ioctl didn't crash (unlikely).\n");
    close(fd);
    return 0;
}
