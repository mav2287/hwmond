# hwmond Linux Port — Complete Implementation Plan

## Overview

Port hwmond from ESXi to Linux, supporting all major distributions with emphasis on Proxmox VE. The daemon drives the Xserve 3,1 front panel CPU LEDs and populates the BMC for Apple Server Monitor — identical functionality to the ESXi version, using Linux-native data sources.

## Architecture

Same three-thread architecture as ESXi:

```
hwmond
├── CPU Thread (1 Hz)  — /proc/stat delta-based utilization
├── LED Thread (10 Hz) — USB SUBMITURB + poll + REAPURBNDELAY (standard Linux)
└── BMC Init + Update  — Apple OEM IPMI via /dev/ipmi0 (identical to ESXi)
```

### What's Identical (no changes)

| Component | Why |
|-----------|-----|
| BMC/IPMI protocol | `/dev/ipmi0`, Apple OEM NetFn 0x36, all wire formats, packed strings |
| LED bar graph logic | Smoothing, RAMP_STEP, FINE_DIVISOR, 10Hz update |
| IPMI parameter formats | All 15 parameters use the same binary + string packing |
| SEL time sync | Standard IPMI command, same on all platforms |
| Parameter clearing | Same Apple OEM clear commands |
| APPLE_OEM_IPMI_SPEC.md | Protocol is OS-independent |
| Graceful shutdown | NOT handled by hwmond — host OS handles natively (confirmed on ESXi, expected same on Linux) |

### What Changes

| Component | ESXi | Linux |
|-----------|------|-------|
| USB device path | `/dev/usb0502` | `/dev/bus/usb/BBB/DDD` |
| USB reap ioctl | `0xC0105512` (vmkusb bug workaround) | Standard `REAPURBNDELAY` (no bug in Linux kernel) |
| USB arbitrator | Stop `/etc/init.d/usbarbitrator` | Not needed |
| USB claim/exclusion | passthru.map + esxcli | udev rule + CLAIMINTERFACE |
| CPU monitoring | vsish popen per-PCPU | `/proc/stat` parser |
| CPU topology | `esxcli hardware cpu global get` | `/proc/cpuinfo` or `lscpu` |
| Firmware version | `vsish bios` | `/sys/class/dmi/id/bios_version` |
| Hostname | `esxcli system hostname` | `gethostname()` |
| OS version | `esxcli system version get` | `/etc/os-release` |
| CPU model | `vsish cpuModelName` | `/proc/cpuinfo` model name |
| Serial/model | `esxcli hardware platform` | `dmidecode -t system` or `/sys/class/dmi/id/` |
| Total RAM | `esxcli hardware memory` | `/proc/meminfo` MemTotal |
| Per-DIMM info | `smbiosDump` (Type 17, visor format) | `dmidecode -t memory` (standard format) |
| Drive inventory | `esxcli storage core device list` | `lsblk -J` + `/sys/block/` + `smartctl` |
| NIC info | `esxcli network nic list` | `ip link` + `/sys/class/net/` + `ethtool` |
| IP addresses | `esxcli network ip interface ipv4` | `ip -4 addr show` |
| Drive change detection | `ls /vmfs/devices/disks/ \| wc -l` | Poll `/sys/block/` or `inotify` |
| Network change detection | `stat /etc/vmware/esx.conf` | `stat /etc/network/interfaces` or netlink |
| Uptime | `/proc/uptime` (with fallback) | `/proc/uptime` (native, no fallback needed) |
| Packaging | VIB (visor tar + ar) | .deb + .rpm + install.sh |
| Service management | `/etc/rc.local.d/` script | systemd service |

---

## File Structure

