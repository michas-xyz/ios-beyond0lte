#!/usr/bin/env bash
# Reproduce AvailabilityVersions' CMake "install" (non-DriverKit, kernel headers on)
# without cmake/ninja. Usage: install-availability.sh <AvailabilityVersions src> <DSTROOT>
set -euo pipefail
src="$1"; dst="$2"; obj="$(mktemp -d)"
trap 'rm -rf "$obj"' EXIT
"$src/availability" --av_version AvailabilityVersions-157.2 --preprocess "$src/availability" "$obj/availability"
chmod 755 "$obj/availability"
for t in "$src"/templates/*; do "$obj/availability" --preprocess "$t" "$obj/$(basename "$t")"; done

install -d "$dst/usr/local/libexec" "$dst/usr/local/include/dyld" "$dst/usr/local/include/mach-o" \
  "$dst/usr/include/os" "$dst/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/os" \
  "$dst/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders"
install -m 755 "$obj/availability" "$dst/usr/local/libexec/availability.pl"
for h in VersionMap.h for_dyld_priv.inc; do install -m 644 "$obj/$h" "$dst/usr/local/include/dyld/"; done
install -m 644 "$obj/dyld_version_defines.h" "$dst/usr/local/include/mach-o/"
for h in AvailabilityInternalPrivate.h AvailabilityProhibitedInternal.h; do
  install -m 644 "$obj/$h" "$dst/usr/local/include/"
  ln -sfn "../../../../../../../usr/local/include/$h" "$dst/System/Library/Frameworks/Kernel.framework/Versions/A/PrivateHeaders/$h"
done
install -m 644 "$obj/os_availability.h" "$dst/usr/include/os/availability.h"
ln -sfn "../../../../../../../../usr/include/os/availability.h" "$dst/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/os/availability.h"
for h in Availability.h AvailabilityInternal.h AvailabilityInternalLegacy.h AvailabilityMacros.h AvailabilityVersions.h; do
  install -m 644 "$obj/$h" "$dst/usr/include/"
  ln -sfn "../../../../../../../usr/include/$h" "$dst/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/$h"
done
echo "AvailabilityVersions installed into $dst"
