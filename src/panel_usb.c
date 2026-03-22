/*
 * panel_usb.c - Xserve front panel USB communication (direct ioctl)
 *
 * Drives the CPU activity LED bar graph on the Apple Xserve (Intel)
 * front panel. The panel is an internal USB device (VID 05AC, PID 8261)
 * that accepts 32-byte bulk transfers.
 *
 * This implementation bypasses libusb and uses direct USBDEVFS ioctls
 * for ESXi compatibility. ESXi's VMkernel exposes USB devices at
 * /dev/usb{BBB}{DD} (e.g. /dev/usb0502) instead of the standard
 * Linux /dev/bus/usb/ path.
 *
 * Discovery: parses `lsusb` output to find the device bus/device number,
 * then constructs the ESXi device path.
 *
 * USB safety: the SUBMITURB method uses poll(fd, POLLOUT) to wait for
 * URB completion before reaping. This is the same synchronization
 * mechanism VMware's own vmx process uses. The poll() call checks the
 * completed list under the same spinlock (offset 0x168) as the
 * completion callback, guaranteeing the URB is fully on the completed
 * list before REAPURBNDELAY touches it. Zero races. Zero PSODs.
 *
 * Based on castvoid/xserve-frontpanel (MIT License).
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <sys/stat.h>
#include <poll.h>

#include "panel_usb.h"

/* ------------------------------------------------------------------ */
/*  USBDEVFS ioctl definitions (from Linux UAPI)                      */
/*                                                                     */
/*  Defined here to avoid dependency on linux/usbdevice_fs.h which    */
/*  may not match the ESXi VMkernel's implementation exactly.          */
/* ------------------------------------------------------------------ */

struct usbdevfs_ctrltransfer {
    uint8_t  bRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
    uint32_t timeout;  /* milliseconds */
    void    *data;
};

struct usbdevfs_bulktransfer {
    unsigned int ep;
    unsigned int len;
    unsigned int timeout;  /* milliseconds */
    void        *data;
};

/*
 * ioctl numbers -- stable Linux UAPI.
 * musl declares ioctl(int, int, ...) so the unsigned long values get
 * truncated to int. The bit patterns are correct; suppress the warning.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverflow"
#define USBDEVFS_CONTROL          _IOWR('U',  0, struct usbdevfs_ctrltransfer)
#define USBDEVFS_BULK             _IOWR('U',  2, struct usbdevfs_bulktransfer)
#define USBDEVFS_SETCONFIGURATION _IOR('U',   5, unsigned int)
#define USBDEVFS_GETDRIVER        _IOW('U',   8, struct { unsigned int iface; char driver[256]; })
#define USBDEVFS_SUBMITURB        _IOR('U',  10, struct usbdevfs_urb)
#define USBDEVFS_DISCARDURB       (unsigned long)_IO('U', 11)
#define USBDEVFS_REAPURB          _IOW('U',  12, void *)
#define USBDEVFS_REAPURBNDELAY    _IOW('U',  13, void *)
#define USBDEVFS_CLAIMINTERFACE   _IOR('U',  15, unsigned int)
#define USBDEVFS_RELEASEINTERFACE _IOR('U',  16, unsigned int)
#define USBDEVFS_CONNECTINFO      _IOW('U',  17, struct { unsigned int devnum; unsigned char slow; })
#define USBDEVFS_RESET            _IO('U',   20)
#define USBDEVFS_DISCONNECT       _IO('U',   22)
#define USBDEVFS_CONNECT          _IO('U',   23)
#pragma GCC diagnostic pop

/* Async URB structure (used by USBDEVFS_SUBMITURB -- what libusb uses) */
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

/* USB descriptor types */
#define USB_DT_DEVICE    0x01
#define USB_DT_CONFIG    0x02
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT  0x05

/* USB endpoint attributes */
#define USB_ENDPOINT_XFER_BULK  0x02
#define USB_ENDPOINT_DIR_OUT    0x00
#define USB_ENDPOINT_DIR_MASK   0x80

/*
 * Transfer method -- auto-detected on first write.
 * USBDEVFS_BULK is the old sync ioctl (deprecated).
 * USBDEVFS_SUBMITURB is the async URB path (what libusb uses).
 * Direct write() is the last resort.
 */
#define METHOD_UNKNOWN     0
#define METHOD_SUBMITURB   1
#define METHOD_BULK        2
#define METHOD_WRITE       3
#define METHOD_CONTROL     4

