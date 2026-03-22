# hwmond — Apple Xserve Hardware Monitor

A hardware monitoring daemon for Apple Xserve servers. Replicates Apple's original `hwmond` functionality on non-macOS operating systems: drives the front panel CPU activity LEDs and populates the BMC with system data readable by Apple Server Monitor.

Runs on **VMware ESXi 6.5** and **Linux** (Proxmox, Debian, Ubuntu, RHEL, Fedora, Rocky, Arch, and any systemd-based distro).

## Features

- **Front Panel CPU Activity LEDs** — Drives the 2x8 LED bar graph at 100 Hz with smooth ramping that matches Apple's original visual behavior
- **BMC Data Population** — Writes system information to the Xserve BMC via Apple's OEM IPMI protocol (NetFn 0x36):
  - System identity (hostname, FQDN, OS version, Boot ROM version, serial number, model)
  - CPU information (model, speed, core count, package count)
  - Total memory and per-DIMM details (slot label, size, speed, type, ECC)
  - Drive inventory (model, manufacturer, capacity, bus type, location)
  - Network interface status (MAC, IP, link state, speed, duplex, driver)
  - System uptime
- **Apple Server Monitor Compatible** — All data formatted to match the wire protocol expected by Apple Server Monitor
- **Hardware Safety Check** — Blocks installation and startup on non-Xserve hardware to prevent BMC damage
- **Production Stable** — Designed for unlimited unattended uptime with zero kernel panics

## Hardware Compatibility

| Component | Details |
|-----------|---------|
| Server | Apple Xserve 1,1 / 2,1 / 3,1 |
| Front Panel USB | VID `05AC`, PID `8261` (Apple Xserve front panel controller) |
| BMC/LOM | Built-in Lights-Out Management accessible via IPMI |

### Tested Configurations

| Platform | Server | Status |
|----------|--------|--------|
| VMware ESXi 6.5 | Xserve 3,1 (Dual Xeon W5590) | 24+ hours, 100K USB stress test |
| Proxmox VE 8.x | Xserve 3,1 (Dual Xeon W5590) | Production stable |

## Installation

### Linux — APT (Debian, Ubuntu, Proxmox)

```bash
# Add the repository
echo "deb [trusted=yes] https://raw.githubusercontent.com/mav2287/hwmond/apt-repo ./" \
  > /etc/apt/sources.list.d/hwmond.list

# Install
apt update && apt install hwmond-xserve
```

Updates are automatic via `apt upgrade`.

### Linux — RPM (RHEL, Fedora, Rocky)

Download the RPM from the [`releases/`](releases/) directory and install:

```bash
rpm -ivh hwmond-xserve-3.2.0-1.noarch.rpm
```

Or with dnf/yum:

```bash
dnf install ./hwmond-xserve-3.2.0-1.noarch.rpm
```

### Linux — Universal (Any Distro with systemd)

Clone and build from source:

```bash
git clone https://github.com/mav2287/hwmond.git
cd hwmond
make linux
sudo ./scripts/install.sh
```

### VMware ESXi 6.5

#### Step 1: Enable SSH

In the vSphere/ESXi web UI, navigate to **Host > Manage > Services**, find **TSM-SSH**, and click **Start**.

#### Step 2: Set Acceptance Level

```bash
ssh root@<your-esxi-host>
esxcli software acceptance set --level CommunitySupported
```

#### Step 3: Upload and Install the VIB

```bash
scp releases/hwmond-xserve-3.2.0.vib root@<your-esxi-host>:/tmp/
```

On the ESXi host:

```bash
esxcli software vib install -v /tmp/hwmond-xserve-3.2.0.vib --force --no-sig-check --no-live-install
```

`--no-live-install` stages the VIB for activation on next boot (required for ESXi 6.5 bootbank VIBs).

#### Step 4: Reboot

Reboot through the **vSphere/ESXi web UI** (not the `reboot` shell command) to ensure VMs are shut down properly and VIB staging is finalized.

#### Step 5: Verify

```bash
ps -c | grep hwmond
```

The front panel LEDs should be active and Apple Server Monitor should show system data when pointed at the Xserve's LOM address.

## Removal

### Linux (APT)

```bash
apt remove hwmond-xserve
```

### Linux (RPM)

```bash
rpm -e hwmond-xserve
```

### Linux (Manual)

```bash
systemctl stop hwmond && systemctl disable hwmond
rm /usr/local/sbin/hwmond /etc/systemd/system/hwmond.service \
   /etc/udev/rules.d/99-xserve-panel.rules /etc/modprobe.d/hwmond-ipmi.conf
systemctl daemon-reload
```

### ESXi

```bash
esxcli software vib remove -n hwmond-xserve
```

Then reboot through the web UI.

## What Gets Installed

### Linux

| File | Purpose |
|------|---------|
| `/usr/local/sbin/hwmond` | The daemon binary |
| `/etc/systemd/system/hwmond.service` | systemd service unit |
| `/etc/udev/rules.d/99-xserve-panel.rules` | Blocks VM passthrough of front panel USB |
| `/etc/modprobe.d/hwmond-ipmi.conf` | Apple BMC KCS port config (0xCA2) |

