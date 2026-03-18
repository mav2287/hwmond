# hwmond — Apple Xserve Hardware Monitor for ESXi

A hardware monitoring daemon for Apple Xserve 3,1 (Early 2009) servers running VMware ESXi 6.5. Replicates Apple's original `hwmond` functionality: drives the front panel CPU activity LEDs and populates the BMC with system data readable by Apple Server Monitor.

## Features

- **Front Panel CPU Activity LEDs** — Drives the 2x8 LED bar graph at 10 Hz with smooth ramping that matches Apple's original visual behavior
- **BMC Data Population** — Writes comprehensive system information to the Xserve BMC via Apple's OEM IPMI protocol (NetFn 0x36):
  - System identity (hostname, OS version, Boot ROM version, serial number)
  - CPU information (model, speed, core count, package count)
  - Total memory and per-DIMM details (slot label, size, speed, type)
  - Drive inventory (model, manufacturer, capacity, bus type, SMART status)
  - Network interface status (MAC address, IP, link state, speed, duplex)
  - System uptime
- **Apple Server Monitor Compatible** — All data formatted to match the exact wire protocol expected by Apple Server Monitor and compatible management tools
- **Production Stable** — Designed for unlimited unattended uptime with safe USB handling, safe IPMI access patterns, and no kernel-crashing code paths
- **Automatic Startup** — Installs as a boot service; starts automatically on every ESXi boot with no manual intervention

## Installation

Pre-built VIB packages are available in the [`releases/`](releases/) directory.

### Step 1: Enable SSH on the ESXi Host

In the vSphere/ESXi web UI:

1. Navigate to **Host > Manage > Services**
2. Find **TSM-SSH** in the service list
3. Click **Start** to enable the SSH service

### Step 2: Set the Acceptance Level

SSH into the ESXi host and set the software acceptance level to allow community-supported VIBs:

```
ssh root@<your-esxi-host>
esxcli software acceptance set --level CommunitySupported
```

### Step 3: Upload the VIB

Transfer the VIB file to the ESXi host. You can use SCP from your local machine:

```
scp releases/hwmond-xserve-3.0.0.vib root@<your-esxi-host>:/tmp/
```

Alternatively, upload the file through the ESXi datastore browser in the web UI and note the path (e.g. `/vmfs/volumes/datastore1/hwmond-xserve-3.0.0.vib`).

### Step 4: Install the VIB

On the ESXi host via SSH:

```
esxcli software vib install -v /tmp/hwmond-xserve-3.0.0.vib --force --no-sig-check --no-live-install
```

The `--no-live-install` flag is required because ESXi 6.5 cannot live-install bootbank VIBs. The VIB will be staged for activation on the next boot.

### Step 5: Restart the Host

Restart the ESXi host through the vSphere/ESXi web UI:

1. Navigate to **Host**
2. Click **Reboot**

**Important:** Use the web UI reboot, NOT the `reboot` shell command. The web UI ensures all VMs are properly shut down and the VIB staging is finalized.

### Step 6: Verify

After the host restarts, hwmond starts automatically. You can verify it is running:

```
ps -c | grep hwmond
```

The front panel LEDs should be active, and Apple Server Monitor should show system data when pointed at the Xserve's LOM address.

## Removal

SSH into the ESXi host and run:

```
esxcli software vib remove -n hwmond-xserve
```

Then reboot the host through the web UI.

## What Gets Installed

The VIB places three files on the ESXi host:

| File | Purpose |
|------|---------|
| `/opt/hwmond/hwmond` | The daemon binary |
| `/etc/rc.local.d/hwmond-startup.sh` | Auto-start script (runs on every boot) |
| `/etc/init.d/hwmond` | Service lifecycle hooks (start/stop/status) |

## Architecture

```
hwmond
 |- CPU Thread (1 Hz)  -- vsish popen, delta-based utilization, uptime
 |- LED Thread (10 Hz) -- USB SUBMITURB + REAPURB, smooth ramp, bar graph
 '- BMC Init + Update (60s) -- Apple OEM IPMI, all parameters
```

- **LED Thread**: Reads CPU utilization, computes target LED levels with smooth ramping, and sends USB interrupt transfers to the front panel device at 10 Hz.
- **CPU Thread**: Reads all physical CPUs via `vsish` in interactive mode. Computes delta-based utilization with hyper-threading correction for accurate per-core readings.
- **BMC Thread**: Collects system information (hostname, OS version, CPU, memory, drives, NICs) and writes it to the BMC using Apple's OEM IPMI multi-block wire format.
- **IPMI Safety**: Opens `/dev/ipmi0` per-write, never holds the file descriptor open. Uses Apple's multi-block wire format with encoding markers and packed strings.

## USB Safety: Avoiding vmkusb Kernel Panics

