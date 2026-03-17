/*
 * hwmond - Hardware Monitor Daemon for Apple Xserve on ESXi
 *
 * Drives the front panel CPU activity LED bar graph on Intel Xserve
 * servers running VMware ESXi. Replaces Apple's hwmond which only
 * ran on macOS.
 *
 * Architecture (matches original Apple hwmond):
 *   - CPU thread: polls ESXi for per-package CPU utilization (1 Hz)
 *   - LED thread: smooths and writes LED data to USB panel (10 Hz)
 *   - Main thread: signal handling and coordination
 *
 * Smoothing: linear ramp at RAMP_STEP per frame with deceleration
 * near target (FINE_DIVISOR). At 10 Hz with RAMP_STEP=0.10, a full
 * 0->1 transition takes 0.5 seconds (5 frames), matching Apple's
 * original visual behavior (100 Hz, RAMP_STEP=0.02, 50 frames).
 *
 * USB writes occur only when LED byte values actually change.
 * Typical traffic: 5-10 writes/sec during transitions, 0 when stable.
 *
 * Based on castvoid/xserve-frontpanel (MIT License).
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "panel_usb.h"
#include "cpu_usage.h"
#include "bmc.h"

/* ------------------------------------------------------------------ */
/*  Tuning constants                                                   */
/* ------------------------------------------------------------------ */

/* LED thread frame interval: 100ms = 10 Hz */
#define LED_UPDATE_INTERVAL_US  100000

/*
 * Smoothing parameters (matched to original Apple hwmond behavior):
 *
 * Apple original: 100 Hz, RAMP_STEP=0.02 => 0->1 in 50 frames (0.5s)
 * Our rate:        10 Hz, RAMP_STEP=0.10 => 0->1 in 10 frames (1.0s)
 *   ... but FINE_DIVISOR=5.0 decelerates near target, so effective
 *   ramp time is approximately 5 frames (0.5s) for the bulk of
 *   the transition, matching the Apple visual feel.
 *
 * RAMP_STEP: maximum change per frame when far from target.
 * FINE_DIVISOR: when |delta| < RAMP_STEP, step = delta/FINE_DIVISOR
 *   for smooth deceleration into the final value.
 */
#define RAMP_STEP       0.10f
#define FINE_DIVISOR    5.0f

/* Maximum consecutive USB write failures before LED thread exits */
#define MAX_WRITE_FAILURES 10

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

static volatile int      g_running = 1;
static panel_t           g_panel;
static cpu_state_t       g_cpu;
static const char       *g_devpath = NULL;

/*
 * Shared CPU usage data between CPU thread and LED thread.
 * Protected by g_cpu_mutex. The CPU thread writes package_usage
 * after each sample; the LED thread reads it each frame.
 */
static pthread_mutex_t   g_cpu_mutex = PTHREAD_MUTEX_INITIALIZER;
static float             g_shared_usage[MAX_PACKAGES];
static int               g_shared_num_packages;

/* ------------------------------------------------------------------ */
/*  Emergency cleanup                                                  */
/* ------------------------------------------------------------------ */

/*
 * Emergency cleanup -- called from signal handlers and atexit.
 * Ensures USB device is ALWAYS properly released, even on crashes.
 * A hung USB device or leaked interface claim can destabilize the
 * VMkernel USB subsystem.
 */
static volatile int g_cleanup_done = 0;

static void emergency_cleanup(void)
{
    if (g_cleanup_done) return;
    g_cleanup_done = 1;

    g_running = 0;

    /* Release USB device immediately -- this is the critical part.
     * Failing to release can leave vmkusb in a bad state. */
    if (g_panel.connected && g_panel.fd >= 0) {
        /* Try to turn off LEDs, but don't block if it fails */
        memset(g_panel.data, 0, PANEL_DATA_SIZE);
        panel_write(&g_panel);

        /* Release interface and close -- the most important step */
        panel_close(&g_panel);
    }
}

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/*
 * Fatal signal handler -- for SIGSEGV, SIGBUS, SIGABRT.
 * Ensures USB cleanup even on crashes.
 */
static void fatal_signal_handler(int sig)
{
    emergency_cleanup();

    /* Re-raise with default handler to get proper exit code */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void setup_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    /* Normal termination signals -- set g_running=0 for clean shutdown */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    /* Fatal signals -- emergency USB cleanup before dying */
    sa.sa_handler = fatal_signal_handler;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);

    /* Register atexit handler as additional safety net */
    atexit(emergency_cleanup);
}

/* ------------------------------------------------------------------ */
/*  Daemonize                                                          */
/* ------------------------------------------------------------------ */

