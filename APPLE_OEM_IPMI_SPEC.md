# Apple Xserve OEM IPMI Parameter Specification

Reverse-engineered from Apple's PlatformHardwareManagement framework, Server Monitor application, and hwmond binary.

## BMC Identity

| Field | Value |
|-------|-------|
| Manufacturer ID | 63 (0x3F) — Apple Computer, Inc. |
| Product ID | 0x0002 (Xserve 3,1) |
| IPMI Version | 2.0 |
| Firmware | 1.1.9 |
| Cipher Suite | 3 (HMAC-SHA1 / HMAC-SHA1-96 / AES-CBC-128) |
| KCS I/O Port | 0xCA2 (not advertised via ACPI/SMBIOS) |

**Known quirks:**
- SDR Repository: non-standard, `Get SDR` returns unspecified error
- FRU: non-standard, returns "Unknown header version 0xff"
- LAN Config: returns "Invalid channel" on standard channels
- Fan Fault flag: always set under ESXi (known firmware bug)
- Open Session algorithm payloads must be 8 bytes each (not 4)
- `Get Channel Cipher Suites` is required before `Open Session`

## Command Table (NetFn 0x36)

| Cmd | Function | Data | Direction |
|-----|----------|------|-----------|
| 0x01 | Set Apple System Info Parameter | [param, set_sel, block, payload...] | Write |
| 0x02 | Get Apple System Info Parameter | [0x00, param, set_sel, block] | Read |
| 0x04 | Graceful Chassis Control | [action] | Write (expects response) |
| 0x05 | Graceful Chassis Control Response | [action] | Write (fire-and-forget) |
| 0x20 | Get LAN Channel-to-Port Map | unknown | Read |
| 0x21 | Set LAN Channel-to-Port Map | unknown | Write |
| 0x80 | Debug Read BMC Memory | unknown | Read |

## IPMI Transport (Cmd 0x01 / 0x02)

- **NetFn**: `0x36` (Apple OEM)
- **Get Command**: `0x02`
- **Set Command**: `0x01`

### Get Request
```
Data: [0x00, param, set_selector, block_number]
```

### Get Response
```
Block 0: [completion_code, revision, total_len_lo, total_len_hi, payload...]
Block N: [completion_code, block_number, payload...]
```

### Set Request (Block 0)
```
Data: [param, set_selector, 0x00, total_len_lo, total_len_hi, payload(max 30 bytes)]
```

### Set Request (Block N > 0)
```
Data: [param, set_selector, block_number, payload(max 32 bytes)]
```

## Wire Format for Payload

Every parameter's payload follows this structure:

```
[binary_data (N bytes)] [0x01 encoding=UTF-8] [packed_strings...]
```

### String Packing

Each string is packed as:
```
[length_byte] [string_bytes] [0x00 null] [0x00 pad]
```

Where `length_byte = strlen + 2` (includes the null terminator + pad byte).

For empty/null strings:
```
[0x01] [0x00]
```

### Multi-Block

If total payload > 30 bytes, it spans multiple blocks:
- Block 0 holds up to 30 bytes of payload (after the 2-byte total_len header)
- Each subsequent block holds up to 32 bytes
- Reassemble all blocks in order to get the full payload

---

## Parameters

### 0x01 — Firmware Version (Boot ROM)
| Field | Value |
|-------|-------|
| Binary | 0 bytes |
| Strings | 1: Firmware version string |
| Set Selector | 0 |

Example string: `"MP31.006C.B09"`

---

### 0x02 — System Name
| Field | Value |
|-------|-------|
| Binary | 0 bytes |
| Strings | 1: Hostname |
| Set Selector | 0 |

Example string: `"XserveUpper"`

---

### 0x03 — Primary OS
| Field | Value |
|-------|-------|
| Binary | 0 bytes |
| Strings | 3: [Product, Version, Update] |
| Set Selector | 0 |

Example strings: `["VMware ESXi", "6.5.0", "Update 3"]`

---

### 0x04 — Current OS
| Field | Value |
|-------|-------|
| Binary | 0 bytes |
| Strings | 3: [Product, Version, Build] |
| Set Selector | 0 |

Example strings: `["VMware ESXi", "6.5.0", "Build 20502893"]`

---

### 0xC0 — Processor Info
| Field | Value |
|-------|-------|
| Binary | 12 bytes (3 x uint32 LE) |
| Strings | 1: CPU model name |
| Set Selector | 0 |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0-3 | uint32 LE | Packages | Number of CPU packages/sockets |
| 4-7 | uint32 LE | Speed | CPU speed in MHz (SM divides by 1000 for GHz display) |
| 8-11 | uint32 LE | CoresPerPackage | Cores per physical package |

