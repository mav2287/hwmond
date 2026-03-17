#!/bin/bash
# build-vib.sh - Package hwmond as an ESXi VIB
#
# Creates a VIB (VMware Installation Bundle) that installs:
#   /opt/hwmond/hwmond              - the daemon binary (native, dynamically-linked)
#   /etc/rc.local.d/hwmond-startup.sh - boot-time startup script
#   /etc/init.d/hwmond              - VIB lifecycle hooks
#
# No companion scripts are needed. The binary handles CPU monitoring
# internally via popen(vsish).
#
# Usage:
#   ./scripts/build-vib.sh <binary-path> <output-vib-path>
#
# Example:
#   ./scripts/build-vib.sh build/hwmond build/hwmond-xserve.vib
#
# Requirements: ar, tar, gzip, sha256sum/shasum, sha1sum/shasum

set -euo pipefail

# ---- Configuration ----

VIB_NAME="hwmond-xserve"
VIB_VERSION="2.0.0-1"
VIB_VENDOR="hwmond"
VIB_SUMMARY="Xserve front panel CPU LED daemon for ESXi"
VIB_DESCRIPTION="Drives the CPU activity LED bar graph on Apple Xserve front panels. Native dynamically-linked binary with built-in CPU monitoring via vsish."
VIB_DATE=$(date -u '+%Y-%m-%dT%H:%M:%S')

# ---- Arguments ----

BINARY="${1:?Usage: $0 <binary-path> <output-vib-path>}"
VIB_OUTPUT="${2:?Usage: $0 <binary-path> <output-vib-path>}"

if [ ! -f "${BINARY}" ]; then
    echo "Error: binary not found: ${BINARY}" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

# ---- Platform-specific tool detection ----

# SHA-256 sum
if command -v sha256sum >/dev/null 2>&1; then
    SHA256="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA256="shasum -a 256"
else
    echo "Error: sha256sum or shasum not found" >&2
    exit 1
fi

# SHA-1 sum
if command -v sha1sum >/dev/null 2>&1; then
    SHA1="sha1sum"
elif command -v shasum >/dev/null 2>&1; then
    SHA1="shasum -a 1"
else
    echo "Error: sha1sum or shasum not found" >&2
    exit 1
fi

# stat file size (macOS vs Linux)
file_size() {
    if stat -f%z "$1" 2>/dev/null; then
        return
    fi
    stat -c%s "$1" 2>/dev/null
}

# ---- Working directory ----

TMPDIR=$(mktemp -d)
trap 'rm -rf "${TMPDIR}"' EXIT

echo "==> Building VIB: ${VIB_NAME} v${VIB_VERSION}"

# ---- Create payload directory tree ----

PAYLOAD_DIR="${TMPDIR}/payload"
mkdir -p "${PAYLOAD_DIR}/opt/hwmond"
mkdir -p "${PAYLOAD_DIR}/etc/rc.local.d"
mkdir -p "${PAYLOAD_DIR}/etc/init.d"

# Copy binary
cp "${BINARY}" "${PAYLOAD_DIR}/opt/hwmond/hwmond"
chmod 755 "${PAYLOAD_DIR}/opt/hwmond/hwmond"

# Copy startup script (boot-time entry point)
cp "${PROJECT_DIR}/scripts/hwmond-startup.sh" \
   "${PAYLOAD_DIR}/etc/rc.local.d/hwmond-startup.sh"
chmod 755 "${PAYLOAD_DIR}/etc/rc.local.d/hwmond-startup.sh"

# Copy init script (VIB lifecycle hooks)
cp "${PROJECT_DIR}/scripts/hwmond-init.sh" \
   "${PAYLOAD_DIR}/etc/init.d/hwmond"
chmod 755 "${PAYLOAD_DIR}/etc/init.d/hwmond"

echo "    Payload contents:"
find "${PAYLOAD_DIR}" -type f | sed "s|${PAYLOAD_DIR}/|      /|"