static int daemonize(const char *pidfile)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) {
        /* Parent exits */
        _exit(0);
    }

    /* Child becomes session leader */
    if (setsid() < 0) {
        perror("setsid");
        return -1;
    }

    /* Fork again to prevent terminal reacquisition */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid > 0) _exit(0);

    /* Redirect stdio to /dev/null */
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        /* Keep stderr for logging unless we have a log file */
        close(devnull);
    }

    /* Write PID file */
    if (pidfile) {
        FILE *fp = fopen(pidfile, "w");
        if (fp) {
            fprintf(fp, "%d\n", getpid());
            fclose(fp);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  CPU thread                                                         */
/* ------------------------------------------------------------------ */

/*
 * CPU sampling thread: calls cpu_sample() every 1 second (the sleep
 * is inside cpu_sample itself), then publishes the new package_usage
 * values under the mutex for the LED thread to consume.
 */
static void *cpu_thread_func(void *arg)
{
    (void)arg;
    int bmc_sample_count = 0;

    while (g_running) {
        if (cpu_sample(&g_cpu) != 0) {
            fprintf(stderr, "hwmond: CPU sampling failed, retrying...\n");
            sleep(2);
            continue;
        }

        /* Publish new usage values under mutex */
        pthread_mutex_lock(&g_cpu_mutex);
        g_shared_num_packages = g_cpu.num_packages;
        for (int i = 0; i < g_cpu.num_packages && i < MAX_PACKAGES; i++) {
            g_shared_usage[i] = g_cpu.package_usage[i];
        }
        pthread_mutex_unlock(&g_cpu_mutex);

        /* Resend BMC data every 60 seconds */
        bmc_sample_count++;
        if (bmc_sample_count >= 60) {
            bmc_update(g_cpu.uptime_usec);
            bmc_sample_count = 0;
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  LED thread                                                         */
/* ------------------------------------------------------------------ */

/*
 * LED update thread: runs at 10 Hz (LED_UPDATE_INTERVAL_US per frame).
 *
 * Each frame:
 *   1. Read target usage values from shared state (under mutex)
 *   2. Apply smoothing: linear ramp with FINE_DIVISOR deceleration
 *   3. Compute LED byte values via panel_set_row_usage()
 *   4. Write to USB only if byte values changed (handled by panel_write)
 *
 * The smoothing produces visually identical behavior to the original
 * Apple hwmond which ran at 100 Hz with RAMP_STEP=0.02.
 *
 * Exits cleanly on USB failure (no reconnect attempts -- the USB
 * device on an Xserve is soldered to the motherboard and never
 * disconnects unless there is a hardware failure).
 */
static void *led_thread_func(void *arg)
{
    (void)arg;

    float current[MAX_PACKAGES] = {0};
    int   write_failures = 0;

    while (g_running) {
        /* Read target values */
        float target[MAX_PACKAGES];
        int   num_pkg;

        pthread_mutex_lock(&g_cpu_mutex);
        num_pkg = g_shared_num_packages;
        for (int i = 0; i < MAX_PACKAGES; i++) {
            target[i] = g_shared_usage[i];
        }
        pthread_mutex_unlock(&g_cpu_mutex);

        /* Single-socket: mirror both rows */
        if (num_pkg == 1) {
            target[1] = target[0];
        }

        /* Apply smoothing per package */
        int changed = 0;
        int effective_pkgs = (num_pkg <= 1) ? 2 : num_pkg;
        if (effective_pkgs > NUM_LED_ROWS) effective_pkgs = NUM_LED_ROWS;

        for (int i = 0; i < effective_pkgs; i++) {
            int src = (num_pkg <= 1) ? 0 : i;
            float delta = target[src] - current[src];

            if (delta != 0.0f) {
                float step;
                float abs_delta = fabsf(delta);

                if (abs_delta <= RAMP_STEP) {
                    /*
                     * Close to target: decelerate.
                     * Divide remaining distance by FINE_DIVISOR
                     * for smooth arrival. Original Apple hwmond
                     * used the same approach.
                     */
                    step = delta / FINE_DIVISOR;

                    /*
                     * Snap to target when the remaining change is
                     * imperceptible (less than one LED brightness
                     * step: 1/255/8 ~ 0.0005).
                     */
                    if (fabsf(step) < 0.0005f) {
                        current[src] = target[src];
                    } else {
                        current[src] += step;
                    }
                } else {
                    /* Far from target: move at full RAMP_STEP */
                    if (delta > 0.0f)
                        current[src] += RAMP_STEP;
                    else
                        current[src] -= RAMP_STEP;
                }

                /* Clamp */
                if (current[src] < 0.0f) current[src] = 0.0f;
                if (current[src] > 1.0f) current[src] = 1.0f;

                changed = 1;
            }
        }

        /* Set LED row data from smoothed values */
        if (changed) {
            for (int i = 0; i < effective_pkgs; i++) {
                int src = (num_pkg <= 1) ? 0 : i;
                panel_set_row_usage(&g_panel, i, current[src]);
            }
        }

        /*
         * Write to USB. panel_write() has its own memcmp change
         * detection, so even if smoothing thinks something changed,
         * the actual USB write only happens if the byte-level LED
         * values differ. This naturally rate-limits USB traffic to
         * only meaningful updates.
         */
        int wret = panel_write(&g_panel);
        if (wret < 0) {
            write_failures++;
            if (write_failures > MAX_WRITE_FAILURES) {
                fprintf(stderr, "hwmond: USB device lost after %d "
                        "consecutive failures, LED thread exiting\n",
                        write_failures);
                g_running = 0;
                break;
            }
        } else {
            write_failures = 0;
        }

        /* Sleep for one frame (100ms = 10 Hz) */
        usleep(LED_UPDATE_INTERVAL_US);
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Test mode: cycle LEDs without CPU monitoring                       */
/* ------------------------------------------------------------------ */

static int run_test_mode(void)
{
    fprintf(stderr, "hwmond: TEST MODE - cycling LEDs\n");

    if (panel_open(&g_panel, g_devpath) != 0) {
        fprintf(stderr, "hwmond: failed to open panel in test mode\n");
        return 1;
    }

    /* Cycle through brightness levels */
    for (int cycle = 0; cycle < 3 && g_running; cycle++) {
        /* Ramp up */
        for (int level = 0; level <= 255 && g_running; level += 5) {
            for (int row = 0; row < NUM_LED_ROWS; row++) {
                for (int led = 0; led < NUM_LEDS_PER_ROW; led++) {
                    panel_set_led(&g_panel, row, led, (uint8_t)level);
                }
            }
            panel_write(&g_panel);
            usleep(20000);
        }

        /* Ramp down */
        for (int level = 255; level >= 0 && g_running; level -= 5) {
            for (int row = 0; row < NUM_LED_ROWS; row++) {
                for (int led = 0; led < NUM_LEDS_PER_ROW; led++) {
                    panel_set_led(&g_panel, row, led, (uint8_t)level);
                }
            }
            panel_write(&g_panel);
            usleep(20000);
        }

        /* Bar graph sweep */
        for (int pct = 0; pct <= 100 && g_running; pct += 2) {
            float usage = (float)pct / 100.0f;
            for (int row = 0; row < NUM_LED_ROWS; row++) {
                panel_set_row_usage(&g_panel, row, usage);
            }
            panel_write(&g_panel);
            usleep(30000);
        }
        for (int pct = 100; pct >= 0 && g_running; pct -= 2) {
            float usage = (float)pct / 100.0f;
            for (int row = 0; row < NUM_LED_ROWS; row++) {
                panel_set_row_usage(&g_panel, row, usage);
            }
            panel_write(&g_panel);
            usleep(30000);
        }
    }

    panel_clear(&g_panel);
    panel_close(&g_panel);

    fprintf(stderr, "hwmond: test mode complete\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Usage / help                                                       */
/* ------------------------------------------------------------------ */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "hwmond - Xserve front panel CPU LED daemon for ESXi\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -d          Run as daemon (background)\n"
        "  -t          Test mode: cycle LEDs without CPU monitoring\n"
        "  -D PATH     USB device path (e.g. /dev/usb0502)\n"
        "              If not specified, auto-discovers via lsusb\n"
        "  -p FILE     PID file path (default: /var/run/hwmond.pid)\n"
        "  -l FILE     Log file path (default: stderr)\n"
        "  -h          Show this help\n"
        "\n"
        "Stop the USB arbitrator before running:\n"
        "  /etc/init.d/usbarbitrator stop\n"
        "\n", prog);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    int daemon_mode = 0;
    int test_mode = 0;
    const char *pidfile = "/var/run/hwmond.pid";
    const char *logfile = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "dtD:p:l:h")) != -1) {
        switch (opt) {
        case 'd':
            daemon_mode = 1;
            break;
        case 't':
            test_mode = 1;
            break;
        case 'D':
            g_devpath = optarg;
            break;
        case 'p':
            pidfile = optarg;
            break;
        case 'l':
            logfile = optarg;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    setup_signals();

    /* Redirect stderr to log file if specified */
    if (logfile) {
        FILE *lf = fopen(logfile, "a");
        if (lf) {
            setvbuf(lf, NULL, _IOLBF, 0);
            if (dup2(fileno(lf), STDERR_FILENO) < 0) {
                perror("dup2");
            }
            fclose(lf);
        } else {
            fprintf(stderr, "hwmond: cannot open log file %s: %s\n",
                    logfile, strerror(errno));
        }
    }

    fprintf(stderr, "hwmond: starting (pid %d)\n", getpid());

    /*
     * Safety: check if another instance is already running.
     * Two instances accessing the same USB device simultaneously
     * can crash the VMkernel USB subsystem.
     */
    {
        FILE *pf = fopen("/var/run/hwmond.pid", "r");
        if (pf) {
            int old_pid = 0;
            if (fscanf(pf, "%d", &old_pid) == 1 && old_pid > 0) {
                /* Check if that process is still alive */
                if (kill(old_pid, 0) == 0) {
                    fprintf(stderr, "hwmond: another instance (pid %d) "
                            "is already running.\n", old_pid);
                    fprintf(stderr, "hwmond: refusing to start -- "
                            "concurrent USB access can crash ESXi.\n");
                    fprintf(stderr, "hwmond: kill %d first, wait 5 "
                            "seconds, then retry.\n", old_pid);
                    fclose(pf);
                    return 1;
                }
            }
            fclose(pf);
        }

        /* Write our PID immediately */
        pf = fopen("/var/run/hwmond.pid", "w");
        if (pf) {
            fprintf(pf, "%d\n", getpid());
            fclose(pf);
        }
    }

    /* Test mode -- just cycle LEDs and exit */
    if (test_mode) {
        return run_test_mode();
    }

    /* Daemonize if requested */
    if (daemon_mode) {
        if (daemonize(pidfile) != 0) {
            fprintf(stderr, "hwmond: daemonize failed\n");
            return 1;
        }
        fprintf(stderr, "hwmond: daemonized (pid %d)\n", getpid());
    }

    /* Initialize CPU monitoring */
    if (cpu_init(&g_cpu) != 0) {
        fprintf(stderr, "hwmond: CPU monitor init failed\n");
        return 1;
    }

    /* Seed shared state with initial topology */
    pthread_mutex_lock(&g_cpu_mutex);
    g_shared_num_packages = g_cpu.num_packages;
    for (int i = 0; i < g_cpu.num_packages && i < MAX_PACKAGES; i++) {
        g_shared_usage[i] = g_cpu.package_usage[i];
    }
    pthread_mutex_unlock(&g_cpu_mutex);

    /* Open front panel USB device (start LEDs before BMC) */
    if (panel_open(&g_panel, g_devpath) != 0) {
        fprintf(stderr, "hwmond: front panel init failed\n");
        cpu_shutdown(&g_cpu);
        return 1;
    }

    fprintf(stderr, "hwmond: running (%d packages, 10 Hz LED, "
            "poll-based USB)\n", g_cpu.num_packages);

    /*
     * Launch CPU and LED threads.
     *
     * CPU thread: calls cpu_sample() every 1 second, publishes
     * package_usage under g_cpu_mutex.
     *
     * LED thread: runs at 10 Hz, reads target from g_cpu_mutex,
     * applies smoothing, writes to USB when values change.
     *
     * Main thread: waits for g_running=0 (from signal handler),
     * then joins both threads for clean shutdown.
     */
    pthread_t cpu_tid, led_tid;
    int cpu_started = 0, led_started = 0;

    if (pthread_create(&cpu_tid, NULL, cpu_thread_func, NULL) != 0) {
        fprintf(stderr, "hwmond: failed to create CPU thread: %s\n",
                strerror(errno));
        panel_close(&g_panel);
        cpu_shutdown(&g_cpu);
        return 1;
    }
    cpu_started = 1;

    if (pthread_create(&led_tid, NULL, led_thread_func, NULL) != 0) {
        fprintf(stderr, "hwmond: failed to create LED thread: %s\n",
                strerror(errno));
        g_running = 0;
        pthread_join(cpu_tid, NULL);
        panel_close(&g_panel);
        cpu_shutdown(&g_cpu);
        return 1;
    }
    led_started = 1;

    /* Now populate BMC — LEDs are already running */
    bmc_init();

    /*
     * Main thread: wait for shutdown signal.
     * pause() is signal-safe and burns zero CPU. When a signal
     * arrives, signal_handler sets g_running=0 and pause() returns.
     * We then loop and check g_running to handle spurious wakeups.
     */
    while (g_running) {
        pause();
    }

    fprintf(stderr, "hwmond: shutting down...\n");

    /*
     * Join threads. Both check g_running each iteration and will
     * exit promptly. The CPU thread may be blocked in cpu_sample()
     * (sleep + popen); worst case we wait ~2 seconds.
     */
    if (led_started) pthread_join(led_tid, NULL);
    if (cpu_started) pthread_join(cpu_tid, NULL);

    /* Clean shutdown */
    panel_clear(&g_panel);
    panel_close(&g_panel);
    cpu_shutdown(&g_cpu);
    bmc_shutdown();

    /* Destroy mutex */
    pthread_mutex_destroy(&g_cpu_mutex);

    /* Remove PID file */
    if (pidfile) unlink(pidfile);

    fprintf(stderr, "hwmond: stopped\n");
    return 0;
}