**String:** CPU model name (e.g. `"Intel(R) Xeon(R) CPU W5590"`)

Server Monitor displays: `"{Packages} x {Speed/1000} GHz {ModelString}"`

---

### 0xC1 — Miscellaneous Info
| Field | Value |
|-------|-------|
| Binary | 4 bytes (1 x uint32 LE) |
| Strings | 2: [Model, Serial] |
| Set Selector | 0 |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0-3 | uint32 LE | **RAM** | **Total installed RAM in MB** |

This is where Server Monitor reads the total memory value.

**Strings:**
1. Model — hardware model identifier (e.g. `"Xserve3,1"`)
2. Serial — serial number (e.g. `"CK2Y1234567"`)

---

### 0xC2 — Memory Info (per DIMM)
| Field | Value |
|-------|-------|
| Binary | 6 bytes |
| Strings | 3: [Slot, Speed, Type] |
| Set Selector | DIMM index (0, 1, 2, ...) |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0 | uint8 | ConfigurationType | 0x00 = populated, 0xFF = empty slot |
| 1 | uint8 | (ecc flag) | 0x00 = no ECC display, 0x02 = show ECC errors |
| 2-5 | uint32 LE | Size | DIMM size in MB |

**Strings:**
1. Slot — slot label (e.g. `"A1"`, `"B3"`)
2. Speed — memory speed (e.g. `"1066 MHz"`)
3. Type — memory type (e.g. `"DDR3"`)

**Loop termination:** Server Monitor reads set_selectors starting at 0, incrementing until the Get command returns an error. For each slot:
- If firmware_supports_extra (BMC FW >= 4): `ConfigurationType == 0xFF` → skip (empty)
- Otherwise: `ConfigurationType == 0` → populated, anything else → skip

---

### 0xC9 — Memory Dynamic Info (per DIMM)
| Field | Value |
|-------|-------|
| Binary | 16 bytes (4 x uint32 LE) |
| Strings | 0 |
| Set Selector | DIMM index (matches 0xC2) |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0-3 | uint32 LE | Summary | ECC error summary |
| 4-7 | uint32 LE | ParityErrorCount | Parity error count |
| 8-11 | uint32 LE | ParityErrorResetBaseline | Baseline (masked & 0x7FFFFFFF) |
| 12-15 | uint32 LE | (reserved) | |

All zeros = no ECC errors.

---

### 0xC3 — Drive Static Info (per drive)
| Field | Value |
|-------|-------|
| Binary | 8 bytes |
| Strings | 5: [Kind, Manufacturer, Model, Interconnect, Location] |
| Set Selector | Drive index (0, 1, 2, ...) |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0-3 | int32 LE | **Capacity** | Drive capacity in **MB** (uses `numberWithInt:`) |
| 4-7 | uint32 LE | (reserved) | Must be 0 |

**IMPORTANT:** Capacity is read as a **signed 32-bit integer** via `numberWithInt:`. Send MB, not bytes. Max representable: ~2 PB.

**Strings:**
1. Kind — drive type (e.g. `"SSD"`, `"HDD"`)
2. Manufacturer — vendor name (e.g. `"Samsung"`, `"OWC"`, `"APPLE"`)
3. Model — model string (e.g. `"Samsung SSD 970"`)
4. Interconnect — bus type (e.g. `"SAS"`, `"SATA"`, `"NVMe"`, `"Fibre Channel"`)
5. Location — physical location (e.g. `"Bay 1"`)

---

### 0xC5 — Drive Dynamic Info (per drive)
| Field | Value |
|-------|-------|
| Binary | 36 bytes (9 x uint32 LE) |
| Strings | 2: [SMARTMessage, RaidLevel] |
| Set Selector | Drive index (matches 0xC3) |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0-7 | uint64 LE | BytesRead | Total bytes read |
| 8-15 | uint64 LE | BytesWritten | Total bytes written |
| 16-19 | uint32 LE | (unknown) | |
| 20-23 | uint32 LE | ReadErrors | Read error count |
| 24-27 | uint32 LE | WriteErrors | Write error count |
| 28-31 | int32 LE | RaidStatus | RAID status (only stored if negative/flagged) |
| 32-35 | uint32 LE | Rebuild-Progress | RAID rebuild progress |

**Strings:**
1. SMARTMessage — SMART status message (empty if healthy)
2. RaidLevel — RAID level string (empty if not RAID)