```
src/
├── main.c                  — shared, compile-time #ifdef for defaults
├── panel_usb.c             — shared, #ifdef for ESXi ioctl workaround
├── panel_usb.h             — shared
├── bmc.c                   — shared IPMI protocol (write/clear/sync)
├── bmc.h                   — shared
├── cpu_usage.h             — shared interface (cpu_state_t, cpu_sample)
│
├── cpu_esxi.c              — ESXi: vsish + esxcli topology
├── cpu_linux.c             — Linux: /proc/stat + /proc/cpuinfo
│
├── collect_esxi.c          — ESXi: all esxcli/vsish/smbiosDump data collection
├── collect_linux.c         — Linux: /proc, /sys, dmidecode, ip, lsblk
├── collect.h               — shared interface for data collection
│
├── ipmi_dump.c             — diagnostic tool (portable)
├── usb_stress.c            — stress test (portable)
│
scripts/
├── build-vib.sh            — ESXi VIB packaging
├── hwmond-startup.sh       — ESXi boot script
├── hwmond-init.sh          — ESXi lifecycle hooks
├── hwmond.service          — Linux systemd service (NEW)
├── 99-xserve-panel.rules   — Linux udev rule (NEW)
├── build-deb.sh            — Debian package builder (NEW)
├── build-rpm.sh            — RPM package builder (NEW)
├── install.sh              — Universal installer (NEW)
│
packaging/
├── debian/                 — .deb control files (NEW)
│   ├── control
│   ├── postinst
│   ├── prerm
│   └── conffiles
├── rpm/                    — .rpm spec file (NEW)
│   └── hwmond.spec
│
Makefile                    — updated with linux/deb/rpm targets
```

---

## Detailed Implementation

### Phase 1: Refactor — Split Platform Code

**Goal:** Separate ESXi-specific code from shared code without breaking the current ESXi build.

#### 1.1 Extract data collection from bmc.c into collect_esxi.c

Move these functions out of bmc.c:
- `collect_drives()` — esxcli storage parsing
- `collect_nics()` — esxcli network parsing
- `collect_net_dynamic()` — esxcli + ip parsing
- `count_disk_devices()` — /vmfs/ listing
- All `popen_field()` / `popen_line()` calls for system data in `bmc_init()`

Create `collect.h` with a shared interface:
```c
/* collect.h — platform-independent data collection interface */

typedef struct {
    char firmware[256];
    char hostname[256];
    char fqdn[256];
    char os_product[256];
    char os_version[256];
    char os_update[256];
    char os_build[256];
    char cpu_model[256];
    int cpu_packages;
    int cpu_cores;
    int cpu_speed_mhz;
    char serial[256];
    char model[256];
    uint32_t total_ram_mb;
} system_info_t;

int collect_system_info(system_info_t *info);
int collect_dimms(struct dimm_info *dimms, int max);
int collect_drives(/* drive cache structs */);
int collect_nics(/* nic cache structs */);
int collect_net_dynamic(/* net_dynamic cache structs */);
int detect_drive_changes(void);
int detect_network_changes(void);
```

bmc.c calls these functions instead of doing its own popen/parsing.

#### 1.2 Extract CPU monitoring into cpu_esxi.c

Move all vsish-specific code from cpu_usage.c into cpu_esxi.c.
cpu_usage.c becomes a thin wrapper that calls platform-specific functions.
cpu_usage.h interface stays the same — `cpu_init()`, `cpu_sample()`, `cpu_state_t`.

#### 1.3 Add #ifdef to panel_usb.c

```c
#ifdef __ESXI__
    /* ESXi: use native REAPURB ioctl to avoid vmkusb bug */
    ret = ioctl(fd, (int)0xC0105512u, &reap_buf);
#else
    /* Linux: standard REAPURBNDELAY works fine */
    void *reap_ptr = NULL;
    ret = ioctl(fd, USBDEVFS_REAPURBNDELAY, &reap_ptr);
#endif
```

Also add standard Linux device path discovery:
```c
#ifdef __ESXI__
    /* /dev/usb0502 format */
    snprintf(path, pathlen, "/dev/usb%02d%02d", bus, dev);
#else
    /* /dev/bus/usb/005/002 format */
    snprintf(path, pathlen, "/dev/bus/usb/%03d/%03d", bus, dev);
#endif
```

#### 1.4 Verify ESXi build still works

```bash
make esxi    # must produce identical binary to current
```

---

### Phase 2: Linux CPU Monitoring (cpu_linux.c)

