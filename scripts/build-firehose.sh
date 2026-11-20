#!/bin/bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base="${B2L_BUILD_ENV:-$repo_root/out/build-env}"
if [[ -f "${B2L_BUILD_ENV:-}/env.sh" ]]; then
  source "$B2L_BUILD_ENV/env.sh"
elif [[ -f "$base/env.sh" ]]; then
  source "$base/env.sh"
fi
: "${KDKROOT:?Set KDKROOT to the merged KDK and KDK_SDK payload root}"
: "${B2L_DEPSROOT:?Set B2L_DEPSROOT to the isolated dependency installation root}"
src="${LIBDISPATCH_SRC:-$base/src/libdispatch}"
xnu_path="${XNU_PATH:-$repo_root/xnu}"

mkdir -p "$base/firehose-obj" "$base/firehose-sym" "$B2L_DEPSROOT/usr/local/lib/kernel"

echo "Building libfirehose_kernel..."
set +e
xcodebuild -project "$src/libdispatch.xcodeproj" \
  -target libfirehose_kernel \
  -configuration Release \
  -sdk iphoneos \
  -arch arm64 \
  OBJROOT="$base/firehose-obj" \
  SYMROOT="$base/firehose-sym" \
  DSTROOT="$B2L_DEPSROOT" \
  CODE_SIGNING_ALLOWED=NO \
  OTHER_CFLAGS="-mkernel -Wno-packed -mcpu=generic -march=armv8-a+crypto -w -I$B2L_DEPSROOT/usr/local/include -I$B2L_DEPSROOT/usr/local/include/kernel -I$B2L_DEPSROOT/usr/include -I$xnu_path/bsd -I$xnu_path/osfmk -I$xnu_path/libkern -I$src/private -I$src/private/firehose" \
  HEADER_SEARCH_PATHS="$B2L_DEPSROOT/usr/local/include $B2L_DEPSROOT/usr/include $B2L_DEPSROOT/usr/local/include/kernel $B2L_DEPSROOT/System/Library/Frameworks/Kernel.framework/PrivateHeaders $B2L_DEPSROOT/System/Library/Frameworks/Kernel.framework/Headers $src $src/private $src/private/firehose $src/private/os $B2L_DEPSROOT/usr/local/include/os $B2L_DEPSROOT/usr/local/include/firehose $KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders $KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/Headers $xnu_path/bsd $xnu_path/osfmk $xnu_path/libkern" \
  install
status=$?
set -e

if [[ $status -ne 0 ]] || [[ ! -f "$B2L_DEPSROOT/usr/local/lib/kernel/liblibfirehose_kernel.a" && ! -f "$B2L_DEPSROOT/usr/local/lib/kernel/libfirehose_kernel.a" ]]; then
  echo "xcodebuild did not produce target library; falling back to direct clang compilation..."
  sdk_path="$(xcrun --sdk iphoneos --show-sdk-path)"
  xcrun clang -target arm64-apple-ios26.5 -mcpu=generic -march=armv8-a+crypto \
    -isysroot "$sdk_path" \
    -mkernel -Wno-packed -O2 -w \
    -DKERNEL=1 -DDISPATCH_USE_DTRACE=0 \
    -DOS_ATOMIC_CONFIG_MEMORY_ORDER_DEPENDENCY=1 -DOS_ATOMIC_CONFIG_STARVATION_FREE_ONLY=0 \
    -I"$B2L_DEPSROOT/usr/local/include" \
    -I"$B2L_DEPSROOT/usr/local/include/kernel" \
    -I"$B2L_DEPSROOT/usr/include" \
    -I"$B2L_DEPSROOT/System/Library/Frameworks/Kernel.framework/PrivateHeaders" \
    -I"$B2L_DEPSROOT/System/Library/Frameworks/Kernel.framework/Headers" \
    -I"$src" -I"$src/private" -I"$src/private/firehose" -I"$src/private/os" \
    -I"$xnu_path/bsd" -I"$xnu_path/osfmk" -I"$xnu_path/libkern" \
    -I"$KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders" \
    -I"$KDKROOT/System/Library/Frameworks/Kernel.framework/Versions/A/Headers" \
    -c "$src/src/firehose/firehose_buffer.c" \
    -o "$base/firehose-obj/firehose_buffer.o"

  libtool -static -o "$B2L_DEPSROOT/usr/local/lib/kernel/libfirehose_kernel.a" "$base/firehose-obj/firehose_buffer.o" 2>/dev/null || \
    ar -rcs "$B2L_DEPSROOT/usr/local/lib/kernel/libfirehose_kernel.a" "$base/firehose-obj/firehose_buffer.o"
  ranlib "$B2L_DEPSROOT/usr/local/lib/kernel/libfirehose_kernel.a" 2>/dev/null || true
fi

if [[ -f "$B2L_DEPSROOT/usr/local/lib/kernel/liblibfirehose_kernel.a" ]]; then
  cp "$B2L_DEPSROOT/usr/local/lib/kernel/liblibfirehose_kernel.a" "$B2L_DEPSROOT/usr/local/lib/kernel/libfirehose_kernel.a"
fi