**Server Monitor requires BOTH 0xC3 and 0xC5 to display drive entries.**

---

### 0xC4 — Network Static Info (per NIC)
| Field | Value |
|-------|-------|
| Binary | 0 bytes |
| Strings | 3: [HWAddress, UserDefinedName, Name] |
| Set Selector | NIC index (0, 1, ...) |

**Strings:**
1. HWAddress — MAC address (e.g. `"00:24:36:f3:31:ae"`)
2. UserDefinedName — user-assigned name (e.g. `"vmnic0"`)
3. Name — interface name (e.g. `"vmnic0"`)

---

### 0xC6 — Network Dynamic Info (per NIC)
| Field | Value |
|-------|-------|
| Binary | 20 bytes (5 x uint32 LE) |
| Strings | 5: [IPAddress, SubNetMask, Link, Mbps, DuplexMode] |
| Set Selector | NIC index (matches 0xC4) |

**Binary layout:**
| Offset | Type | Key in Server Monitor | Description |
|--------|------|----------------------|-------------|
| 0-3 | uint32 LE | PacketsIn | Packets received |
| 4-7 | uint32 LE | PacketsOut | Packets sent |
| 8-11 | uint32 LE | BytesIn | Bytes received |
| 12-15 | uint32 LE | BytesOut | Bytes sent |
| 16-19 | uint32 LE | (reserved) | |

**Strings:**
1. IPAddress — IPv4 address (e.g. `"192.168.1.100"`)
2. SubNetMask — subnet mask (e.g. `"255.255.255.0"`)
3. Link — link status: **`"active"`** = up, anything else = down
4. Mbps — speed string (e.g. `"1000"`)
5. DuplexMode — duplex mode (e.g. `"Full"`, `"Half"`)

**IMPORTANT:** The Link string must be exactly `"active"` (lowercase) for Server Monitor to show link up. Other values like `"Up"`, `"CONNECTED"`, etc. do NOT work.

---

### 0xC7 — Uptime
| Field | Value |
|-------|-------|
| Binary | 4 bytes (1 x uint32 LE) |
| Strings | 0 |
| Set Selector | 0 |

**Binary layout:**
| Offset | Type | Description |
|--------|------|-------------|
| 0-3 | uint32 LE | System uptime in seconds |

---

### 0xCB — Computer Name (FQDN)
| Field | Value |
|-------|-------|
| Binary | 0 bytes |
| Strings | 1: Fully qualified domain name |
| Set Selector | 0 |

Example string: `"xserve.local"`

---

## Reading Algorithm

To read all data from an Xserve BMC:

1. **System Info**: Read 0x01, 0x02, 0x03, 0x04, 0xCB (single set_selector = 0)
2. **Hardware**: Read 0xC0 (CPU), 0xC1 (Misc/RAM), 0xC7 (Uptime)
3. **Memory**: Loop set_selector 0..N for 0xC2, stop on error. For each success, also read 0xC9 with same set_selector.
4. **Drives**: Loop set_selector 0..N for 0xC3, stop on error. For each success, also read 0xC5 with same set_selector.
5. **Network**: Loop set_selector 0..N for 0xC4, stop on error. For each success, also read 0xC6 with same set_selector.

## Parsing Payload

```
1. Read binary_len bytes as binary data
2. Read 1 byte as encoding marker (0x00=ASCII, 0x01=UTF-8, 0x02=UTF-16)
3. For each string:
   a. Read 1 byte as str_len
   b. Read str_len bytes
   c. If byte at (current_binary_offset + str_len) == 0x00: string_data = str_len - 2 bytes from offset+2
   d. Else: string_data = str_len - 1 bytes from offset+2
   e. Advance by str_len + 1 bytes
```

Simplified (for UTF-8 with the +2 format we write):
```
1. Skip binary_len bytes (binary data)
2. Skip 1 byte (encoding = 0x01)
3. For each string:
   a. len = read 1 byte
   b. string = read (len - 2) bytes
   c. skip 2 bytes (null + pad)
```

---

## Graceful Chassis Control (Cmd 0x04 / 0x05)

This is the **only** graceful shutdown path on Xserve. Standard IPMI chassis power soft (ACPI) does NOT work because Apple firmware does not route the ACPI power button signal.

### Request: NetFn 0x36, Cmd 0x04

| Data Byte | Action |
|-----------|--------|
| 0x01 | Graceful Shutdown |
| 0x02 | Graceful Restart |

- BMC returns completion code 0x00 on success
- Sending 0x00 is accepted but is a no-op
- Sending empty data returns 0xC7 (data length invalid)

### How It Works

