#!/bin/bash
# Clean, low-disk-footprint iOS userspace root builder for macOS CI / local runner.
# Creates a 32 GiB Case-sensitive Journaled HFS+ image populated with real iOS 26.6.2.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$REPO_ROOT/out/ipsw"
DMG_DIR="$REPO_ROOT/ipsw-23G90"
IMG_PATH="$OUT_DIR/ios26-userdata.hfs.img"

mkdir -p "$OUT_DIR" "$DMG_DIR"

echo "=== 1. Compile nc-static ==="
clang -O3 -I"$REPO_ROOT/common/cache" -I"$REPO_ROOT/xnu-overlay/bsd/kern" \
  "$REPO_ROOT/common/cache/b2l_cache_core.c" \
  "$REPO_ROOT/common/cache/CacheSha.c" \
  "$REPO_ROOT/tools/b1l/static-translate/nc-static.c" \
  -o "$REPO_ROOT/nc-static"
chmod +x "$REPO_ROOT/nc-static"

echo "=== 2. Create raw 32 GiB Case-sensitive Journaled HFS+ volume ==="
rm -f "$OUT_DIR/ios26-userdata.hfs"* "$IMG_PATH"*
hdiutil create -size 32g -layout NONE -fs "Case-sensitive Journaled HFS+" \
  -volname B2LIOSROOT -type UDTO -o "$OUT_DIR/ios26-userdata.hfs"
mv "$OUT_DIR/ios26-userdata.hfs.cdr" "$IMG_PATH"

attached=()
cleanup() {
  for m in "${attached[@]:-}"; do
    [[ -n "$m" ]] || continue
    hdiutil detach "$m" >/dev/null 2>&1 || hdiutil detach -force "$m" >/dev/null 2>&1 || true
  done
}
trap cleanup EXIT

attach_raw() {
  local mnt
  mnt=$(hdiutil attach -plist -nobrowse -owners on "$@" | \
    grep -A1 '<key>mount-point</key>' | grep -o '<string>.*</string>' | sed 's/<[^>]*>//g' | head -1)
  [[ -n "$mnt" ]] || { echo "attach failed: $*" >&2; exit 1; }
  echo "$mnt"
}

ROOT_MNT=$(attach_raw -readwrite -imagekey diskimage-class=CRawDiskImage "$IMG_PATH")
attached+=("$ROOT_MNT")
echo "Mounted HFS+ root at $ROOT_MNT"
mdutil -i off "$ROOT_MNT" >/dev/null 2>&1 || true

echo "=== 3. Download & copy AppOS ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" AppOS
APPOS_MNT=$(attach_raw -readonly "$DMG_DIR/AppOS.dmg")
attached+=("$APPOS_MNT")
mkdir -p "$ROOT_MNT/private/preboot/Cryptexes/App"
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" "$APPOS_MNT" "$ROOT_MNT/private/preboot/Cryptexes/App"
hdiutil detach "$APPOS_MNT" >/dev/null
attached=("${attached[@]/$APPOS_MNT}")
rm -f "$DMG_DIR/AppOS.dmg"
echo "AppOS copied and source removed."

echo "=== 4. Download, decrypt & copy SystemOS ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" SystemOS
SYSOS_MNT=$(attach_raw -readonly "$DMG_DIR/SystemOS.dmg")
attached+=("$SYSOS_MNT")
mkdir -p "$ROOT_MNT/private/preboot/Cryptexes/OS"
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" "$SYSOS_MNT" "$ROOT_MNT/private/preboot/Cryptexes/OS"
hdiutil detach "$SYSOS_MNT" >/dev/null
attached=("${attached[@]/$SYSOS_MNT}")
rm -f "$DMG_DIR/SystemOS.dmg"
echo "SystemOS copied and source removed."

echo "=== 5. Download, decrypt & copy OS ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" OS
OS_MNT=$(attach_raw -readonly "$DMG_DIR/OS.dmg")
attached+=("$OS_MNT")
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" "$OS_MNT" "$ROOT_MNT"
hdiutil detach "$OS_MNT" >/dev/null
attached=("${attached[@]/$OS_MNT}")
rm -f "$DMG_DIR/OS.dmg"
echo "OS root copied and source removed."

echo "=== 6. Prepare iOS root configuration ==="
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --dry-run
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --report "$OUT_DIR/prepare-report.json"
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --verify

echo "=== 7. Translate Mach-O arm64e binaries in-place ==="
find "$ROOT_MNT" -xdev -type f -size +8k ! -path '*/com.apple.dyld/*' ! -path "$ROOT_MNT/private/var/*" -print0 | \
  xargs -0 sudo "$REPO_ROOT/nc-static" macho-apply || true

echo "=== 8. Clean unmount and fsck_hfs ==="
sync
cleanup
attached=()

RAW_DEV=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$IMG_PATH" | awk 'NR==1{print $1}')
attached+=("$RAW_DEV")
fsck_hfs -fn "$RAW_DEV"
cleanup
attached=()

echo "=== 9. Compress and chunk root image ==="
cd "$OUT_DIR"
shasum -a 256 "ios26-userdata.hfs.img" > "ios26-userdata.hfs.img.sha256"
cat "ios26-userdata.hfs.img.sha256"

# Compress with gzip -1 and split into 1.8 GiB chunks for GitHub Actions artifact compatibility
echo "Compressing into sparse chunks..."
gzip -1 -c "ios26-userdata.hfs.img" | split -b 1800m - "ios26-userdata.hfs.img.gz.part_"
rm -f "ios26-userdata.hfs.img"
ls -lh "$OUT_DIR"
echo "Build completed successfully!"