**Source:** `/proc/stat`

```
cpu  1234 5678 9012 345678 ...
cpu0 1000 2000 3000 100000 ...
cpu1  234 3678 6012 245678 ...
```

#### Implementation:

```c
/* Read /proc/stat, compute per-CPU deltas */
int cpu_sample(cpu_state_t *state)
{
    FILE *fp = fopen("/proc/stat", "r");
    /* Parse "cpuN user nice system idle iowait irq softirq" lines */
    /* Compute: used = user + nice + system + irq + softirq */
    /* Compute: total = used + idle + iowait */
    /* Delta: usage = delta_used / delta_total */
    /* Aggregate per-package using /proc/cpuinfo physical id mapping */
    fclose(fp);
}
```

#### Topology detection:

```c
/* Parse /proc/cpuinfo for:
 *   "physical id" — which socket/package
 *   "cpu cores"   — cores per package
 *   "model name"  — CPU model string
 * Or use lscpu for cleaner parsing */
int cpu_detect_topology(cpu_state_t *state)
{
    /* Read /proc/cpuinfo */
    /* Map each logical CPU to its physical package */
    /* Count packages, cores per package */
}
```

#### Key differences from ESXi:
- No vsish — read `/proc/stat` directly (faster, simpler)
- No popen — direct file reads
- `/proc/cpuinfo` for topology instead of `esxcli hardware cpu`
- HT correction same as ESXi (divide by logical CPUs per physical core)

---

### Phase 3: Linux Data Collection (collect_linux.c)

#### 3.1 System Info

```c
/* Firmware: /sys/class/dmi/id/bios_version (no root needed) */
read_file("/sys/class/dmi/id/bios_version", info->firmware);

/* Hostname: gethostname() syscall */
gethostname(info->hostname, sizeof(info->hostname));

/* FQDN: getaddrinfo() or /etc/hostname */
/* Falls back to hostname if DNS not configured */

/* OS: /etc/os-release */
/* PRETTY_NAME="Proxmox VE 8.1" or "Debian GNU/Linux 12" */
parse_os_release("/etc/os-release", info);

/* CPU model: /proc/cpuinfo "model name" line */
read_cpuinfo_field("model name", info->cpu_model);

/* Serial/Model: /sys/class/dmi/id/ (no root for most fields) */
read_file("/sys/class/dmi/id/product_serial", info->serial);
read_file("/sys/class/dmi/id/product_name", info->model);
/* Fallback: dmidecode -t system (requires root) */

/* Total RAM: /proc/meminfo MemTotal */
/* Round to nearest GB same as ESXi version */
parse_meminfo(&info->total_ram_mb);
```

#### 3.2 Per-DIMM Memory Info

```c
/* dmidecode -t memory (Type 17) — requires root */
/* Standard dmidecode format (NOT visor/smbiosDump): */
/*
 * Memory Device
 *     Size: 16 GB
 *     Locator: DIMM_A1
 *     Type: DDR3
 *     Speed: 1066 MT/s
 */
/* Parser is simpler than ESXi smbiosDump — standard dmidecode format */
int collect_dimms(struct dimm_info *dimms, int max)
{
    FILE *fp = popen("dmidecode -t memory 2>/dev/null", "r");
    /* Parse "Memory Device" sections */
    /* Fields: Size, Locator, Type, Speed */
    /* Skip entries with "No Module Installed" */
}
```

#### 3.3 Drive Inventory

```c
/* lsblk for device list, /sys/block/ for details */
int collect_drives(void)
{
    /* List block devices: lsblk -d -n -o NAME,SIZE,TYPE,TRAN,MODEL,VENDOR,REV */
    /* Filter: TYPE=disk only (skip partitions, loops, etc.) */
    /* Transport from TRAN field: sata, sas, nvme, usb */
    /* Size from SIZE field (human-readable) or /sys/block/DEV/size (sectors) */
    /* Model from MODEL field */
    /* Vendor from VENDOR field */
    /* SSD detection: /sys/block/DEV/queue/rotational (0=SSD, 1=HDD) */
}
```