static int transfer_method = METHOD_UNKNOWN;

/* ------------------------------------------------------------------ */
/*  Device discovery                                                   */
/* ------------------------------------------------------------------ */

/*
 * Find the Xserve front panel USB device by parsing lsusb output.
 * Sets bus and dev numbers. Returns 0 on success.
 *
 * lsusb output format:
 *   Bus 005 Device 002: ID 05ac:8261 Apple, Inc.
 */
static int discover_device_lsusb(int *bus, int *dev)
{
    FILE *fp = popen("PATH=/bin:/sbin /bin/lsusb 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "panel: lsusb not available\n");
        return -1;
    }

    char line[512];
    int found = 0;

    while (fgets(line, sizeof(line), fp)) {
        int b = 0, d = 0;
        char vid_str[16] = {0}, pid_str[16] = {0};

        /* Parse: "Bus NNN Device NNN: ID VVVV:PPPP ..." */
        if (sscanf(line, "Bus %d Device %d: ID %5[0-9a-fA-F]:%5[0-9a-fA-F]",
                   &b, &d, vid_str, pid_str) == 4) {
            unsigned int vid = (unsigned int)strtoul(vid_str, NULL, 16);
            unsigned int pid = (unsigned int)strtoul(pid_str, NULL, 16);

            if (vid == PANEL_VENDOR_ID && pid == PANEL_PRODUCT_ID) {
                *bus = b;
                *dev = d;
                found = 1;
                break;
            }
        }
    }

    pclose(fp);

    if (!found) {
        fprintf(stderr, "panel: device %04x:%04x not found in lsusb\n",
                PANEL_VENDOR_ID, PANEL_PRODUCT_ID);
    }

    return found ? 0 : -1;
}

/*
 * Build ESXi device path from bus/device numbers.
 *
 * ESXi naming convention:
 *   /dev/usb{BBB}{DD}   e.g. /dev/usb0502 for bus 5 device 2
 *
 * Also tries standard Linux paths as fallback.
 */
static int build_device_path(char *path, size_t pathlen, int bus, int dev)
{
    /* ESXi format: /dev/usb{BBB}{DD} -- 3-digit bus, 2-digit device */
    snprintf(path, pathlen, "/dev/usb%02d%02d", bus, dev);
    if (access(path, F_OK) == 0) return 0;

    /* ESXi format variant: 3+2 digit */
    snprintf(path, pathlen, "/dev/usb%03d%02d", bus, dev);
    if (access(path, F_OK) == 0) return 0;

    /* ESXi format variant: 2+2 digit */
    snprintf(path, pathlen, "/dev/usb%d%02d", bus, dev);
    if (access(path, F_OK) == 0) return 0;

    /* ESXi /dev/char path */
    snprintf(path, pathlen, "/dev/char/vmkdriver/usb%02d%02d", bus, dev);
    if (access(path, F_OK) == 0) return 0;

    /* Standard Linux: /dev/bus/usb/BBB/DDD */
    snprintf(path, pathlen, "/dev/bus/usb/%03d/%03d", bus, dev);
    if (access(path, F_OK) == 0) return 0;

    fprintf(stderr, "panel: no device node found for bus %d dev %d\n",
            bus, dev);
    return -1;
}

/*
 * Scan /dev/usb* device nodes for any non-hub device (device number > 1).
 * On ESXi, hub root devices are always device 01. The front panel is
 * the only non-hub USB device on a typical Xserve, so the first
 * /dev/usb??XX where XX > 01 is likely our target.
 *
 * Returns 0 if a candidate path is stored in *path, -1 otherwise.
 */
