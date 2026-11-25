#!/bin/bash
# B1L-0051 (Metal M3): Scion-Project/hackios-xnu-kernel's Metal driver for the Mali (MIT, userspace/metal/scion, commit in
# SCION-COMMIT): libhakiosair (Mesa: the AIR front end into NIR, panfrost's Bifrost compiler, Gallium on HakiosMali; built by
# scripts/build-mesa.sh), libhakiosmetal (the device object, hakiosmetal_air.m + hakiosdrawable.m) and their test program
# metal_drv_test as a library (main renamed), run by our test in tzinit (libb2l-metal-test, userspace/metal/b2l_metal_test.m).
# /redwood/lib/ becomes /usr/local/lib/$V/. Output: ~/Developer/s10-port/build-metal/$V.
set -euo pipefail
V=${V:-b2l-metal-v7}
R=$(cd "$(dirname "$0")/.." && pwd)
O=${METAL_OUT:-$HOME/Developer/s10-port/build-metal/$V}
T=$O.src
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC="xcrun --sdk iphoneos clang -target arm64e-apple-ios${B2L_IOS_TARGET:-$(xcrun --sdk iphoneos --show-sdk-version)} -isysroot $SDK -O2 -fno-ptrauth-returns -fno-ptrauth-calls -Wno-deprecated-declarations -Wno-unused"   # B1L-0075/0076: no signed returns or calls (no PAC on the Exynos; the kernel accepts unsigned pointers)
M=${MESA_LIB:-${MESA_BUILD:-$HOME/Developer/s10-port/build-mesa}/build-softpipe-panfrost/src/gallium/targets/hakiosair/libhakiosair.dylib}
rm -rf "$O" "$T"; mkdir -p "$O" "$T"
for f in "$R"/userspace/metal/scion/*.m "$R"/userspace/metal/scion/*.h; do
	sed "s#/redwood/lib/#/usr/local/lib/$V/#g" "$f" > "$T/$(basename "$f")"
done
cp "$M" "$O/libhakiosair.dylib"
"$(xcrun -f strip)" -x "$O/libhakiosair.dylib"
/usr/bin/install_name_tool -id /usr/local/lib/$V/libhakiosair.dylib "$O/libhakiosair.dylib" 2>/dev/null || true
$CC -dynamiclib -install_name /usr/local/lib/$V/libhakiosmetal.dylib -I"$T" \
	-framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
	-o "$O/libhakiosmetal.dylib" "$T/hakiosmetal_air.m" "$T/hakiosdrawable.m"
$CC -dynamiclib -Dmain=b2l_drv_main -install_name /usr/local/lib/$V/libb2l-metal-drvtest.dylib -I"$T" \
	-framework Foundation -framework IOKit -framework Metal -framework IOSurface -lobjc \
	-o "$O/libb2l-metal-drvtest.dylib" "$T/metal_drv_test.m"
$CC -dynamiclib -install_name /usr/local/lib/$V/libb2l-metal-test.dylib -DB2L_METAL_DRVTEST="\"/usr/local/lib/$V/libb2l-metal-drvtest.dylib\"" \
	-framework Foundation -framework Metal -framework IOSurface -framework QuartzCore -lobjc -o "$O/libb2l-metal-test.dylib" "$R/userspace/metal/b2l_metal_test.m"
# B1L-0079 (M6a): the library the kernel inserts into the Clock app (userspace/metal/b2l_metal_app.m)
$CC -dynamiclib -install_name /usr/local/lib/$V/libb2l-metal-app.dylib -DB2L_METAL_SET="\"/usr/local/lib/$V\"" \
	-framework Foundation -framework UIKit -framework Metal -framework QuartzCore -framework IOSurface -framework IOKit -lobjc \
	-o "$O/libb2l-metal-app.dylib" "$R/userspace/metal/b2l_metal_app.m"
# B1L-0080 (M6b): the plug-in bundle's executable and Info.plist, published by the kernel as
# /System/Library/Extensions/B2LMetal<set>.bundle (B2LMetalAccel.cpp chooses it when b2l_metal_ui is on)
P=B2LMetal${V#b2l-metal-}   # B1L-0081: one plug-in bundle per set (B2LMetalv37...), linked to that set's driver
$CC -bundle -o "$O/$P" "$R/userspace/metal/b2l_metal_plugin.m" \
	-framework Foundation -framework IOKit -lobjc "$O/libhakiosmetal.dylib"
cat > "$O/$P-Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key><string>dev.b2l.metal.$P</string>
	<key>CFBundleName</key><string>$P</string>
	<key>CFBundleExecutable</key><string>$P</string>
	<key>CFBundlePackageType</key><string>BNDL</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
	<key>NSPrincipalClass</key><string>B2LMetalUIDevice</string>
</dict>
</plist>
PLIST
# B1L-0078: the linker's stubs (__auth_stubs, __objc_stubs) branch with BRAA, a PAC trap each on the Exynos
python3 "$R/scripts/unsign-branches.py" "$O"/*.dylib "$O/$P"
for f in "$O"/*.dylib "$O/$P"; do /usr/bin/codesign --force --sign - "$f"; done
rm -rf "$T"
shasum -a 256 "$O"/*
