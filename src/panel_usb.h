/*
 * panel_usb.h - Xserve front panel USB communication
 *
 * Controls the CPU activity LED bar graph on Apple Xserve (Intel)
 * front panel via USB bulk transfers to device 05AC:8261.
 *
 * Uses direct USBDEVFS ioctls instead of libusb for ESXi compatibility.
 * ESXi exposes USB devices at /dev/usb{BBB}{DD} rather than the
 * standard Linux /dev/bus/usb/{BBB}/{DDD} path.
 *
 * Based on reverse engineering from castvoid/xserve-frontpanel (MIT License).
 */

#ifndef PANEL_USB_H
#define PANEL_USB_H

#include <stdint.h>

/* Apple Xserve front panel USB identifiers */
#define PANEL_VENDOR_ID   0x05AC
#define PANEL_PRODUCT_ID  0x8261

/* LED layout: 2 rows (one per CPU package) x 8 LEDs each */
#define NUM_LED_ROWS      2
#define NUM_LEDS_PER_ROW  8
#define CPU_LED_SIZE      16   /* only the CPU LED bytes (2x8) */
#define PANEL_DATA_SIZE   32   /* full panel buffer */

/* USB transfer timeout in milliseconds */
#define PANEL_WRITE_TIMEOUT_MS 90

/* Panel state */
typedef struct {
    int                    fd;           /* device file descriptor */
    uint8_t                endpoint;     /* bulk OUT endpoint address */
    uint8_t                data[PANEL_DATA_SIZE];
    uint8_t                last_sent[PANEL_DATA_SIZE]; /* last data written to USB */
    int                    connected;
    char                   devpath[128]; /* /dev/usb0502 etc */
} panel_t;

/*
 * Initialize and open the front panel USB device.
 * If devpath is non-NULL, use it directly instead of auto-discovery.
 * Returns 0 on success, -1 on failure.
 */
int panel_open(panel_t *panel, const char *devpath);

/*
 * Set LED brightness for a single LED.
 *   row: 0 or 1 (CPU package index)
 *   led: 0-7 (LED position in the row)
 *   brightness: 0 (off) to 255 (full)
 */
void panel_set_led(panel_t *panel, int row, int led, uint8_t brightness);

/*
 * Set an entire row of LEDs from a CPU usage value (0.0 - 1.0).
 * Maps the usage to 8 LEDs as a bar graph with smooth dimming
 * on the boundary LED.
 */
void panel_set_row_usage(panel_t *panel, int row, float usage);

/*
 * Write the current LED data buffer to the USB device.
 * Returns 0 on success, -1 on failure.
 */
int panel_write(panel_t *panel);

/*
 * Clear all LEDs (set to 0) and write to device.
 */
int panel_clear(panel_t *panel);

/*
 * Close the USB device and free resources.
 */
void panel_close(panel_t *panel);

#endif /* PANEL_USB_H */
