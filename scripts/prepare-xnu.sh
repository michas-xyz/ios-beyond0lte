#!/usr/bin/env bash
# Apply the board patch only to the pinned upstream revision. Never reset a checkout.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
xnu_path="${1:-${XNU_PATH:-$repo_root/xnu}}"
expected=ac9718fb1af618d5ce8678d0dc6e8a58f252216f
[[ "$(git -C "$xnu_path" rev-parse HEAD)" == "$expected" ]] || {
  echo "XNU must be at $expected" >&2; exit 1;
}
python3 "$repo_root/scripts/apply-board-patches.py" "$xnu_path" \
  "$repo_root/patches/0001-b2l-machine-and-local-dependencies.patch" \
  "$repo_root/patches/0002-b2l-builtin-hfs-root.patch" \
  "$repo_root/patches/0003-b2l-hfs-md5.patch" \
  "$repo_root/patches/0004-b2l-unavailable-amfi-entitlements.patch" \
  "$repo_root/patches/0005-b2l-userdata-root.patch" \
  "$repo_root/patches/0006-b2l-arm64e-compat.patch" \
  "$repo_root/patches/0007-b2l-disable-signature-enforcement.patch" \
  "$repo_root/patches/0008-b2l-userspace-progress-trace.patch" \
  "$repo_root/patches/0009-b2l-first-compat-trap-stages.patch" \
  "$repo_root/patches/0010-b2l-scalar-instruction-fetch.patch" \
  "$repo_root/patches/0011-b2l-native-pan-copyio.patch" \
  "$repo_root/patches/0012-b2l-unsigned-shared-cache.patch" \
  "$repo_root/patches/0013-b2l-unsigned-executable-boothash.patch" \
  "$repo_root/patches/0014-b2l-arm64e-dyld-page-fixups.patch" \
  "$repo_root/patches/0015-b2l-sandbox-container-query.patch" \
  "$repo_root/patches/0016-b2l-registered-sandbox-policy.patch" \
  "$repo_root/patches/0017-b2l-development-amfi-metadata.patch" \
  "$repo_root/patches/0018-b2l-prelockdown-policy-registration.patch" \
  "$repo_root/patches/0019-b2l-unsigned-code-identity.patch" \
  "$repo_root/patches/0020-b2l-volatile-development-keystore.patch" \
  "$repo_root/patches/0021-b2l-persistent-system-log.patch" \
  "$repo_root/patches/0022-b2l-two-core-startup.patch" \
  "$repo_root/patches/0023-b2l-automatic-native-cache.patch" \
  "$repo_root/patches/0024-b2l-bounded-crash-backtrace.patch" \
  "$repo_root/patches/0025-b2l-platform-status-and-watchdog.patch" \
  "$repo_root/patches/0026-b2l-persona-manifest-bootstrap.patch" \
  "$repo_root/patches/0027-b2l-der-entitlement-query.patch" \
  "$repo_root/patches/0028-b2l-smp-startup-diagnostics.patch" \
  "$repo_root/patches/0029-b2l-ordered-cpu-registration.patch" \
  "$repo_root/patches/0030-b2l-cpu-startup-context.patch" \
  "$repo_root/patches/0031-b2l-first-secondary-panic.patch" \
  "$repo_root/patches/0032-b2l-reject-secondary-affinity-alias.patch" \
  "$repo_root/patches/0033-b2l-el1-affinity-translation.patch" \
  "$repo_root/patches/0034-b2l-cdhash-metadata-query.patch" \
  "$repo_root/patches/0035-b2l-service-fault-detail.patch" \
  "$repo_root/patches/0036-b2l-signature-presence-status.patch" \
  "$repo_root/patches/0037-b2l-hfs-regular-file-swap.patch" \
  "$repo_root/patches/0038-b2l-owned-entitlement-query-bridges.patch" \
  "$repo_root/patches/0039-b2l-surface-framebuffer-hid-services.patch" \
  "$repo_root/patches/0040-b2l-physical-gpio-touch-input.patch" \
  "$repo_root/patches/0041-b2l-container-query-diagnostics.patch" \
  "$repo_root/patches/0042-b2l-targeted-ipc-wait-diagnostics.patch" \
  "$repo_root/patches/0043-b2l-ipc-copyout-and-right-lifetime.patch" \
  "$repo_root/patches/0044-b2l-service-exception-diagnostics.patch" \
  "$repo_root/patches/0045-b2l-mach-provenance-diagnostics.patch" \
  "$repo_root/patches/0046-b2l-anonymous-forward-connection-label.patch" \
  "$repo_root/patches/0047-b2l-null-safe-debugger-entry.patch" \
  "$repo_root/patches/0048-b2l-pinned-extension-cache-service.patch" \
  "$repo_root/patches/0049-b2l-eight-cpu-m4-trial.patch" \
  "$repo_root/patches/0050-b2l-m4-measured-identity.patch" \
  "$repo_root/patches/0051-b2l-explicit-generic-cpu-target.patch" \
  "$repo_root/patches/0052-b2l-exact-detached-cache-admission.patch" \
  "$repo_root/patches/0053-b2l-three-tier-edge-scheduler.patch" \
  "$repo_root/patches/0054-b2l-isolate-storage-scheduling-group.patch" \
  "$repo_root/patches/0055-b2l-discontiguous-owned-ram.patch" \
  "$repo_root/patches/0056-b2l-scalar-pacga-siphash.patch" \
  "$repo_root/patches/0057-b2l-scoped-coregraphics-backtrace.patch" \
  "$repo_root/patches/0058-b2l-owned-launch-cache.patch" \
  "$repo_root/patches/0059-b2l-typed-critical-exit.patch" \
  "$repo_root/patches/0100-b2l-installd-apfs-fstype.patch" \
  "$repo_root/patches/0101-b2l-report-arm64e-host-subtype.patch" \
  "$repo_root/patches/0102-b2l-hfs-decmpfs-zlib-stored.patch" \
  "$repo_root/patches/0103-b2l-kernel-rename-excl.patch" \
  "$repo_root/patches/0105-b2l-phys-cache-op-unowned-page.patch" \
  "$repo_root/patches/0106-b2l-pmap-owned-ram-is-dram-and-ptoa.patch" \
  "$repo_root/patches/0108-b2l-el0-cache-maintenance.patch"