# ---- Create payload tarball in VMware "visor" format ----
#
# ESXi uses a modified tar format with "visor" magic at offset 257
# instead of "ustar", plus specific field formatting (7-char null-terminated
# octal, uid/gid=311, uname="root", gname="root").
# We build the ENTIRE tar in Python for byte-exact control.

PAYLOAD_FILE="${TMPDIR}/payload1"

python3 - "${PAYLOAD_DIR}" "${PAYLOAD_FILE}" << 'PYEOF'
import os, sys, struct, gzip, time

def tar_checksum(header):
    chk = 0
    for i in range(512):
        if 148 <= i < 156:
            chk += 0x20
        else:
            chk += header[i]
    return chk

def make_header(name, size, mode, is_dir, mtime):
    """Create a 512-byte visor tar header."""
    h = bytearray(512)

    # Name (100 bytes)
    name_bytes = name.encode('ascii')[:100]
    h[0:len(name_bytes)] = name_bytes

    # Mode (8 bytes, 7-char octal null-terminated)
    h[100:108] = ('%07o\x00' % mode).encode('ascii')

    # UID (8 bytes) — ESXi uses 0
    h[108:116] = b'0000000\x00'

    # GID (8 bytes) — ESXi uses 0
    h[116:124] = b'0000000\x00'

    # Size (12 bytes, 11-char octal null-terminated)
    h[124:136] = ('%011o\x00' % size).encode('ascii')

    # Mtime (12 bytes, 11-char octal null-terminated)
    h[136:148] = ('%011o\x00' % mtime).encode('ascii')

    # Checksum placeholder (filled below)
    h[148:156] = b'        '

    # Type flag
    h[156] = ord('5') if is_dir else ord('0')

    # Magic: "visor  \0" at offset 257
    h[257:265] = b'visor  \x00'

    # Uname (32 bytes)
    h[265:269] = b'root'

    # Gname (32 bytes)
    h[297:301] = b'root'

    # Compute and set checksum
    chk = tar_checksum(h)
    h[148:156] = ('%06o\x00 ' % chk).encode('ascii')

    return bytes(h)

def visor_tar_gz(payload_dir, output_path):
    entries = []

    # Collect all files and directories
    for root, dirs, files in os.walk(payload_dir):
        rel = os.path.relpath(root, payload_dir)
        if rel == '.':
            rel = ''

        # Add directory entry
        if rel:
            dir_path = rel + '/'
            st = os.stat(root)
            entries.append((dir_path, None, 0o755, True, int(st.st_mtime)))

        # Add file entries
        for fname in sorted(files):
            full = os.path.join(root, fname)
            rel_path = os.path.join(rel, fname) if rel else fname
            st = os.stat(full)
            entries.append((rel_path, full, st.st_mode & 0o7777, False, int(st.st_mtime)))

    # Sort entries for consistent ordering
    entries.sort(key=lambda e: e[0])

    # Build tar and gzip
    with gzip.open(output_path, 'wb') as gz:
        for name, filepath, mode, is_dir, mtime in entries:
            if is_dir:
                hdr = make_header(name, 0, mode, True, mtime)
                gz.write(hdr)
            else:
                data = open(filepath, 'rb').read()
                hdr = make_header(name, len(data), mode, False, mtime)
                gz.write(hdr)
                gz.write(data)
                # Pad to 512-byte boundary
                remainder = len(data) % 512
                if remainder:
                    gz.write(b'\x00' * (512 - remainder))

        # End-of-archive: two 512-byte zero blocks
        gz.write(b'\x00' * 1024)

    print(f"    Created visor tar with {len(entries)} entries")

visor_tar_gz(sys.argv[1], sys.argv[2])
PYEOF

# ---- Compute checksums ----

PAYLOAD_SIZE=$(file_size "${PAYLOAD_FILE}")
PAYLOAD_SHA256=$(${SHA256} "${PAYLOAD_FILE}" | awk '{print $1}')
PAYLOAD_SHA256_ZCAT=$(gzip -dc "${PAYLOAD_FILE}" | ${SHA256} | awk '{print $1}')
PAYLOAD_SHA1_ZCAT=$(gzip -dc "${PAYLOAD_FILE}" | ${SHA1} | awk '{print $1}')

