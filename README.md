# hwmond — Apple Xserve Hardware Monitor for ESXi

A hardware monitoring daemon for Apple Xserve 3,1 servers running VMware ESXi 6.5. Replicates the functionality of Apple's original `hwmond`, driving the front panel CPU activity LEDs and populating the BMC with system data readable by Apple Server Monitor.

## Features

- **Front Panel LEDs** — Drives the CPU activity LED bar graph (2 rows of 8 LEDs) with smooth 10 Hz updates matching Apple's original visual behavior
- **BMC Data Population** — Writes system information to the Xserve BMC via Apple's OEM IPMI protocol (NetFn 0x36), including:
  - System identity (hostname, OS version, Boot ROM, serial number)
  - CPU info (model, speed, core count)
  - Total memory and per-DIMM details (slot, size, speed, type)
  - Drive inventory (model, manufacturer, capacity, bus type)
  - Network interface status (MAC, IP, link state, speed, duplex)
  - System uptime
- **Apple Server Monitor Compatible** — All data formatted to match the exact wire protocol expected by Apple Server Monitor and compatible tools
- **Production Stable** — Designed for unlimited unattended uptime with safe USB handling, open-write-close IPMI patterns, and no kernel-crashing code paths

## Architecture

```
hwmond
├── CPU Thread (1 Hz) — vsish popen, delta-based utilization, uptime
├── LED Thread (10 Hz) — USB SUBMITURB+poll, smooth ramp, bar graph
└── BMC Init + Update (60s) — Apple OEM IPMI, all parameters
```

- **USB Safety**: Uses `SUBMITURB` → `poll(POLLOUT)` → `REAPURBNDELAY`. The poll acquires the same vmkusb spinlock as the completion callback. Never calls `DISCARDURB` (causes PSOD).
- **IPMI Safety**: Opens `/dev/ipmi0` per-write, never holds the fd open. Uses Apple's multi-block wire format with encoding markers and packed strings.
- **CPU Monitoring**: Reads all PCPUs via `vsish` in interactive mode. Delta-based utilization with HT correction.

## Building

Requires [Zig](https://ziglang.org/) for cross-compilation targeting ESXi's glibc 2.12:

```bash
make zig        # Build the binary
make vib        # Build + package as VIB
```

## Installation

```bash
# Copy VIB to ESXi host
scp build/hwmond-xserve.vib root@<host>:/tmp/

# Install (requires reboot — ESXi 6.5 cannot live-install bootbank VIBs)
esxcli software acceptance set --level CommunitySupported
esxcli software vib install -v /tmp/hwmond-xserve.vib --force --no-sig-check --no-live-install

# Reboot to activate
reboot
```

The VIB installs:
- `/opt/hwmond/hwmond` — the daemon binary
- `/etc/rc.local.d/hwmond-startup.sh` — auto-start on boot
- `/etc/init.d/hwmond` — service lifecycle hooks

## Removal

```bash
esxcli software vib remove -n hwmond-xserve
```

## Protocol Documentation

See [APPLE_OEM_IPMI_SPEC.md](APPLE_OEM_IPMI_SPEC.md) for the complete reverse-engineered Apple OEM IPMI parameter specification, including wire formats, binary layouts, string packing, and dictionary keys for every parameter.

## Diagnostic Tools

The `src/` directory includes standalone IPMI utilities:

- `ipmi_dump.c` — Reads all Apple BMC parameters and displays raw data
- `ipmi_one.c` — Writes a single IPMI parameter for testing
- `ipmi_probe.c` — Probes IPMI device availability

Build with: `zig cc -target x86_64-linux-gnu.2.12 -O2 -o build/ipmi_dump src/ipmi_dump.c`

## Hardware

Tested on Apple Xserve 3,1 (Early 2009) with:
- Dual Intel Xeon W5590 processors
- 12 DIMM slots (DDR3 ECC, up to 192 GB)
- SAS/SATA drive bays
- Front panel USB device (VID 05AC, PID 8261)
- BMC/LOM (Lights-Out Management)

## License

Based on [castvoid/xserve-frontpanel](https://github.com/castvoid/xserve-frontpanel) (MIT License).
