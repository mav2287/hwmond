#!/bin/sh
# install.sh — Universal installer for hwmond on Linux
# Works on any distro with systemd
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="${SCRIPT_DIR}/../build/hwmond"

if [ ! -f "${BINARY}" ]; then
    echo "Error: build/hwmond not found. Run 'make linux' first." >&2
    exit 1
fi

echo "Installing hwmond for Apple Xserve..."

# Binary
install -D -m 755 "${BINARY}" /usr/local/sbin/hwmond

# systemd service
install -D -m 644 "${SCRIPT_DIR}/hwmond.service" /etc/systemd/system/hwmond.service

# udev rule — exclusive USB claim + passthrough block
install -D -m 644 "${SCRIPT_DIR}/99-xserve-panel.rules" /etc/udev/rules.d/99-xserve-panel.rules

# Load IPMI kernel modules if not loaded
modprobe ipmi_devintf 2>/dev/null || true
modprobe ipmi_si 2>/dev/null || true

# Activate
systemctl daemon-reload
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true
systemctl enable hwmond
systemctl start hwmond

echo ""
echo "hwmond installed and running."
echo "  Status:  systemctl status hwmond"
echo "  Logs:    journalctl -u hwmond -f"
echo "  Stop:    systemctl stop hwmond"
echo "  Remove:  systemctl stop hwmond && systemctl disable hwmond && rm /usr/local/sbin/hwmond /etc/systemd/system/hwmond.service /etc/udev/rules.d/99-xserve-panel.rules"
