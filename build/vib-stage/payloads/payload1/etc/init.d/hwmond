#!/bin/sh
# hwmond-init.sh - VIB install/upgrade/remove hooks
#
# This script runs ONCE during VIB lifecycle events.
# For boot-time startup, see hwmond-startup.sh.
#
# The hwmond binary is self-contained — it handles CPU monitoring
# internally via popen(vsish). No companion scripts to manage.
#
# WARNING: Never use kill -9 (SIGKILL) on hwmond!
# SIGTERM is required for clean USB interface release.

HWMOND_PID="/var/run/hwmond.pid"
HWMOND_LOG="/var/log/hwmond.log"
STARTUP_SCRIPT="/etc/rc.local.d/hwmond-startup.sh"

# Stop hwmond gracefully via SIGTERM. Never SIGKILL.
stop_daemon() {
    if [ -f "${HWMOND_PID}" ]; then
        pid=$(cat "${HWMOND_PID}" 2>/dev/null)
        if [ -n "${pid}" ] && kill -0 "${pid}" 2>/dev/null; then
            kill "${pid}" 2>/dev/null
            # Wait up to 10 seconds for clean USB release
            i=0
            while [ $i -lt 100 ] && kill -0 "${pid}" 2>/dev/null; do
                usleep 100000 2>/dev/null || sleep 1
                i=$((i + 1))
            done
            if kill -0 "${pid}" 2>/dev/null; then
                logger -t hwmond "WARNING: pid ${pid} did not exit after SIGTERM. NOT sending SIGKILL."
            fi
        fi
        rm -f "${HWMOND_PID}"
    fi
}

case "$1" in
    start)
        case "$2" in
            install)
                logger -t hwmond "VIB installed (v2.0.0)"
                if [ -x "${STARTUP_SCRIPT}" ]; then
                    "${STARTUP_SCRIPT}"
                fi
                ;;
            upgrade)
                logger -t hwmond "VIB upgraded (v2.0.0)"
                stop_daemon
                sleep 2
                if [ -x "${STARTUP_SCRIPT}" ]; then
                    "${STARTUP_SCRIPT}"
                fi
                ;;
        esac
        ;;
    stop)
        case "$2" in
            remove)
                logger -t hwmond "VIB removed, stopping daemon"
                stop_daemon
                rm -f "${HWMOND_LOG}"
                ;;
        esac
        ;;
esac

exit 0