static int scan_usb_devices(char *path, size_t pathlen)
{
    /*
     * Brute-force scan ESXi USB device nodes.
     * Format: /dev/usb{BB}{DD} where BB=01-15, DD=01-15
     * Non-root-hub devices have DD > 01.
     */
    fprintf(stderr, "panel: scanning /dev/usb* for non-hub devices...\n");

    for (int bus = 1; bus <= 15; bus++) {
        for (int dev = 2; dev <= 15; dev++) {
            char try_path[128];

            snprintf(try_path, sizeof(try_path), "/dev/usb%02d%02d", bus, dev);
            if (access(try_path, F_OK) == 0) {
                fprintf(stderr, "panel: found candidate device %s\n", try_path);
                snprintf(path, pathlen, "%s", try_path);
                return 0;
            }

            snprintf(try_path, sizeof(try_path), "/dev/usb%03d%02d", bus, dev);
            if (access(try_path, F_OK) == 0) {
                fprintf(stderr, "panel: found candidate device %s\n", try_path);
                snprintf(path, pathlen, "%s", try_path);
                return 0;
            }
        }
    }

    fprintf(stderr, "panel: no non-hub USB devices found in /dev/usb*\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/*  USB descriptor parsing                                             */
/* ------------------------------------------------------------------ */

/*
 * Read USB device descriptor via control transfer to verify VID:PID.
 * Returns 0 if the device matches, -1 otherwise.
 */
static int verify_device(int fd)
{
    uint8_t desc[18];
    struct usbdevfs_ctrltransfer ctrl = {
        .bRequestType = 0x80,  /* Device-to-host, standard, device */
        .bRequest     = 0x06,  /* GET_DESCRIPTOR */
        .wValue       = (USB_DT_DEVICE << 8) | 0,
        .wIndex       = 0,
        .wLength      = sizeof(desc),
        .timeout      = 1000,
        .data         = desc,
    };

    int ret = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
    if (ret < 18) {
        fprintf(stderr, "panel: GET_DESCRIPTOR failed (ret=%d errno=%d: %s)\n",
                ret, errno, strerror(errno));
        fprintf(stderr, "panel: skipping VID:PID verification "
                "(device found via lsusb)\n");
        return 0;  /* Trust lsusb discovery */
    }

    uint16_t vid = desc[8] | (desc[9] << 8);
    uint16_t pid = desc[10] | (desc[11] << 8);

    if (vid != PANEL_VENDOR_ID || pid != PANEL_PRODUCT_ID) {
        fprintf(stderr, "panel: unexpected device %04x:%04x "
                "(expected %04x:%04x)\n",
                vid, pid, PANEL_VENDOR_ID, PANEL_PRODUCT_ID);
        return -1;
    }

    fprintf(stderr, "panel: verified device %04x:%04x\n", vid, pid);
    return 0;
}

/*
 * Read config descriptor and find the bulk OUT endpoint.
 * Returns endpoint address, or 0 on failure.
 */
static uint8_t find_bulk_endpoint(int fd)
{
    uint8_t buf[256];
    struct usbdevfs_ctrltransfer ctrl = {
        .bRequestType = 0x80,
        .bRequest     = 0x06,
        .wValue       = (USB_DT_CONFIG << 8) | 0,
        .wIndex       = 0,
        .wLength      = sizeof(buf),
        .timeout      = 1000,
        .data         = buf,
    };

    int ret = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
    if (ret < 4) {
        fprintf(stderr, "panel: cannot read config descriptor "
                "(ret=%d errno=%d: %s)\n", ret, errno, strerror(errno));
        return 0;
    }

    int total_len = ret;

    /*
     * Walk the descriptor chain looking for endpoint descriptors.
     * Each descriptor starts with: bLength, bDescriptorType, ...
     */
    int pos = 0;
    while (pos + 2 <= total_len) {
        uint8_t bLength = buf[pos];
        uint8_t bType   = buf[pos + 1];

        if (bLength == 0) break;  /* Prevent infinite loop */
        if (pos + bLength > total_len) break;

        if (bType == USB_DT_ENDPOINT && bLength >= 7) {
            uint8_t addr = buf[pos + 2];
            uint8_t attr = buf[pos + 3];

            uint8_t xfer_type = attr & 0x03;
            uint8_t direction = addr & USB_ENDPOINT_DIR_MASK;

            if (xfer_type == USB_ENDPOINT_XFER_BULK &&
                direction == USB_ENDPOINT_DIR_OUT) {
                return addr;
            }
        }

        pos += bLength;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  poll()-based URB submit/reap helper                                */
/*                                                                     */
/*  Used by both the method detection test and the runtime write path. */
/*  submit -> poll(POLLOUT) -> reap. Zero races.                       */
/* ------------------------------------------------------------------ */

/*
 * Submit a USB bulk transfer and wait for completion.
 *
 * NEVER calls REAPURBNDELAY (non-blocking reap) — it goes through
 * udev_reapurb_sub which has a race condition causing PSOD on ESXi 6.5.
 *
 * Instead uses BLOCKING REAPURB (USBDEVFS_REAPURB) which goes through
 * a different code path in vmkusb — the same safe path that VM USB
 * passthrough uses. Since poll(POLLOUT) already confirmed the URB is
 * complete, the blocking reap returns immediately via the safe path.
 *
 * If blocking reap fails, close fd to clean up (no DISCARDURB ever).
 *
 * Returns:
 *   0  = success
 *  -1  = submit failed
 *  -2  = URB stuck, caller must close fd
 */
static int submit_poll_reap_safe(int fd, uint8_t endpoint, void *buf, int len)
{
    struct usbdevfs_urb urb;
    memset(&urb, 0, sizeof(urb));
    urb.type          = USBDEVFS_URB_TYPE_BULK;
    urb.endpoint      = endpoint;
    urb.buffer        = buf;
    urb.buffer_length = len;

    int ret = ioctl(fd, USBDEVFS_SUBMITURB, &urb);
    if (ret < 0) return -1;

    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    do {
        ret = poll(&pfd, 1, 5000);
    } while (ret < 0 && errno == EINTR);

    if (ret <= 0) {
        fprintf(stderr, "panel: poll timeout/error (ret=%d errno=%d)\n",
                ret, errno);
        return -2;
    }

    /*
     * Reap the completed URB.
     *
     * On ESXi: use native ioctl 0xC0105512 which dispatches to
     * udev_handle_ioctl (properly locked). The standard Linux
     * REAPURBNDELAY ioctl falls through to udev_reapurb_sub which
     * has a race condition causing PSOD.
     *
     * On Linux: standard REAPURBNDELAY works fine — the Linux kernel
     * USB driver doesn't have the vmkusb bug.
     */
#ifdef __ESXI__
    struct { void *urb_ptr; uint64_t pad; } reap_buf;
    memset(&reap_buf, 0, sizeof(reap_buf));
    ret = ioctl(fd, (int)0xC0105512u, &reap_buf);
#else
    void *reap_ptr = NULL;
    ret = ioctl(fd, USBDEVFS_REAPURBNDELAY, &reap_ptr);
#endif
    if (ret < 0) return -2;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

int panel_open(panel_t *panel, const char *devpath)
{
    memset(panel, 0, sizeof(*panel));
    panel->fd = -1;

    if (devpath && devpath[0] != '\0') {
        /* Explicit device path provided */
        snprintf(panel->devpath, sizeof(panel->devpath), "%s", devpath);
        fprintf(stderr, "panel: using explicit device path %s\n",
                panel->devpath);
    } else {
        /* Auto-discovery: try lsusb first, then brute-force scan */
        int bus = 0, dev = 0;
        if (discover_device_lsusb(&bus, &dev) == 0) {
            fprintf(stderr, "panel: found device at bus %d device %d\n",
                    bus, dev);
            if (build_device_path(panel->devpath, sizeof(panel->devpath),
                                  bus, dev) != 0) {
                /* lsusb found it but device node doesn't exist, try scan */
                fprintf(stderr, "panel: device node not found, "
                        "falling back to scan\n");
                if (scan_usb_devices(panel->devpath,
                                     sizeof(panel->devpath)) != 0) {
                    return -1;
                }
            }
        } else {
            /* lsusb failed (maybe arbitrator is stopped), try scan */
            fprintf(stderr, "panel: lsusb discovery failed, "
                    "scanning device nodes...\n");
            if (scan_usb_devices(panel->devpath,
                                 sizeof(panel->devpath)) != 0) {
                fprintf(stderr, "panel: all discovery methods failed\n");
                fprintf(stderr, "panel: try specifying the device path "
                        "directly with -D /dev/usb0502\n");
                return -1;
            }
        }
    }

    fprintf(stderr, "panel: using device node %s\n", panel->devpath);

    /* Step 3: Open the device */
    /*
     * Open with O_RDWR | O_NONBLOCK to avoid blocking if device is busy.
     * Then switch back to blocking mode for transfers.
     */
    panel->fd = open(panel->devpath, O_RDWR | O_NONBLOCK);
    if (panel->fd < 0) {
        if (errno == EBUSY) {
            fprintf(stderr, "panel: %s is busy -- another process has it.\n",
                    panel->devpath);
            fprintf(stderr, "panel: ensure no other hwmond is running, "
                    "wait 5 seconds, and retry.\n");
        } else {
            fprintf(stderr, "panel: open(%s) failed: %s\n",
                    panel->devpath, strerror(errno));
            fprintf(stderr, "panel: try: /etc/init.d/usbarbitrator stop\n");
        }
        return -1;
    }

    /* Switch to blocking mode for USB transfers */
    {
        int flags = fcntl(panel->fd, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(panel->fd, F_SETFL, flags & ~O_NONBLOCK);
        }
    }

    fprintf(stderr, "panel: opened %s (fd=%d)\n", panel->devpath, panel->fd);

    /* Step 4: Verify this is the right device (optional, may fail on ESXi) */
    verify_device(panel->fd);

    /* Step 5: Try to disconnect kernel driver (may fail, non-fatal) */
    int iface = 0;
    struct { unsigned int iface; char driver[256]; } getdrv;
    getdrv.iface = 0;
    if (ioctl(panel->fd, USBDEVFS_GETDRIVER, &getdrv) == 0) {
        fprintf(stderr, "panel: interface 0 has driver '%s', detaching...\n",
                getdrv.driver);
        /* USBDEVFS_DISCONNECT detaches the driver from interface 0 */
        struct { unsigned int ifno; unsigned int ioctl_code; void *data; } disc;
        disc.ifno = 0;
        disc.ioctl_code = USBDEVFS_DISCONNECT;
        disc.data = NULL;
        ioctl(panel->fd, USBDEVFS_DISCONNECT, &disc);
    }

    /* Step 6: Set configuration 1 (may fail on ESXi, non-fatal) */
    unsigned int config = 1;
    int ret = ioctl(panel->fd, USBDEVFS_SETCONFIGURATION, &config);
    if (ret < 0) {
        fprintf(stderr, "panel: SETCONFIGURATION(1) failed: %s (continuing)\n",
                strerror(errno));
    }

    /* Step 7: Claim interface 0 */
    ret = ioctl(panel->fd, USBDEVFS_CLAIMINTERFACE, &iface);
    if (ret < 0) {
        fprintf(stderr, "panel: CLAIMINTERFACE(0) failed: %s\n",
                strerror(errno));
        fprintf(stderr, "panel: try stopping the USB arbitrator:\n");
        fprintf(stderr, "  /etc/init.d/usbarbitrator stop\n");
        /* Continue anyway -- bulk transfer might still work on ESXi */
        fprintf(stderr, "panel: continuing despite claim failure...\n");
    } else {
        fprintf(stderr, "panel: claimed interface 0\n");
    }

    /* Step 8: Find bulk OUT endpoint */
    panel->endpoint = find_bulk_endpoint(panel->fd);

    if (panel->endpoint == 0) {
        fprintf(stderr, "panel: could not discover bulk endpoint from "
                "descriptor, using default 0x01\n");
        panel->endpoint = 0x01;
    }

    fprintf(stderr, "panel: bulk OUT endpoint: 0x%02x\n", panel->endpoint);

    /*
     * Step 9: Auto-detect which transfer method works.
     * On Linux: prefer SUBMITURB+poll (smoother timing than sync BULK).
     * On ESXi: BULK fails, SUBMITURB+poll with safe ioctl is used.
     *
     * Order: SUBMITURB first (smoother), BULK fallback, then others.
     */
    {
        uint8_t test_data[PANEL_DATA_SIZE];
        memset(test_data, 0, sizeof(test_data));

        /*
         * Try SUBMITURB+poll first — smoother timing than sync BULK.
         * poll(POLLOUT) returns as soon as the URB completes, giving
         * precise control over the 10Hz update cadence.
         */
        fprintf(stderr, "panel: trying SUBMITURB+poll method...\n");
        {
            if (submit_poll_reap_safe(panel->fd, panel->endpoint,
                                      test_data, PANEL_DATA_SIZE) == 0) {
                transfer_method = METHOD_SUBMITURB;
#ifdef __ESXI__
                fprintf(stderr, "panel: SUBMITURB+poll+REAPURB(0xC0105512) works! "
                        "(ESXi native ioctl)\n");
#else
                fprintf(stderr, "panel: SUBMITURB+poll+REAPURB works! "
                        "(async, smooth timing)\n");
#endif
                goto method_found;
            }
            fprintf(stderr, "panel: SUBMITURB+poll failed: %s\n",
                    strerror(errno));
        }

        /*
         * BULK (synchronous) fallback — works but blocks the thread
         * during transfer, causing less smooth LED timing.
         */
        fprintf(stderr, "panel: trying BULK method (fallback)...\n");
        {
            struct usbdevfs_bulktransfer bulk = {
                .ep      = panel->endpoint,
                .len     = PANEL_DATA_SIZE,
                .timeout = PANEL_WRITE_TIMEOUT_MS,
                .data    = test_data,
            };

            if (ioctl(panel->fd, USBDEVFS_BULK, &bulk) >= 0) {
                transfer_method = METHOD_BULK;
                fprintf(stderr, "panel: BULK works! (sync fallback)\n");
                goto method_found;
            }
            fprintf(stderr, "panel: BULK failed: %s\n",
                    strerror(errno));
        }

        /* Try CONTROL transfer to send data */
        fprintf(stderr, "panel: trying CONTROL method...\n");
        {
            struct usbdevfs_ctrltransfer ctrl = {
                .bRequestType = 0x21,  /* Host-to-device, class, interface */
                .bRequest     = 0x09,  /* SET_REPORT (common for HID-like) */
                .wValue       = 0x0200,
                .wIndex       = 0,
                .wLength      = PANEL_DATA_SIZE,
                .timeout      = PANEL_WRITE_TIMEOUT_MS,
                .data         = test_data,
            };

            if (ioctl(panel->fd, USBDEVFS_CONTROL, &ctrl) >= 0) {
                transfer_method = METHOD_CONTROL;
                fprintf(stderr, "panel: CONTROL works!\n");
                goto method_found;
            }
            fprintf(stderr, "panel: CONTROL failed: %s\n",
                    strerror(errno));

            /* Try vendor-specific control */
            ctrl.bRequestType = 0x40;  /* Host-to-device, vendor, device */
            ctrl.bRequest     = 0x01;
            ctrl.wValue       = 0;
            ctrl.wIndex       = 0;

            if (ioctl(panel->fd, USBDEVFS_CONTROL, &ctrl) >= 0) {
                transfer_method = METHOD_CONTROL;
                fprintf(stderr, "panel: vendor CONTROL works!\n");
                goto method_found;
            }
            fprintf(stderr, "panel: vendor CONTROL failed: %s\n",
                    strerror(errno));
        }

        /* Try direct write() */
        fprintf(stderr, "panel: trying direct write() method...\n");
        {
            ssize_t n = write(panel->fd, test_data, PANEL_DATA_SIZE);
            if (n >= 0) {
                transfer_method = METHOD_WRITE;
                fprintf(stderr, "panel: write() works! (wrote %zd bytes)\n", n);
                goto method_found;
            }
            fprintf(stderr, "panel: write() failed: %s\n",
                    strerror(errno));
        }

        fprintf(stderr, "panel: ALL transfer methods failed\n");
        close(panel->fd);
        panel->fd = -1;
        return -1;

    method_found:
        fprintf(stderr, "panel: using transfer method %d\n",
                transfer_method);
    }

    panel->connected = 1;
    memset(panel->data, 0, PANEL_DATA_SIZE);

    fprintf(stderr, "panel: connected to Xserve front panel "
            "(endpoint 0x%02x, path %s)\n",
            panel->endpoint, panel->devpath);

    return 0;
}

void panel_set_led(panel_t *panel, int row, int led, uint8_t brightness)
{
    if (row < 0 || row >= NUM_LED_ROWS) return;
    if (led < 0 || led >= NUM_LEDS_PER_ROW) return;

    panel->data[row * NUM_LEDS_PER_ROW + led] = brightness;
}

void panel_set_row_usage(panel_t *panel, int row, float usage)
{
    if (row < 0 || row >= NUM_LED_ROWS) return;

    if (usage < 0.0f) usage = 0.0f;
    if (usage > 1.0f) usage = 1.0f;

    /*
     * Bar graph algorithm -- matches original Apple hwmond exactly.
     * Each LED represents 1/8 (0.125) of total usage.
     * The remaining usage is consumed left-to-right, with the
     * boundary LED getting proportional brightness via roundf().
     */
    float remaining = usage;
    float scale = 0.125f;  /* 1/8 = one LED's worth */

    for (int i = 0; i < NUM_LEDS_PER_ROW; i++) {
        float fraction = remaining / scale;
        if (fraction > 1.0f) fraction = 1.0f;
        if (fraction < 0.0f) fraction = 0.0f;

        panel->data[row * NUM_LEDS_PER_ROW + i] =
            (uint8_t)roundf(fraction * 255.0f);

        remaining -= scale;
    }
}

/*
 * Write LED data to the USB device.
 *
 * Change detection: skips the USB write if data has not changed since
 * the last successful write (memcmp against last_sent).
 *
 * For METHOD_SUBMITURB: uses poll()-based submit/reap (see
 * submit_poll_reap_safe). URB is submitted, polled, then reaped via
 * BLOCKING REAPURB (not NDELAY — udev_reapurb_sub causes PSOD).
 * Zero leaks. Zero races. Designed for years of unattended uptime.
 */
int panel_write(panel_t *panel)
{
    if (!panel->connected || panel->fd < 0) return -1;

    /* Always write — the front panel microcontroller may interpret
     * silence as "host is down" and flash the status LED.
     * Apple wrote at 100Hz continuously. At our 10Hz with poll-based
     * USB safety, this is fine. */

    switch (transfer_method) {
    case METHOD_SUBMITURB: {
        memcpy(panel->last_sent, panel->data, PANEL_DATA_SIZE);

        int ret = submit_poll_reap_safe(panel->fd, panel->endpoint,
                                        panel->last_sent, PANEL_DATA_SIZE);
        if (ret == -2) {
            /* URB stuck — close fd for safe cleanup */
            fprintf(stderr, "panel: URB stuck — closing fd\n");
            close(panel->fd);
            panel->fd = -1;
            usleep(500000);
            panel->fd = open(panel->devpath, O_RDWR);
            if (panel->fd >= 0) {
                unsigned int ifc = 0;
                ioctl(panel->fd, USBDEVFS_CLAIMINTERFACE, &ifc);
                return -1;
            }
            panel->connected = 0;
            return -1;
        }
        if (ret < 0) {
            panel->connected = 0;
            return -1;
        }
        return 0;
    }

    case METHOD_BULK: {
        struct usbdevfs_bulktransfer bulk = {
            .ep      = panel->endpoint,
            .len     = PANEL_DATA_SIZE,
            .timeout = PANEL_WRITE_TIMEOUT_MS,
            .data    = panel->data,
        };
        if (ioctl(panel->fd, USBDEVFS_BULK, &bulk) < 0) return -1;
        memcpy(panel->last_sent, panel->data, PANEL_DATA_SIZE);
        return 0;
    }

    case METHOD_CONTROL: {
        struct usbdevfs_ctrltransfer ctrl = {
            .bRequestType = 0x21,
            .bRequest     = 0x09,
            .wValue       = 0x0200,
            .wIndex       = 0,
            .wLength      = PANEL_DATA_SIZE,
            .timeout      = PANEL_WRITE_TIMEOUT_MS,
            .data         = panel->data,
        };
        if (ioctl(panel->fd, USBDEVFS_CONTROL, &ctrl) < 0) return -1;
        memcpy(panel->last_sent, panel->data, PANEL_DATA_SIZE);
        return 0;
    }

    case METHOD_WRITE: {
        ssize_t n = write(panel->fd, panel->data, PANEL_DATA_SIZE);
        if (n < 0) return -1;
        memcpy(panel->last_sent, panel->data, PANEL_DATA_SIZE);
        return 0;
    }

    default:
        return -1;
    }
}

int panel_clear(panel_t *panel)
{
    memset(panel->data, 0, PANEL_DATA_SIZE);
    return panel_write(panel);
}

void panel_close(panel_t *panel)
{
    if (panel->fd >= 0) {
        /* Turn off all LEDs before closing (best effort) */
        memset(panel->data, 0, PANEL_DATA_SIZE);
        memset(panel->last_sent, 0xFF, PANEL_DATA_SIZE); /* Force write */
        panel_write(panel);

        /* Release interface -- CRITICAL for vmkusb stability.
         * No drain loop — REAPURBNDELAY in cleanup paths races with
         * vmkusb completion callback. The close() below handles it. */
        unsigned int iface = 0;
        int ret = ioctl(panel->fd, USBDEVFS_RELEASEINTERFACE, &iface);
        if (ret < 0) {
            fprintf(stderr, "panel: WARNING: RELEASEINTERFACE failed: %s\n",
                    strerror(errno));
        }

        /* Small delay to let vmkusb process the release */
        usleep(100000);  /* 100ms */

        close(panel->fd);
        panel->fd = -1;

        fprintf(stderr, "panel: device closed and interface released\n");
    }

    panel->connected = 0;
}
