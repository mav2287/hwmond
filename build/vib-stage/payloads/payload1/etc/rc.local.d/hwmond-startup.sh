#!/bin/sh
# hwmond-startup.sh - Start hwmond at ESXi boot
#
# Installed to /etc/rc.local.d/ by the hwmond VIB.
# Runs automatically during ESXi boot (after hostd starts).
#
# The hwmond binary is a native dynamically-linked ESXi binary that
# handles CPU monitoring internally via popen(vsish). No companion
# scripts are needed.
#
# WARNING: Never use kill -9 (SIGKILL) on hwmond!
# =========================================================
# hwmond holds an exclusive USB interface claim on the Xserve front
# panel. SIGKILL bypasses the cleanup handler, leaving the vmkusb
# driver in a bad state (leaked interface claim). This can require
# a full ESXi reboot to recover. Always use SIGTERM (kill <pid>)
# which triggers a clean shutdown: LEDs off, interface released,
# device closed.
# =========================================================

HWMOND_BIN="/opt/hwmond/hwmond"
HWMOND_PID="/var/run/hwmond.pid"
HWMOND_LOG="/var/log/hwmond.log"
HWMOND_DEV="/dev/usb0502"

# Stop the USB arbitrator so hwmond can claim the front panel device.
# The arbitrator holds all USB devices by default on ESXi; we need
# it released before opening /dev/usb0502.
configure_usb() {
    /etc/init.d/usbarbitrator stop 2>/dev/null
    sleep 2
}

# Stop hwmond gracefully using SIGTERM only.
# NEVER use SIGKILL — see warning at top of file.
stop_hwmond() {
    if [ -f "${HWMOND_PID}" ]; then
        pid=$(cat "${HWMOND_PID}" 2>/dev/null)
        if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
            # Send SIGTERM for clean USB release
            kill "${pid}" 2>/dev/null
            # Wait up to 10 seconds for clean shutdown
            i=0
            while [ $i -lt 100 ] && kill -0 "${pid}" 2>/dev/null; do
                usleep 100000 2>/dev/null || sleep 1
                i=$((i + 1))
            done
            if kill -0 "${pid}" 2>/dev/null; then
                # Do NOT use kill -9. Log a warning instead.
                logger -t hwmond "WARNING: pid ${pid} did not exit after SIGTERM. NOT sending SIGKILL (would corrupt USB state). Manual intervention required."
            fi
        fi
        rm -f "${HWMOND_PID}"
    fi
}

start_hwmond() {
    if [ ! -x "${HWMOND_BIN}" ]; then
        logger -t hwmond "Binary not found at ${HWMOND_BIN}"
        return 1
    fi

    # Stop any existing instances
    stop_hwmond

    # Release USB device from arbitrator
    configure_usb

    # Start hwmond daemon
    "${HWMOND_BIN}" -d -D "${HWMOND_DEV}" -p "${HWMOND_PID}" -l "${HWMOND_LOG}"
    ret=$?

    if [ ${ret} -eq 0 ]; then
        logger -t hwmond "Started (PID $(cat ${HWMOND_PID} 2>/dev/null))"
    else
        logger -t hwmond "Failed to start (exit code ${ret})"
    fi

    return ${ret}
}

case "$1" in
    stop)
        stop_hwmond
        ;;
    restart)
        stop_hwmond
        sleep 2
        start_hwmond
        ;;
    *)
        start_hwmond
        ;;
esac
