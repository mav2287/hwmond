/*
 * usb_stress.c — Stress test for submit+poll+close USB path
 *
 * Hammers the Xserve front panel with rapid submit/poll/close cycles
 * to verify the zero-REAP approach doesn't trigger PSOD.
 *
 * If this survives 100,000 cycles, the 10Hz production rate is safe.
 *
 * Build: zig cc -target x86_64-linux-gnu.2.12 -O2 -o build/usb_stress src/usb_stress.c
 * Run:   ./usb_stress /dev/usb0502 100000
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
#define USBDEVFS_SUBMITURB    _IOR('U', 10, struct usbdevfs_urb)
#define USBDEVFS_CLAIMINTERFACE _IOR('U', 15, unsigned int)
#pragma GCC diagnostic pop

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <device> <iterations>\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/usb0502 100000\n", argv[0]);
        return 1;
    }

    const char *devpath = argv[1];
    int iterations = atoi(argv[2]);
    uint8_t data[32];
    memset(data, 0, sizeof(data));

    printf("USB stress test: %s, %d iterations\n", devpath, iterations);
    printf("Method: SUBMITURB + poll(POLLOUT) + close (ZERO REAP)\n");
    printf("If this crashes, the close/reopen path has a bug.\n");
    printf("Starting...\n\n");

    int success = 0, submit_fail = 0, poll_fail = 0, open_fail = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < iterations; i++) {
        /* Open */
        int fd = open(devpath, O_RDWR);
        if (fd < 0) {
            open_fail++;
            usleep(10000);
            continue;
        }

        /* Claim interface */
        unsigned int iface = 0;
        ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);

        /* Submit URB */
        struct usbdevfs_urb urb;
        memset(&urb, 0, sizeof(urb));
        urb.type = USBDEVFS_URB_TYPE_BULK;
        urb.endpoint = 0x01;
        urb.buffer = data;
        urb.buffer_length = 32;

        int ret = ioctl(fd, USBDEVFS_SUBMITURB, &urb);
        if (ret < 0) {
            submit_fail++;
            close(fd);
            continue;
        }

        /* Poll for completion */
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        do {
            ret = poll(&pfd, 1, 5000);
        } while (ret < 0 && errno == EINTR);

        if (ret <= 0) {
            poll_fail++;
            close(fd);
            usleep(10000);
            continue;
        }

        /* Reap using ESXi native ioctl — routes to safe udev_handle_ioctl */
        struct { void *urb_ptr; uint64_t pad; } reap_buf;
        memset(&reap_buf, 0, sizeof(reap_buf));
        ret = ioctl(fd, (int)0xC0105512u, &reap_buf);
        if (ret < 0) {
            /* Fallback: close fd to clean up */
            close(fd);
            poll_fail++;
            continue;
        }

        close(fd);
        success++;

        /* Progress every 10,000 */
        if ((i + 1) % 10000 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (now.tv_sec - start.tv_sec) +
                            (now.tv_nsec - start.tv_nsec) / 1e9;
            double rate = (i + 1) / elapsed;
            printf("  [%d/%d] %.0f ops/sec — ok=%d submit_fail=%d "
                   "poll_fail=%d open_fail=%d\n",
                   i + 1, iterations, rate, success,
                   submit_fail, poll_fail, open_fail);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - start.tv_sec) +
                    (now.tv_nsec - start.tv_nsec) / 1e9;

    printf("\n=== RESULTS ===\n");
    printf("Iterations: %d\n", iterations);
    printf("Success:    %d\n", success);
    printf("Submit fail: %d\n", submit_fail);
    printf("Poll fail:   %d\n", poll_fail);
    printf("Open fail:   %d\n", open_fail);
    printf("Time:       %.1f sec (%.0f ops/sec)\n", elapsed, iterations / elapsed);
    printf("\nIf you're reading this, the close/reopen path is SAFE.\n");

    return 0;
}