# Port files have separate provenance and are applied as an overlay.
rsync -a "$repo_root/xnu-overlay/" "$xnu_path/"
# The keyed PACGA (QARMA5/SipHash) is gone; historical patches 0006/0056 still list its sources in conf/files.
perl -ni -e 'print unless m{b2l_pacga_(qarma5|siphash)}' "$xnu_path/bsd/conf/files" "$xnu_path/osfmk/conf/files"
# Register additional B2L services and crypto descriptors in conf/files
grep -q "b2l_mali_core.c" "$xnu_path/bsd/conf/files" || echo "bsd/dev/b2l_mali_core.c optional b2l" >> "$xnu_path/bsd/conf/files"
grep -q "B2LMaliService.cpp" "$xnu_path/iokit/conf/files" || echo "iokit/Kernel/arm/B2LMaliService.cpp optional iokitcpp b2l" >> "$xnu_path/iokit/conf/files"
grep -q "B2LMetalAccel.cpp" "$xnu_path/iokit/conf/files" || echo "iokit/Kernel/arm/B2LMetalAccel.cpp optional iokitcpp b2l" >> "$xnu_path/iokit/conf/files"
grep -q "B2LBluetooth.cpp" "$xnu_path/iokit/conf/files" || echo "iokit/Kernel/arm/B2LBluetooth.cpp optional iokitcpp b2l" >> "$xnu_path/iokit/conf/files"
grep -q "b2l_prof.c" "$xnu_path/osfmk/conf/files" || echo "osfmk/arm64/b2l_prof.c optional b2l" >> "$xnu_path/osfmk/conf/files"
grep -q "b2l_ccsha1_ltc.c" "$xnu_path/osfmk/conf/files" || echo "osfmk/corecrypto/b2l_ccsha1_ltc.c optional b2l" >> "$xnu_path/osfmk/conf/files"
python3 "$repo_root/scripts/build-userspace-bootstrap.py" --output "$xnu_path/bsd/kern/b2l_userspace_payload.h"
for source_file in "$repo_root/common/cache/"*; do
  destination="$xnu_path/bsd/kern/$(basename "$source_file")"
  if [[ -f "$destination" ]] && ! cmp -s "$source_file" "$destination"; then
    echo "Refusing different generated cache source: $destination" >&2; exit 1
  fi
  cp "$source_file" "$destination"
done
echo "Prepared B2L source: $xnu_path"

for source_file in "$repo_root/common/smp/"*; do
  destination="$xnu_path/iokit/Kernel/arm/$(basename "$source_file")"
  if [[ -f "$destination" ]] && ! cmp -s "$source_file" "$destination"; then
    echo "Refusing different generated SMP header: $destination" >&2; exit 1
  fi
  cp "$source_file" "$destination"
done