This section documents critical findings about ESXi 6.5's USB subsystem that are essential for any userspace USB driver on this platform.

### The Problem

ESXi 6.5's `vmkusb` driver contains a race condition bug in the function `udev_reapurb_sub` (the URB reap subroutine). This function is reached through certain ioctl paths and will cause a Purple Screen of Death (PSOD) — ESXi's equivalent of a kernel panic — under concurrent USB traffic.

### Root Cause

The bug is a lock-ordering violation in `udev_reapurb_sub`:

1. The function acquires the spinlock at device offset `+0x168`
2. It locates the completed URB in the queue
3. **It releases the spinlock BEFORE accessing the URB data**
4. The completion callback (running on an interrupt context) can free or modify the URB between steps 3 and 4
5. The function dereferences stale/freed memory, causing a PSOD

### Why Standard Linux ioctls Trigger This

The Linux-standard `REAPURBNDELAY` ioctl has the number `0x4008550D` (ioctl nr=13). However, VMware remapped the usbdevfs ioctl dispatch table in vmkusb. The ioctl number that vmkusb expects for `REAPURB`-class operations uses nr=18, not nr=13.

When vmkusb receives the unrecognized Linux-standard ioctl number, it falls through to a default handler that calls `udev_reapurb_sub` — the buggy function with the premature spinlock release.

### The Fix

Use ESXi's native `REAPURB` ioctl (`0xC0105512`, nr=18) instead of the Linux-standard `REAPURBNDELAY` (`0x4008550D`, nr=13). The native ioctl dispatches to `udev_handle_ioctl`, which holds the spinlock for the entire duration of URB data access, eliminating the race condition.

This is the same ioctl that ESXi's own bundled `libusb-1.0.so` uses internally, confirmed by disassembly of the library.

### What hwmond Does

hwmond uses the following safe sequence for every USB transfer:

```
SUBMITURB  ->  poll(POLLOUT)  ->  REAPURB (0xC0105512)
```

The `poll()` call blocks until the URB completes (the kernel signals `POLLOUT` on completion). The native `REAPURB` ioctl then retrieves the completed URB through the safe, properly-locked code path.

### Approaches That Caused PSODs

During development, the following approaches each caused kernel panics:

| Approach | Failure Mode |
|----------|-------------|
| `DISCARDURB` after timeout | Race between discard and completion callback — PSOD in spinlock code |
| `poll()` + `REAPURBNDELAY` (0x4008550D) | Wrong ioctl number for vmkusb dispatch table — falls through to buggy `udev_reapurb_sub` |
| Any path through `udev_reapurb_sub` | Premature spinlock release creates use-after-free race |

### Stress Test Results

The safe `SUBMITURB` + `poll` + `REAPURB` sequence was stress tested with **100,000 consecutive USB operations at 37 ops/sec with zero failures** and zero kernel panics.

## Building from Source

Requires [Zig](https://ziglang.org/) (for cross-compilation targeting ESXi's glibc 2.12 ABI):

```bash
# Build the binary only
make zig

# Build + package as installable VIB
make vib

# Deploy directly to an ESXi host (set ESXI_HOST)
ESXI_HOST=192.168.1.100 make deploy
```

The build produces a dynamically-linked x86_64 Linux binary compatible with ESXi 6.5's glibc 2.12 runtime.

### Diagnostic Tools

The `src/` directory includes standalone IPMI utilities for debugging:

- **`ipmi_dump.c`** — Reads all Apple BMC parameters and displays raw data
- **`ipmi_one.c`** — Writes a single IPMI parameter for testing
- **`ipmi_probe.c`** — Probes IPMI device availability

Build individually with:

```bash
zig cc -target x86_64-linux-gnu.2.12 -O2 -o build/ipmi_dump src/ipmi_dump.c
```

## Protocol Documentation

See [APPLE_OEM_IPMI_SPEC.md](APPLE_OEM_IPMI_SPEC.md) for the complete reverse-engineered Apple OEM IPMI parameter specification, including wire formats, binary layouts, string packing, and dictionary keys for every BMC parameter.

## Hardware Compatibility

| Component | Details |
|-----------|---------|
| Server | Apple Xserve 3,1 (Early 2009) |
| Processors | Dual Intel Xeon W5590 (or compatible LGA 1366) |
| Memory | 12 DIMM slots, DDR3 ECC, up to 192 GB |
| Storage | 3x SAS/SATA drive bays |
| Front Panel USB | VID `05AC`, PID `8261` (Apple Xserve front panel controller) |
| BMC/LOM | Built-in Lights-Out Management accessible via IPMI |
| Hypervisor | VMware ESXi 6.5 (Build 20502893 tested) |

## License

MIT License. Based on [castvoid/xserve-frontpanel](https://github.com/castvoid/xserve-frontpanel).