#### 3.4 Network Interfaces

```c
/* ip link + /sys/class/net/ */
int collect_nics(void)
{
    /* List interfaces: ls /sys/class/net/ */
    /* Skip: lo, veth*, br*, tap*, bond* (virtual interfaces) */
    /* MAC: /sys/class/net/DEV/address */
    /* Speed: /sys/class/net/DEV/speed (in Mbps) or ethtool */
    /* Duplex: /sys/class/net/DEV/duplex */
    /* Link: /sys/class/net/DEV/operstate (up/down) → "active"/"inactive" */
    /* Driver: /sys/class/net/DEV/device/driver → basename */
}

int collect_net_dynamic(void)
{
    /* IPs: ip -4 addr show DEV → parse inet lines */
    /* Stats: /sys/class/net/DEV/statistics/rx_bytes etc. */
}
```

#### 3.5 Change Detection

```c
/* Drives: poll /sys/block/ directory mtime */
/* Or: count entries, compare to cached count (same as ESXi approach) */
int detect_drive_changes(void)
{
    /* count /sys/block/sd* /sys/block/nvme* entries */
    /* compare to cached count */
}

/* Network: stat /etc/network/interfaces or /etc/netplan/ */
/* Or: monitor /sys/class/net/ for new/removed interfaces */
int detect_network_changes(void)
{
    struct stat st;
    stat("/etc/network/interfaces", &st);
    /* compare mtime to cached */
}
```

---

### Phase 4: USB Adjustments (panel_usb.c)

#### 4.1 Device Discovery

```c
#ifndef __ESXI__
/* Standard Linux: /dev/bus/usb/BBB/DDD */
static int build_device_path(char *path, size_t pathlen, int bus, int dev)
{
    snprintf(path, pathlen, "/dev/bus/usb/%03d/%03d", bus, dev);
    if (access(path, F_OK) == 0) return 0;
    return -1;
}

/* Scan /dev/bus/usb/ for VID:PID match */
/* Read USB device descriptor from each device file */
/* Match against 05ac:8261 */
static int scan_usb_devices(char *path, size_t pathlen)
{
    /* For each /dev/bus/usb/BBB/DDD:
     *   open, read 18-byte device descriptor
     *   check VID:PID at bytes 8-11
     *   if match, return path */
}
#endif
```

#### 4.2 Reap Ioctl

```c
static int submit_poll_reap(int fd, uint8_t endpoint, void *buf, int len)
{
    /* ... submit and poll same as current ... */

#ifdef __ESXI__
    /* ESXi vmkusb bug: use native ioctl 0xC0105512 → udev_handle_ioctl */
    struct { void *urb_ptr; uint64_t pad; } reap_buf;
    memset(&reap_buf, 0, sizeof(reap_buf));
    ret = ioctl(fd, (int)0xC0105512u, &reap_buf);
#else
    /* Linux: standard REAPURBNDELAY — no vmkusb bug */
    void *reap_ptr = NULL;
    ret = ioctl(fd, USBDEVFS_REAPURBNDELAY, &reap_ptr);
#endif
    if (ret < 0) return -2;
    return 0;
}
```

#### 4.3 No USB Arbitrator

The ESXi startup script stops the USB arbitrator. Linux doesn't have one. The udev rule handles exclusion instead.

---

### Phase 5: Packaging

#### 5.1 systemd Service

```ini
# /etc/systemd/system/hwmond.service
[Unit]
Description=Apple Xserve Hardware Monitor (LED + BMC)
After=network.target
Wants=network.target

[Service]
Type=simple
ExecStart=/usr/local/sbin/hwmond
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal
SyslogIdentifier=hwmond

# Security hardening
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
NoNewPrivileges=false
# Needs root for /dev/ipmi0 and USB device access

[Install]
WantedBy=multi-user.target
```

#### 5.2 udev Rule

