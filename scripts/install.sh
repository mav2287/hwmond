#!/bin/sh
# install.sh — Universal installer for hwmond on Linux
# Works on any distro with systemd (Debian, Ubuntu, Proxmox, RHEL, Fedora, Rocky, Arch, etc.)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BINARY="${SCRIPT_DIR}/../build/hwmond"

if [ ! -f "${BINARY}" ]; then
    echo "Error: build/hwmond not found. Run 'make linux' first." >&2
    exit 1
fi

echo "Installing hwmond for Apple Xserve..."

# Binary
install -m 755 "${BINARY}" /usr/local/sbin/hwmond

# systemd service
install -m 644 "${SCRIPT_DIR}/hwmond.service" /etc/systemd/system/hwmond.service

# udev rule — exclusive USB claim + passthrough block
install -m 644 "${SCRIPT_DIR}/99-xserve-panel.rules" /etc/udev/rules.d/99-xserve-panel.rules

# IPMI modprobe config — Apple BMC at KCS port 0xCA2
install -m 644 "${SCRIPT_DIR}/hwmond-ipmi.conf" /etc/modprobe.d/hwmond-ipmi.conf

# Ensure IPMI modules load at boot
mkdir -p /etc/modules-load.d
printf 'ipmi_devintf\nipmi_si\n' > /etc/modules-load.d/hwmond-ipmi.conf

# Load IPMI kernel modules now
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
