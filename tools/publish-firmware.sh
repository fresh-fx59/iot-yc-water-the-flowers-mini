#!/usr/bin/env bash
# publish-firmware.sh — promote a built firmware.bin to the Cloud.ru OTA endpoint.
#
# Usage:
#   tools/publish-firmware.sh <path-to-firmware.bin> <version> [notes]
#
# Computes sha256, writes manifest.json, scps both to the VPS, and atomically
# renames manifest.json on the server so the device never sees a half-written
# manifest. Keeps historical firmware-*.bin files for manual rollback by
# re-publishing an older manifest.
#
# Configure once via environment variables (or edit the defaults below):
#   OTA_SSH_TARGET   e.g.  user1@45.151.30.146     (must have passwordless sudo)
#   OTA_REMOTE_DIR   e.g.  /var/www/firmware
#
# Example:
#   FIRMWARE_VERSION must already be bumped in include/config.h BEFORE pio build.
#   pio run -e esp32-s3-devkitc-1
#   tools/publish-firmware.sh \
#       .pio/build/esp32-s3-devkitc-1/firmware.bin 1.2.0 "remote OTA"

set -euo pipefail

usage() {
    cat <<USAGE
Usage: $0 <firmware.bin> <version> [notes]
Env:
  OTA_SSH_TARGET (default: user1@45.151.30.146)
  OTA_REMOTE_DIR (default: /var/www/firmware)
USAGE
    exit 1
}

[[ $# -ge 2 ]] || usage

BIN_PATH="$1"
VERSION="$2"
NOTES="${3:-}"

OTA_SSH_TARGET="${OTA_SSH_TARGET:-user1@45.151.30.146}"
OTA_REMOTE_DIR="${OTA_REMOTE_DIR:-/var/www/firmware}"

[[ -f "$BIN_PATH" ]] || { echo "error: $BIN_PATH not found" >&2; exit 1; }
[[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "error: version must be N.N.N (got: $VERSION)" >&2
    exit 1
}

# Compute sha256 (portable: prefer sha256sum, fall back to shasum on macOS).
if command -v sha256sum >/dev/null 2>&1; then
    SHA="$(sha256sum "$BIN_PATH" | awk '{print $1}')"
else
    SHA="$(shasum -a 256 "$BIN_PATH" | awk '{print $1}')"
fi
SIZE="$(wc -c < "$BIN_PATH" | tr -d ' ')"

REMOTE_BIN_NAME="firmware-${VERSION}.bin"
REMOTE_URL="/v1/firmware/${REMOTE_BIN_NAME}"

# Build manifest locally — JSON without external deps.
TMP_MANIFEST="$(mktemp -t fw-manifest.XXXXXX.json)"
# mktemp creates 600; nginx runs as www-data and needs read access after scp
# preserves the mode. Make the manifest world-readable before upload.
chmod 644 "$TMP_MANIFEST"
trap 'rm -f "$TMP_MANIFEST"' EXIT

if [[ -n "$NOTES" ]]; then
    NOTES_ESCAPED="$(printf '%s' "$NOTES" | sed 's/\\/\\\\/g; s/"/\\"/g')"
    cat > "$TMP_MANIFEST" <<JSON
{
  "version": "$VERSION",
  "url": "$REMOTE_URL",
  "size": $SIZE,
  "sha256": "$SHA",
  "notes": "$NOTES_ESCAPED"
}
JSON
else
    cat > "$TMP_MANIFEST" <<JSON
{
  "version": "$VERSION",
  "url": "$REMOTE_URL",
  "size": $SIZE,
  "sha256": "$SHA"
}
JSON
fi

echo "===> Manifest:"
cat "$TMP_MANIFEST"
echo
echo "===> Uploading to ${OTA_SSH_TARGET}:${OTA_REMOTE_DIR}/${REMOTE_BIN_NAME} ..."

# scp both files; install manifest atomically via mv from a tmp name.
scp "$BIN_PATH"      "${OTA_SSH_TARGET}:${OTA_REMOTE_DIR}/${REMOTE_BIN_NAME}"
scp "$TMP_MANIFEST"  "${OTA_SSH_TARGET}:${OTA_REMOTE_DIR}/manifest.json.tmp"
ssh "$OTA_SSH_TARGET" "mv -f ${OTA_REMOTE_DIR}/manifest.json.tmp ${OTA_REMOTE_DIR}/manifest.json"

echo "===> Published v${VERSION} (${SIZE} bytes, sha256 ${SHA:0:12}...)."
echo "    On the device, send: /check_update"
