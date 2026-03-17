# Apple Xserve OEM IPMI Parameter Specification

Reverse-engineered from Apple's PlatformHardwareManagement framework and Server Monitor application.

## IPMI Transport

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