### ESXi

| File | Purpose |
|------|---------|
| `/opt/hwmond/hwmond` | The daemon binary |
| `/etc/rc.local.d/hwmond-startup.sh` | Auto-start script (runs on every boot) |
| `/etc/init.d/hwmond` | Service lifecycle hooks (start/stop/status) |

## Architecture

```
hwmond
 |- CPU Thread (1 Hz)  -- per-package utilization, uptime
 |- LED Thread (100 Hz) -- USB SUBMITURB + REAPURB, smooth ramp, bar graph
 '- BMC Init + Update  -- Apple OEM IPMI, all 15 parameters
```

- **CPU Thread**: Reads per-CPU utilization at 1 Hz. On ESXi, uses `vsish` for hypervisor-level stats. On Linux, reads `/proc/stat` with topology detection from `/proc/cpuinfo`.
- **LED Thread**: Smooths CPU utilization into LED levels with linear ramping and deceleration near target. Writes to the front panel USB device at 100 Hz. Uses `SUBMITURB` + `poll` for smooth timing on both platforms.
- **BMC**: Collects system information at startup (hostname, OS, CPU, memory, drives, NICs) and writes it to the BMC via Apple's OEM IPMI protocol. Re-checks drives and NICs every 60 seconds for hot-plug changes.

### Platform Abstraction

The codebase uses a clean platform split:

| Component | ESXi | Linux |
|-----------|------|-------|
| CPU monitoring | `cpu_usage.c` (vsish) | `cpu_linux.c` (/proc/stat) |
| Data collection | `collect_esxi.c` (esxcli, vsish, smbiosDump) | `collect_linux.c` (/proc, /sys, dmidecode) |
| USB panel | `panel_usb.c` (ioctl 0xC0105512) | `panel_usb.c` (REAPURBNDELAY) |
| BMC/IPMI | `bmc.c` (shared) | `bmc.c` (shared) |

## Building from Source

### Prerequisites

- **ESXi build**: [Zig](https://ziglang.org/) (cross-compiles to glibc 2.12)
- **Linux build**: GCC and standard dev headers
- **RPM packaging**: `rpmbuild` (from `rpm-build` package)
- **DEB packaging**: `dpkg-deb` (from `dpkg-dev` package)

### Build Targets

```bash
# ESXi
make esxi                           # Cross-compile binary
make vib                            # Binary + VIB package
ESXI_HOST=192.168.1.100 make deploy # Build + install on ESXi host

# Linux
make linux                          # Native binary
make deb                            # Binary + .deb package
make rpm                            # Binary + .rpm package

# Other
make clean                          # Remove build artifacts
make help                           # Show all targets
```

## USB Safety: Avoiding vmkusb Kernel Panics (ESXi)

ESXi 6.5's `vmkusb` driver contains a race condition in `udev_reapurb_sub` that causes Purple Screen of Death (PSOD) under concurrent USB traffic. This section documents the bug and hwmond's workaround for anyone writing userspace USB drivers on ESXi.

### Root Cause

The function `udev_reapurb_sub` acquires a spinlock at device offset `+0x168`, locates the completed URB, then **releases the spinlock before accessing the URB data**. The completion callback (running on an interrupt) can free the URB between unlock and access, causing a use-after-free PSOD.

The Linux-standard `REAPURBNDELAY` ioctl (`0x4008550D`, nr=13) is not recognized by vmkusb's remapped dispatch table (which uses nr=18). It falls through to the buggy default handler.

### The Fix

hwmond uses ESXi's native `REAPURB` ioctl (`0xC0105512`, nr=18) which dispatches to `udev_handle_ioctl` — the properly locked code path. This is the same ioctl ESXi's own `libusb-1.0.so` uses internally.

```
SUBMITURB  ->  poll(POLLOUT)  ->  REAPURB (0xC0105512)
```

On Linux, standard `REAPURBNDELAY` is used — the Linux kernel does not have this bug.

### Approaches That Caused PSODs

| Approach | Failure Mode |
|----------|-------------|
| `DISCARDURB` after timeout | Race between discard and completion callback |
| `poll()` + `REAPURBNDELAY` (0x4008550D) | Wrong ioctl number, falls through to buggy handler |
| Any path through `udev_reapurb_sub` | Premature spinlock release, use-after-free |

### Validation

The safe sequence was stress tested with **100,000 consecutive USB operations at 37 ops/sec with zero failures** and zero kernel panics.

## Protocol Documentation

See [APPLE_OEM_IPMI_SPEC.md](APPLE_OEM_IPMI_SPEC.md) for the complete reverse-engineered Apple OEM IPMI parameter specification, including wire formats, binary layouts, string packing, and dictionary keys.

## License

MIT License. Based on [castvoid/xserve-frontpanel](https://github.com/castvoid/xserve-frontpanel).