1. Remote management app (Server Monitor) sends `0x36 0x04 [action]` over IPMI LAN
2. BMC stores the pending request
3. BMC fires a KCS interrupt to the local OS
4. The local OS agent receives the interrupt via `IOServiceAddInterestNotification` (macOS) or equivalent KCS notification mechanism
5. The agent registers for BMC notifications by enabling Global Enable bit 0 (Receive Message Queue Interrupt) via `Set BMC Global Enables` (NetFn 0x06, Cmd 0x2E)
6. The agent executes the shutdown/restart
7. The agent acknowledges via Cmd 0x05

### Response: NetFn 0x36, Cmd 0x05

| Data Byte | Action |
|-----------|--------|
| 0x01 | Shutdown acknowledged |
| 0x02 | Restart acknowledged |

- Fire-and-forget (no response expected)
- Sent by the local OS agent after initiating the action

**Note:** On ESXi, the `hostd` process handles graceful shutdown natively via its own IPMI listener. On macOS, Apple's hwmond registered for KCS interrupts via the `AppleKCS` IOKit driver.

---

## SMTP/Email Notification (NetFn 0x30)

The BMC has built-in SMTP email notification capability, separate from the OEM system info commands.

### Commands

- **Get**: NetFn 0x30, Cmd 0xB2
- **Set**: NetFn 0x30, Cmd 0xB3

### Parameters

| Param | Name | Type |
|-------|------|------|
| 0x00 | Lock | Binary (1 byte): 0x01=acquire, 0x00=release |
| 0x01 | Sender Machine Name | String (multi-block) |
| 0x02 | Config Counts | Binary (2 bytes): byte[0]=max EmailTo slots |
| 0x03 | Email From Address | String (multi-block) |
| 0x04 | Email To Address | String (multi-block), set_selector=recipient index |
| 0x05 | Email Subject | String (multi-block) |
| 0x06 | DNS Override | Binary (1 byte) |
| 0x07 | SMTP Server Hostname | String (multi-block) |

Bitmask 0xBA classifies types: bits 1,3,4,5,7 are strings; bits 0,2,6 are binary.

### Read Format

- Binary params (0x00, 0x02, 0x06): `Data = [0x00, param, set_selector, 0x00]`
- String params (0x01, 0x03-0x05, 0x07): `Data = [0x00, param, set_selector, block_selector]`

**CRITICAL:** `block_selector` starts at `0x01`, NOT `0x00`. Using `0x00` returns completion code `0xCC`. Loop increments until response length < 17 bytes or `0xCC` is returned.

### Write Workflow

1. Acquire lock: Cmd 0xB3, Data `[0x00, 0x01]`
2. Write sender machine name (param 0x01)
3. Write email-to addresses (param 0x04, varying set_selector per recipient)
4. Configure PEF alert destinations for each recipient
5. Clear unused slots with all-zero data blocks
6. Write SMTP server (0x07), email from (0x03), subject (0x05)
7. Release lock: Cmd 0xB3, Data `[0x00, 0x00]`

Max string length: 64 bytes (4 blocks). Block_selector starts at 1.

---

## Apple OEM Boot Device Override

Standard IPMI `Set System Boot Options` (NetFn 0x00, Cmd 0x08) with Apple extensions.

### Sequence

1. Clear Boot Info Acknowledge: param 4, data `[0xFF, 0xFF]`
2. If Apple OEM boot mode: set param 7 first (see below)
3. Set Boot Flags: param 5, data `[0x80, device_byte, 0x00, 0x00, 0x00]`

### Standard Boot Devices (param 5, byte[1])

| Index | Name | Byte[1] |
|-------|------|---------|
| 0 | Normal startup (no override) | 0x00 |
| 1 | Optical drive (CD/DVD) | 0x14 |
| 2 | NetBoot/PXE | 0x04 |
| 3 | First internal drive | 0x08 |
| 7 | Diagnostic mode (NetBoot) | 0x10 |

### Apple OEM Boot Modes (set param 7 FIRST, then param 5 with `[0x80, 0x00, 0, 0, 0]`)

| Index | Name | Param 7 Data |
|-------|------|-------------|
| 4 | Skip current startup disk | [0x01, 0x04] |
| 5 | Target Disk Mode | [0x01, 0x03] |
| 6 | Reset NVRAM | [0x01, 0x02] |

### Boot Redirection Support

`Get Device ID` response is checked: if manufacturer ID == 0x3F AND product ID == 0x01, boot redirection is NOT supported (early Xserve firmware). Product ID 0x02 (Xserve 3,1) supports it.