echo "    Payload size: ${PAYLOAD_SIZE} bytes"
echo "    SHA-256: ${PAYLOAD_SHA256}"

# ---- Generate file list ----

FILE_LIST=$(tar tf "${PAYLOAD_FILE}" | grep -v '/$' | while read -r f; do
    echo "    <file>${f}</file>"
done)

# ---- Create descriptor.xml ----

cat > "${TMPDIR}/descriptor.xml" << XMLEOF
<vib version="5.0">
  <type>bootbank</type>
  <name>${VIB_NAME}</name>
  <version>${VIB_VERSION}</version>
  <vendor>${VIB_VENDOR}</vendor>
  <summary>${VIB_SUMMARY}</summary>
  <description>${VIB_DESCRIPTION}</description>
  <release-date>${VIB_DATE}</release-date>
  <urls>
    <url key="website">https://github.com/castvoid/xserve-frontpanel</url>
  </urls>
  <relationships>
    <depends></depends>
    <conflicts/>
    <replaces/>
    <provides/>
    <compatibleWith/>
  </relationships>
  <software-tags></software-tags>
  <system-requires>
    <maintenance-mode>false</maintenance-mode>
  </system-requires>
  <file-list>
${FILE_LIST}
  </file-list>
  <acceptance-level>community</acceptance-level>
  <live-install-allowed>true</live-install-allowed>
  <live-remove-allowed>true</live-remove-allowed>
  <cimom-restart>false</cimom-restart>
  <stateless-ready>true</stateless-ready>
  <overlay>false</overlay>
  <payloads>
    <payload name="payload1" type="tgz" size="${PAYLOAD_SIZE}">
        <checksum checksum-type="sha-256">${PAYLOAD_SHA256}</checksum>
    </payload>
  </payloads>
</vib>
XMLEOF

# ---- Create empty signature (CommunitySupported = unsigned) ----

touch "${TMPDIR}/sig.pkcs7"

# ---- Assemble VIB (ar archive) ----
#
# ESXi requires a specific ar archive format. Both macOS ar (BSD format)
# and GNU ar (trailing / on names) have subtle incompatibilities.
# Use a Python script to produce the exact byte-level format ESXi expects.

mkdir -p "$(dirname "${VIB_OUTPUT}")"

echo "    Assembling VIB ar archive..."
python3 - "${TMPDIR}" << 'PYEOF'
import os, sys

def ar_create(outpath, members):
    with open(outpath, 'wb') as f:
        f.write(b'!<arch>\n')
        for name, filepath in members:
            data = open(filepath, 'rb').read()
            sz = len(data)
            hdr = '%-16s%-12d%-6d%-6d%-8s%-10d\x60\n' % (
                name, 0, 0, 0, '100644', sz)
            f.write(hdr.encode('ascii'))
            f.write(data)
            if sz % 2 == 1:
                f.write(b'\n')

tmpdir = sys.argv[1]
ar_create(os.path.join(tmpdir, 'vib.ar'), [
    ('descriptor.xml', os.path.join(tmpdir, 'descriptor.xml')),
    ('sig.pkcs7',      os.path.join(tmpdir, 'sig.pkcs7')),
    ('payload1',       os.path.join(tmpdir, 'payload1')),
])
PYEOF

cp "${TMPDIR}/vib.ar" "${VIB_OUTPUT}"

echo "==> VIB created: ${VIB_OUTPUT} ($(file_size "${VIB_OUTPUT}") bytes)"
echo ""
echo "Installation on ESXi:"
echo "  1. Copy to ESXi:  scp ${VIB_OUTPUT} root@<host>:/tmp/"
echo "  2. Set acceptance: esxcli software acceptance set --level CommunitySupported"
echo "  3. Install VIB:    esxcli software vib install -v /tmp/$(basename "${VIB_OUTPUT}") --force --no-sig-check"
echo "  4. Reboot or run:  /etc/rc.local.d/hwmond-startup.sh"
echo ""
echo "Removal:"
echo "  esxcli software vib remove -n ${VIB_NAME}"
