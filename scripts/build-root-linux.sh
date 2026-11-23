#!/bin/bash
# Build 32 GiB Case-sensitive Journaled HFS+ iOS 26.6.2 root on Ubuntu Linux.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="$REPO_ROOT/out/ipsw"
DMG_DIR="$REPO_ROOT/ipsw-23G90"
IMG_PATH="$OUT_DIR/ios26-userdata.hfs.img"
ROOT_MNT="/mnt/iosroot"

mkdir -p "$OUT_DIR" "$DMG_DIR"

echo "=== 1. Install Linux packages ==="
sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends \
  hfsprogs libfuse3-dev libbz2-dev cmake clang make git zstd python3-cryptography libattr1-dev

echo "=== 2. Build apfs-fuse with LZFSE submodule ==="
rm -rf /tmp/apfs-fuse
git clone --recursive https://github.com/sgan81/apfs-fuse.git /tmp/apfs-fuse
mkdir -p /tmp/apfs-fuse/build
cd /tmp/apfs-fuse/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
sudo cp apfs-fuse /usr/local/bin/
which apfs-fuse

echo "=== 3. Build lzfse CLI ==="
rm -rf /tmp/lzfse
git clone --depth 1 https://github.com/lzfse/lzfse.git /tmp/lzfse
cd /tmp/lzfse
make -j"$(nproc)"
sudo cp build/bin/lzfse /usr/local/bin/
which lzfse

echo "=== 4. Compile nc-static ==="
cd "$REPO_ROOT"
clang -O3 -I"$REPO_ROOT/common/cache" -I"$REPO_ROOT/xnu-overlay/bsd/kern" \
  "$REPO_ROOT/common/cache/b2l_cache_core.c" \
  "$REPO_ROOT/common/cache/CacheSha.c" \
  "$REPO_ROOT/tools/b1l/static-translate/nc-static.c" \
  -o "$REPO_ROOT/nc-static"
chmod +x "$REPO_ROOT/nc-static"

echo "=== 5. Create 32 GiB Case-sensitive Journaled HFS+ image ==="
rm -f "$IMG_PATH"*
truncate -s 32G "$IMG_PATH"
mkfs.hfsplus -s -J -v B2LIOSROOT "$IMG_PATH"

sudo mkdir -p "$ROOT_MNT"
sudo mount -t hfsplus -o loop,force,rw "$IMG_PATH" "$ROOT_MNT"
echo "Mounted HFS+ image at $ROOT_MNT"

cleanup() {
  set +e
  sudo umount /tmp/mnt_appos 2>/dev/null || true
  sudo umount /tmp/mnt_sysos 2>/dev/null || true
  sudo umount /tmp/mnt_os 2>/dev/null || true
  sudo umount "$ROOT_MNT" 2>/dev/null || true
}
trap cleanup EXIT

echo "=== 6. Stream, mount & copy AppOS cryptex ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" AppOS
sudo mkdir -p /tmp/mnt_appos "$ROOT_MNT/private/preboot/Cryptexes/App"
sudo apfs-fuse -o allow_other "$DMG_DIR/AppOS.dmg" /tmp/mnt_appos
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" /tmp/mnt_appos "$ROOT_MNT/private/preboot/Cryptexes/App"
sudo umount /tmp/mnt_appos
rm -f "$DMG_DIR/AppOS.dmg"
echo "AppOS copied successfully."

echo "=== 7. Stream, decrypt, mount & copy SystemOS cryptex ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" SystemOS
sudo mkdir -p /tmp/mnt_sysos "$ROOT_MNT/private/preboot/Cryptexes/OS"
sudo apfs-fuse -o allow_other "$DMG_DIR/SystemOS.dmg" /tmp/mnt_sysos
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" /tmp/mnt_sysos "$ROOT_MNT/private/preboot/Cryptexes/OS"
sudo umount /tmp/mnt_sysos
rm -f "$DMG_DIR/SystemOS.dmg"
echo "SystemOS copied successfully."

echo "=== 8. Stream, decrypt, mount & copy OS root ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" OS
sudo mkdir -p /tmp/mnt_os
sudo apfs-fuse -o allow_other "$DMG_DIR/OS.dmg" /tmp/mnt_os
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" /tmp/mnt_os "$ROOT_MNT"
sudo umount /tmp/mnt_os
rm -f "$DMG_DIR/OS.dmg"
echo "OS root copied successfully."

echo "=== 9. Prepare iOS root configuration ==="
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --dry-run
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --report "$OUT_DIR/prepare-report.json"
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --verify

echo "=== 10. Translate standalone Mach-O binaries ==="
find "$ROOT_MNT" -xdev -type f -size +8k ! -path '*/com.apple.dyld/*' ! -path "$ROOT_MNT/private/var/*" -print0 | \
  xargs -0 sudo "$REPO_ROOT/nc-static" macho-apply || true

echo "=== 11. Clean unmount and fsck ==="
sync
sudo umount "$ROOT_MNT"
trap - EXIT

sudo fsck.hfsplus -f "$IMG_PATH" || true

echo "=== 12. Hash and compress into chunks ==="
cd "$OUT_DIR"
sha256sum "ios26-userdata.hfs.img" > "ios26-userdata.hfs.img.sha256"
cat "ios26-userdata.hfs.img.sha256"

echo "Compressing with gzip -1 and splitting into 1.8 GiB chunks..."
gzip -1 -c "ios26-userdata.hfs.img" | split -b 1800m - "ios26-userdata.hfs.img.gz.part_"
rm -f "ios26-userdata.hfs.img"
ls -lh "$OUT_DIR"
echo "=== Build finished successfully! ==="
