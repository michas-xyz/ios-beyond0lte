#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause
# GPU step G5 (docs/gpu/plan.md): Mesa 25.0.7's panfrost (Bifrost compiler) as libOSMesa for iOS 26 arm64e,
# talking to the kernel's HakiosMali service through HakiOS's libdrm stand-in. Adapted from
# Scion-Project/hackios-xnu-kernel tools/mk-mesa.sh (its tools/mesa/*.patch and hakios-drm are used unchanged).
# Output: $B/out/libOSMesa.8.dylib (install name /usr/local/lib/b2l-gl/libOSMesa.8.dylib), exported-gl.txt, gl.xml.
#   bash scripts/build-mesa.sh            (DRIVERS=softpipe,panfrost by default)
set -euo pipefail
RROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
REF=${HAKIOS:-$HOME/Developer/s10-port/ref-scion/hackios-xnu-kernel}
B=${MESA_BUILD:-$HOME/Developer/s10-port/build-mesa}
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
CC=$(xcrun --sdk iphoneos -f clang)
CXX=$(xcrun --sdk iphoneos -f clang++)
TARGET=arm64e-apple-ios${B2L_IOS_TARGET:-$(xcrun --sdk iphoneos --show-sdk-version)}
mkdir -p "$B/out"; cd "$B"
PATCHES=$(cat "$REF"/tools/mesa/*.patch | shasum -a 256 | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.b2l-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	for p in "$REF"/tools/mesa/*.patch; do echo "patch ${p##*/}"; patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p"; done
	echo "$PATCHES" > mesa-$VERSION/.b2l-patches
fi
rm -rf mesa-$VERSION/src/hakios-drm mesa-$VERSION/src/compiler/air
cp -r "$RROOT/userspace/mesa/hakios-drm" mesa-$VERSION/src/hakios-drm   # B1L-0034: our traced copy
cp -r "$REF/tools/mesa/air" mesa-$VERSION/src/compiler/air
python3 "$RROOT/userspace/mesa/air-b2l/apply.py" mesa-$VERSION/src/compiler/air   # B1L-0058
# B1L-0051 (Metal M3): libhakiosair (Scion's air_metal.c, the Metal driver's compiler library, panfrost-capable) needs
# driconf, which its meson file leaves out (idep_xmlconfig)
grep -q idep_xmlconfig mesa-$VERSION/src/gallium/targets/hakiosair/meson.build || \
	sed -i '' 's/dependencies : \[dep_m, idep_air, idep_mesautil,/dependencies : [dep_m, idep_air, idep_mesautil, idep_xmlconfig,/' \
	mesa-$VERSION/src/gallium/targets/hakiosair/meson.build
