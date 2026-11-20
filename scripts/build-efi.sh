#!/usr/bin/env bash
# Build standalone applications; the firmware platform is an external dependency.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${WORKSPACE:?Set WORKSPACE to the prepared Mu/EDK II workspace}"
export WORKSPACE
export EDK_TOOLS_PATH="${EDK_TOOLS_PATH:-$WORKSPACE/Mu_Basecore/BaseTools}"
export EDK_TOOLS_BIN="${EDK_TOOLS_BIN:-$EDK_TOOLS_PATH/Source/C/bin}"
export PYTHON_COMMAND="${PYTHON_COMMAND:-python3}"
export PYTHONPATH="${EDK_TOOLS_PATH}/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
: "${CLANG_BIN:?Set CLANG_BIN to a directory containing clang, llvm-lib, llvm-rc and lld-link}"
export CLANG_BIN="${CLANG_BIN%/}/"
export PACKAGES_PATH="$WORKSPACE:$WORKSPACE/Mu_Basecore${PACKAGES_PATH:+:$PACKAGES_PATH}"
export PATH="$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_BIN:$CLANG_BIN:$PATH"
# Fallback for llvm-lib if only llvm-ar is present
if [[ ! -x "${CLANG_BIN}llvm-lib" ]] && command -v llvm-ar >/dev/null 2>&1; then
  ln -sf "$(command -v llvm-ar)" "${CLANG_BIN}llvm-lib" 2>/dev/null || true
fi
for tool in clang llvm-lib llvm-rc lld-link; do
  [[ -x "$CLANG_BIN$tool" ]] || { echo "Missing tool: $CLANG_BIN$tool" >&2; exit 1; }
done
# Ensure GenFw is in EDK_TOOLS_BIN
if [[ ! -x "$EDK_TOOLS_BIN/GenFw" ]] && [[ -d "$EDK_TOOLS_PATH/Bin/Linux-x86-64" ]]; then
  mkdir -p "$EDK_TOOLS_BIN"
  cp -r "$EDK_TOOLS_PATH/Bin/Linux-x86-64/"* "$EDK_TOOLS_BIN/" 2>/dev/null || true
fi
[[ -x "$EDK_TOOLS_BIN/GenFw" ]] || { echo 'Build native BaseTools first.' >&2; exit 1; }
mkdir -p "$WORKSPACE/B2LXnuPkg"
rsync -a "$repo_root/uefi/" "$WORKSPACE/B2LXnuPkg/"
rsync -a "$repo_root/common/cache/" "$WORKSPACE/B2LXnuPkg/XnuLoader/"
rsync -a "$repo_root/common/smp/" "$WORKSPACE/B2LXnuPkg/XnuLoader/"
rsync -a "$repo_root/common/memory/" "$WORKSPACE/B2LXnuPkg/XnuLoader/"
# Optional board display profile for the loader, e.g. B2L_FB_LOGICAL=880x1912 B2L_DISPLAY_SCALE=2.
display_options=()
if [[ -n "${B2L_FB_LOGICAL:-}" ]]; then
  [[ "$B2L_FB_LOGICAL" =~ ^([0-9]+)x([0-9]+)$ ]] || { echo "B2L_FB_LOGICAL must be WxH" >&2; exit 1; }
  display_options+=(-D "B2L_FB_LOGICAL_W=${BASH_REMATCH[1]}" -D "B2L_FB_LOGICAL_H=${BASH_REMATCH[2]}")
fi
if [[ -n "${B2L_DISPLAY_SCALE:-}" ]]; then display_options+=(-D "B2L_DISPLAY_SCALE=$B2L_DISPLAY_SCALE"); fi
cd "$WORKSPACE"
exec "$EDK_TOOLS_PATH/BinWrappers/PosixLike/build" \
  -a AARCH64 -t CLANGPDB -b RELEASE -n "${BUILD_JOBS:-4}" \
  -p "${DSC_PATH:-B2LXnuPkg/B2LXnuPkg.dsc}" ${display_options[@]+"${display_options[@]}"} "$@"
