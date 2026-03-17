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

# ---- Create payload tarball ----

PAYLOAD_FILE="${TMPDIR}/payload1"
# Use POSIX format, strip macOS metadata, set uid/gid to 0
# COPYFILE_DISABLE prevents macOS from adding ._ resource fork files
COPYFILE_DISABLE=1 tar czf "${PAYLOAD_FILE}" -C "${PAYLOAD_DIR}" \
    --format=ustar \
    --uid=0 --gid=0 \
    --no-mac-metadata \
    opt etc 2>/dev/null \
    || COPYFILE_DISABLE=1 tar czf "${PAYLOAD_FILE}" -C "${PAYLOAD_DIR}" \
    --format=ustar \
    --uid=0 --gid=0 \
    opt etc

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

# ---- Assemble VIB (ar archive, order matters!) ----

# Ensure output directory exists
mkdir -p "$(dirname "${VIB_OUTPUT}")"

# ar requires files in current directory on some platforms
(
    cd "${TMPDIR}"
    ar r vib.ar descriptor.xml sig.pkcs7 payload1
)

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