# B1L-0048 (Metal M2): the Metal core over Gallium panfrost, in libOSMesa beside the screen it uses
cp "$RROOT/userspace/metal/b2l_metal_core.c" "$RROOT/userspace/metal/hakios_metal.h" mesa-$VERSION/src/gallium/targets/osmesa/
grep -q b2l_metal_core.c mesa-$VERSION/src/gallium/targets/osmesa/meson.build || \
	sed -i '' "s/  'osmesa_target.c',/  'osmesa_target.c', 'b2l_metal_core.c',/" mesa-$VERSION/src/gallium/targets/osmesa/meson.build
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install mako pyyaml packaging meson ninja
fi
export PATH=$B/venv/bin:$HOME/Developer/tools/mamba/envs/s10/bin:$PATH
# B1L-0075: -fno-ptrauth-returns. The Exynos 9820 has no PAC, so every signed return (retab) of an arm64e function
# is a trap the kernel emulates (~11 us); a draw through Mesa returned from hundreds of functions (1.2-3.4 ms a draw,
# B1L-0074). Signing a return address is private to the function, so leaving it out changes no ABI; signed calls
# through function pointers (blraaz), which callbacks handed to the system rely on, are kept.
# B1L-0076: -fno-ptrauth-calls as well. This kernel's arm64e emulation is the stripped-pointer ABI (b2l_arm64e_compat.c):
# pac* leaves a pointer as it is and aut*/blraa* accept an unsigned one, so unsigned function pointers crossing into
# the system (callbacks) and signed ones coming out of it (dlsym) are the same thing here.
cat > cross.ini.new <<INI
[binaries]
c = ['$CC', '-target', '$TARGET', '-isysroot', '$SDK', '-fno-ptrauth-returns', '-fno-ptrauth-calls']
cpp = ['$CXX', '-target', '$TARGET', '-isysroot', '$SDK', '-fuse-cxa-atexit', '-fno-ptrauth-returns', '-fno-ptrauth-calls']
objc = ['$CC', '-target', '$TARGET', '-isysroot', '$SDK', '-fno-ptrauth-returns', '-fno-ptrauth-calls']
ar = '$(xcrun -f ar)'
ranlib = '$(xcrun -f ranlib)'
strip = '$(xcrun -f strip)'
pkg-config = 'false'
[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
[properties]
needs_exe_wrapper = true
INI
DRIVERS_=${DRIVERS:-softpipe,panfrost}
if ! cmp -s cross.ini.new cross.ini; then rm -rf build-${DRIVERS_//,/-}; mv cross.ini.new cross.ini; else rm -f cross.ini.new; fi
DRIVERS=${DRIVERS:-softpipe,panfrost}
BUILD=build-${DRIVERS//,/-}
# B1L-0089: B2L_MESA_ASSERTS=1 builds Mesa with its assertions on (-Db_ndebug=false); libhakiosair's own __assert_rtn
# (apply.py) writes a failed one to the kernel's log and carries on, so the driver's real compile path says which
# shader breaks a rule instead of producing code the Mali stops on. Changing it reconfigures the build directory.
NDEBUG=true; [ "${B2L_MESA_ASSERTS:-0}" = 1 ] && NDEBUG=false
if [ -f $BUILD/build.ninja ] && ! grep -q "b_ndebug=$NDEBUG" $BUILD/.b2l-ndebug 2>/dev/null; then
	meson configure $BUILD -Db_ndebug=$NDEBUG > /dev/null
fi
# B1L-0095: Mesa's shader cache on (the Mali's binaries kept in /b2l-dump/mesa-cache, apply.py)
if [ -f $BUILD/build.ninja ] && ! grep -q "shader-cache=enabled" $BUILD/.b2l-shader-cache 2>/dev/null; then
	meson configure $BUILD -Dshader-cache=enabled > /dev/null && echo "shader-cache=enabled" > $BUILD/.b2l-shader-cache
fi
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/usr/local/lib/b2l-gl --libdir=. --buildtype=release -Db_ndebug=$NDEBUG \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=enabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled -Dgallium-va=disabled \
	-Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
echo "b_ndebug=$NDEBUG" > $BUILD/.b2l-ndebug
echo "shader-cache=enabled" > $BUILD/.b2l-shader-cache
ninja -C $BUILD > ninja.log
# B1L-0070: libhakiosair (the Metal driver's compiler library) is not a default target: built by name
ninja -C $BUILD src/gallium/targets/hakiosair/libhakiosair.dylib >> ninja.log
"$(xcrun -f strip)" -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
install_name_tool -id /usr/local/lib/b2l-gl/libOSMesa.8.dylib out/libOSMesa.8.dylib
/usr/bin/codesign --force --sign - out/libOSMesa.8.dylib
nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
cp mesa-$VERSION/src/mapi/glapi/registry/gl.xml out/gl.xml
echo "build-mesa.sh: Mesa $VERSION ($DRIVERS): $B/out/libOSMesa.8.dylib, $(stat -f %z out/libOSMesa.8.dylib) bytes, $(wc -l < out/exported-gl.txt) gl* functions"