```
# /etc/udev/rules.d/99-xserve-panel.rules
# Apple Xserve front panel USB device — claimed by hwmond
# Prevents USB passthrough to VMs (Proxmox/QEMU/libvirt)
SUBSYSTEM=="usb", ATTR{idVendor}=="05ac", ATTR{idProduct}=="8261", \
    MODE="0600", OWNER="root", GROUP="root", \
    TAG-="uaccess", \
    ENV{ID_XSERVE_PANEL}="1"
```

#### 5.3 Debian Package (.deb)

```
packaging/debian/control:
  Package: hwmond-xserve
  Version: 3.0.0
  Architecture: amd64
  Maintainer: hwmond
  Depends: dmidecode, ipmitool
  Recommends: smartmontools, ethtool
  Section: admin
  Priority: optional
  Description: Apple Xserve hardware monitor for Linux
   Drives front panel CPU activity LEDs and populates BMC
   for Apple Server Monitor on Xserve 3,1 servers.

packaging/debian/postinst:
  #!/bin/sh
  systemctl daemon-reload
  udevadm control --reload-rules
  udevadm trigger
  systemctl enable hwmond
  systemctl start hwmond

packaging/debian/prerm:
  #!/bin/sh
  systemctl stop hwmond
  systemctl disable hwmond
```

Build script:
```bash
# scripts/build-deb.sh
mkdir -p deb-root/usr/local/sbin
mkdir -p deb-root/etc/systemd/system
mkdir -p deb-root/etc/udev/rules.d
mkdir -p deb-root/DEBIAN

cp build/hwmond deb-root/usr/local/sbin/
cp scripts/hwmond.service deb-root/etc/systemd/system/
cp scripts/99-xserve-panel.rules deb-root/etc/udev/rules.d/
cp packaging/debian/* deb-root/DEBIAN/

dpkg-deb --build deb-root hwmond-xserve-3.0.0.deb
```

#### 5.4 RPM Package

```spec
# packaging/rpm/hwmond.spec
Name:    hwmond-xserve
Version: 3.0.0
Release: 1
Summary: Apple Xserve hardware monitor for Linux
License: MIT
Requires: dmidecode, ipmitool

%description
Drives front panel CPU activity LEDs and populates BMC
for Apple Server Monitor on Xserve 3,1 servers.

%install
install -D -m 755 %{_builddir}/hwmond %{buildroot}/usr/local/sbin/hwmond
install -D -m 644 %{_builddir}/hwmond.service %{buildroot}/etc/systemd/system/hwmond.service
install -D -m 644 %{_builddir}/99-xserve-panel.rules %{buildroot}/etc/udev/rules.d/99-xserve-panel.rules

%post
systemctl daemon-reload
udevadm control --reload-rules
systemctl enable hwmond
systemctl start hwmond

%preun
systemctl stop hwmond
systemctl disable hwmond
```

#### 5.5 Universal Installer

```bash
#!/bin/sh
# scripts/install.sh — works on any distro with systemd
set -e

echo "Installing hwmond for Apple Xserve..."

# Binary
install -m 755 hwmond /usr/local/sbin/hwmond

# systemd service
install -m 644 hwmond.service /etc/systemd/system/hwmond.service

# udev rule
install -m 644 99-xserve-panel.rules /etc/udev/rules.d/

# Activate
systemctl daemon-reload
udevadm control --reload-rules
udevadm trigger
systemctl enable hwmond
systemctl start hwmond

echo "hwmond installed and running."
echo "Check status: systemctl status hwmond"
echo "View logs: journalctl -u hwmond -f"
```

---

### Phase 6: Makefile Updates

```makefile
# Platform detection
UNAME := $(shell uname -s)

# ESXi (cross-compile)
esxi:
	zig cc -target x86_64-linux-gnu.2.12 -D__ESXI__ -O2 \
	  -o build/hwmond src/main.c src/panel_usb.c src/cpu_esxi.c \
	  src/collect_esxi.c src/bmc.c -lpthread -lm -lrt

# Linux (native compile)
linux:
	gcc -D__LINUX__ -O2 -Wall \
	  -o build/hwmond src/main.c src/panel_usb.c src/cpu_linux.c \
	  src/collect_linux.c src/bmc.c -lpthread -lm -lrt

# Packages
vib: esxi
	./scripts/build-vib.sh build/hwmond build/hwmond-xserve.vib

deb: linux
	./scripts/build-deb.sh build/hwmond build/hwmond-xserve-3.0.0.deb

rpm: linux
	./scripts/build-rpm.sh build/hwmond build/hwmond-xserve-3.0.0.rpm
```

