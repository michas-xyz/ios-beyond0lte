#!/bin/bash
# extract-firmware.sh — Extrait le DTB live et les firmwares Wi-Fi/BT Broadcom BCM4375 via ADB en TWRP
# Compatible Galaxy S10e (beyond0lte), S10 (beyond1lte), S10+ (beyond2lte)

set -euo pipefail

ADB="${ADB:-$HOME/Developer/android/platform-tools/adb}${B2L_SERIAL:+ -s $B2L_SERIAL}"
BOARD="${B2L_BOARD:-beyond0lte}"
OUTPUT_DIR="${S10_PORT:-$HOME/Developer/s10-port}/device-s10"

echo "=== Extraction des firmwares périphériques ($BOARD) ==="
echo "Destination : $OUTPUT_DIR"
echo ""

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

python3 "$REPO_DIR/scripts/extract-device-blobs.py" \
  --board "$BOARD" \
  --adb "${ADB%% *}" \
  ${B2L_SERIAL:+--serial "$B2L_SERIAL"} \
  --output-dir "$OUTPUT_DIR" \
  "$@"
