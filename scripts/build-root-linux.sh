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
sed -i '1i #include <cstdint>' /tmp/apfs-fuse/ApfsLib/PList.h
sed -i '1i #include <cstdint>' /tmp/apfs-fuse/ApfsLib/Global.h
mkdir -p /tmp/apfs-fuse/build
cd /tmp/apfs-fuse/build
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release ..
make -j"$(nproc)"
sudo cp apfs-fuse /usr/local/bin/
which apfs-fuse

echo "=== 3. Build lzfse CLI and library ==="
rm -rf /tmp/lzfse
git clone --depth 1 https://github.com/lzfse/lzfse.git /tmp/lzfse
cd /tmp/lzfse
make -j"$(nproc)"
sudo cp build/bin/lzfse /usr/local/bin/
sudo gcc -shared -fPIC -O3 src/lzfse_decode.c src/lzfse_decode_base.c src/lzfse_encode.c src/lzfse_encode_base.c src/lzfse_fse.c src/lzvn_decode_base.c src/lzvn_encode_base.c -Isrc -o /usr/local/lib/liblzfse.so
sudo ldconfig || true
which lzfse
ls -l /usr/local/lib/liblzfse.so

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

find_volume_root() {
  local mnt="$1"
  # 1. If sbin, System, or Applications exists directly under mnt:
  if [[ -d "$mnt/sbin" || -d "$mnt/System" || -d "$mnt/Applications" ]]; then
    echo "$mnt"
    return
  fi
  # 2. Look for child directory containing sbin, System, or Applications:
  for sub in "$mnt"/*; do
    if [[ -d "$sub" ]]; then
      if [[ -d "$sub/sbin" || -d "$sub/System" || -d "$sub/Applications" ]]; then
        echo "$sub"
        return
      fi
    fi
  done
  # 3. Fallback: first directory
  for sub in "$mnt"/*; do
    if [[ -d "$sub" ]]; then
      echo "$sub"
      return
    fi
  done
  echo "$mnt"
}

echo "=== 6. Stream, mount & copy AppOS cryptex ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" AppOS
sudo mkdir -p /tmp/mnt_appos "$ROOT_MNT/private/preboot/Cryptexes/App"
sudo apfs-fuse -o allow_other "$DMG_DIR/AppOS.dmg" /tmp/mnt_appos
src_appos=$(find_volume_root /tmp/mnt_appos)
echo "AppOS source volume root: $src_appos"
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" "$src_appos" "$ROOT_MNT/private/preboot/Cryptexes/App"
sudo umount /tmp/mnt_appos
rm -f "$DMG_DIR/AppOS.dmg"
echo "AppOS copied successfully."

echo "=== 7. Stream, decrypt, mount & copy SystemOS cryptex ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" SystemOS
sudo mkdir -p /tmp/mnt_sysos "$ROOT_MNT/private/preboot/Cryptexes/OS"
sudo apfs-fuse -o allow_other "$DMG_DIR/SystemOS.dmg" /tmp/mnt_sysos
src_sysos=$(find_volume_root /tmp/mnt_sysos)
echo "SystemOS source volume root: $src_sysos"
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" "$src_sysos" "$ROOT_MNT/private/preboot/Cryptexes/OS"
sudo umount /tmp/mnt_sysos
rm -f "$DMG_DIR/SystemOS.dmg"
echo "SystemOS copied successfully."

echo "=== 8. Stream, decrypt, mount & copy OS root ==="
python3 -u "$REPO_ROOT/tools/b1l/stream_dmgs.py" "$DMG_DIR" OS
sudo mkdir -p /tmp/mnt_os
sudo apfs-fuse -o allow_other "$DMG_DIR/OS.dmg" /tmp/mnt_os
src_os=$(find_volume_root /tmp/mnt_os)
echo "OS source volume root: $src_os"
sudo python3 -I "$REPO_ROOT/tools/b1l/copy-tree.py" "$src_os" "$ROOT_MNT"
sudo umount /tmp/mnt_os
rm -f "$DMG_DIR/OS.dmg"
echo "OS root copied successfully."

echo "=== Verifying filesystem layout ==="
if [[ ! -x "$ROOT_MNT/sbin/launchd" ]]; then
  echo "FATAL ERROR: $ROOT_MNT/sbin/launchd does not exist or is not executable!" >&2
  ls -la "$ROOT_MNT"
  exit 1
fi
if [[ ! -d "$ROOT_MNT/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld" ]]; then
  echo "FATAL ERROR: Cryptexes/OS dyld cache directory missing at $ROOT_MNT/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld!" >&2
  ls -la "$ROOT_MNT/private/preboot/Cryptexes/OS"
  exit 1
fi
echo "Verified: /sbin/launchd and Cryptexes layout are in place."

echo "=== 9. Prepare iOS root configuration ==="
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --dry-run
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --report "$OUT_DIR/prepare-report.json"
sudo python3 -I "$REPO_ROOT/scripts/prepare-ios-root.py" "$ROOT_MNT" --verify

if [[ ! -f "$ROOT_MNT/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64e" ]]; then
  echo "FATAL ERROR: Cryptex fallback link for dyld_shared_cache_arm64e missing at $ROOT_MNT/System/Library/Caches/com.apple.dyld/!" >&2
  exit 1
fi
echo "Verified: Cryptex links created successfully in /System/Library/Caches/com.apple.dyld/."

echo "=== 10. Translate dyld shared cache (nc-static apply) ==="
CACHE_DIR="$ROOT_MNT/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
MAIN_CACHE="$CACHE_DIR/dyld_shared_cache_arm64e"
CACHE_OUT="/tmp/nc-static-out"
sudo rm -rf "$CACHE_OUT"
mkdir -p "$CACHE_OUT"

SUB_CACHES=()
for f in "$CACHE_DIR"/dyld_shared_cache_arm64e.*; do
  sudo test -f "$f" || continue
  if sudo head -c 7 "$f" | grep -q '^dyld_v1'; then
    SUB_CACHES+=("$f")
  fi
done
echo "Translating main cache + ${#SUB_CACHES[@]} subcaches with nc-static apply..."
sudo "$REPO_ROOT/nc-static" apply "$CACHE_OUT" "$MAIN_CACHE" "${SUB_CACHES[@]}"

echo "Applying MediaServices nop-call fixes..."
OUT_SUBS=()
for f in "$CACHE_OUT"/dyld_shared_cache_arm64e.*; do
  sudo test -f "$f" && sudo head -c 7 "$f" | grep -q '^dyld_v1' && OUT_SUBS+=("$f")
done
sudo "$REPO_ROOT/nc-static" nop-call "$CACHE_OUT/dyld_shared_cache_arm64e" "${OUT_SUBS[@]}" -- 0x1ae8593f8 0x1b2571fe0 || echo "WARN: MediaServices NOP 1 skipped"
sudo "$REPO_ROOT/nc-static" nop-call "$CACHE_OUT/dyld_shared_cache_arm64e" "${OUT_SUBS[@]}" -- 0x1ae85d7f0 0x1b2571ff0 || echo "WARN: MediaServices NOP 2 skipped"

echo "Writing translated dyld shared cache files back in place (preserving hardlinks)..."
n=0
for f in "$CACHE_OUT"/dyld_shared_cache_arm64e*; do
  name=$(basename "$f")
  dst="$CACHE_DIR/$name"
  sudo test -f "$dst" || { echo "FATAL: missing cache target $dst" >&2; exit 1; }
  meta=$(sudo stat -c '%u:%g %a %i' "$dst")
  sudo dd if="$f" of="$dst" bs=4M conv=notrunc status=none
  sudo cmp -s "$f" "$dst" || { echo "FATAL: verify failed for $name" >&2; exit 1; }
  [[ "$(sudo stat -c '%u:%g %a %i' "$dst")" == "$meta" ]] || { echo "FATAL: metadata changed for $name" >&2; exit 1; }
  n=$((n + 1))
done
echo "Translated cache files written in place: $n"
sudo rm -rf "$CACHE_OUT"

echo "=== 10b. Verify dyld shared cache has 0 convertible words left ==="
sudo "$REPO_ROOT/nc-static" scan "$MAIN_CACHE" "${SUB_CACHES[@]}" | tail -12

echo "=== 10c. Translate standalone Mach-O binaries & staged_system_apps ==="
(
  sudo find "$ROOT_MNT" -xdev -type f -size +8k ! -path '*/com.apple.dyld/*' ! -path "$ROOT_MNT/private/var/*" -print0
  if sudo test -d "$ROOT_MNT/private/var/staged_system_apps"; then
    sudo find "$ROOT_MNT/private/var/staged_system_apps" -xdev -type f -size +8k -print0
  fi
) | xargs -0 -r -n 128 sudo "$REPO_ROOT/nc-static" macho-apply || true

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