---

### Phase 7: Testing

#### 7.1 Build Verification
```bash
make esxi      # must still produce working ESXi binary
make linux     # must compile on Linux
make deb       # must produce valid .deb
make rpm       # must produce valid .rpm
```

#### 7.2 USB Stress Test
```bash
# Run existing usb_stress tool on Linux
# Standard REAPURBNDELAY should pass easily — no vmkusb bug
./usb_stress /dev/bus/usb/005/002 100000
```

#### 7.3 Functional Tests (on actual Xserve hardware)
- [ ] LEDs respond to CPU load
- [ ] All 15 BMC parameters populate correctly
- [ ] Apple Server Monitor reads data via LOM
- [ ] DIMM slots show correctly
- [ ] Drives show correctly
- [ ] NICs show link/speed/IP
- [ ] Uptime updates
- [ ] SEL time syncs on startup
- [ ] Service starts on boot (systemctl)
- [ ] Service stops cleanly (SIGTERM → LEDs off)
- [ ] udev rule blocks USB passthrough
- [ ] No log spam in steady state
- [ ] dmidecode output parses correctly (different format from smbiosDump)
- [ ] /proc/stat CPU monitoring accurate

#### 7.4 Distribution Testing
- [ ] Debian 12 (Bookworm)
- [ ] Proxmox VE 8.x
- [ ] Ubuntu 22.04 / 24.04
- [ ] RHEL 9 / Rocky 9
- [ ] Fedora (latest)

---

### Implementation Order

| Step | What | Depends on | Risk |
|------|------|-----------|------|
| 1 | Refactor: split platform code | Nothing | Low — no functional changes |
| 2 | Verify ESXi build still works | Step 1 | Must pass before continuing |
| 3 | cpu_linux.c (/proc/stat) | Step 1 | Low — well-documented format |
| 4 | collect_linux.c (system info) | Step 1 | Low — /sys and /proc are stable |
| 5 | collect_linux.c (DIMM parser) | Step 1 | Medium — dmidecode format varies |
| 6 | collect_linux.c (drives) | Step 1 | Low — lsblk is standard |
| 7 | collect_linux.c (network) | Step 1 | Low — /sys/class/net is standard |
| 8 | panel_usb.c #ifdefs | Step 1 | Low — standard Linux USB |
| 9 | systemd + udev + packaging | Steps 3-8 | Low — standard tooling |
| 10 | Test on Proxmox with real hardware | Steps 3-9 | Must have physical Xserve |
| 11 | .deb package | Step 9 | Low |
| 12 | .rpm package | Step 9 | Low |
| 13 | Update README + docs | All | — |
| 14 | GitHub release with all packages | All | — |

---

### Timeline Estimate

| Phase | Effort |
|-------|--------|
| Refactor (split platform code) | 2-3 hours |
| cpu_linux.c | 1 hour |
| collect_linux.c | 3-4 hours |
| USB #ifdefs | 30 minutes |
| Packaging (systemd + deb + rpm) | 2 hours |
| Testing on real hardware | 2-4 hours |
| Documentation | 1 hour |
| **Total** | **~12-15 hours** |

---

### Notes

- The IPMI code is 100% portable — `/dev/ipmi0` on Linux is the same interface as ESXi
- The USB front panel is the same hardware — just different device path and no vmkusb bug
- `dmidecode` requires root but so does `/dev/ipmi0` and USB device access — hwmond runs as root anyway
- Proxmox VE is Debian-based so the .deb package covers it directly
- The binary is statically linkable for maximum portability across distro versions
- Consider offering an ARM build for Raspberry Pi-based Xserve management stations (BMC-only, no USB LEDs)
