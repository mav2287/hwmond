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

# Configure USB: claim the front panel device exclusively.
# 1. Stop arbitrator to release the device
# 2. Mark VID:PID 05ac:8261 as non-passthrough so no VM can claim it
# 3. Restart arbitrator with the exclusion in place
configure_usb() {
    # Stop arbitrator to release all USB devices
    /etc/init.d/usbarbitrator stop 2>/dev/null
    sleep 1

    # Exclude the Xserve front panel (05ac:8261) from USB passthrough.
    # This prevents anyone from accidentally passing it through to a VM
    # via the vSphere UI while hwmond owns it.
    PASSTHRU_CONF="/etc/vmware/passthru.map"
    if [ -f "${PASSTHRU_CONF}" ]; then
        # Remove any existing entry for this device
        grep -v "05ac.*8261" "${PASSTHRU_CONF}" > "${PASSTHRU_CONF}.tmp" 2>/dev/null
        mv "${PASSTHRU_CONF}.tmp" "${PASSTHRU_CONF}"
    fi
    # Add deny rule — device will not appear in passthrough UI
    echo "# Xserve front panel — claimed by hwmond" >> "${PASSTHRU_CONF}"
    echo "05ac  8261  d  default  default" >> "${PASSTHRU_CONF}"

    # Also set the device-specific arbitrator config
    esxcli hardware usb passthrough device disable -d 05ac:8261 2>/dev/null

    # Restart arbitrator with the exclusion active.
    # hwmond opens the device directly via /dev/usb* — the arbitrator
    # won't interfere because we close/reopen per write cycle and
    # the device is excluded from passthrough.
    /etc/init.d/usbarbitrator start 2>/dev/null
    sleep 1
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
