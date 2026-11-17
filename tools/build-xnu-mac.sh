#!/bin/bash
# Build the ios-sweet XNU kernel locally on macOS with a current Xcode.
#
# GitHub's macos-14 runners (Xcode 15) are gone, so this replaces the CI
# workflow: blacktop/darwin-xnu-build at the pinned commit, xnu-8796
# (macOS 13.5), all patches from xnu/patches (0082 makes current iig work).
# Tested 2026-11-17 with Xcode 27.2 on macOS 27.2: kernel.development.sweet.
#
# usage: tools/build-xnu-mac.sh WORKDIR [MACHINE_CONFIG]
#   env: KDKROOT   unpacked Kernel Debug Kit 13.5 (22G74), e.g. from
#                  pkgutil --expand-full KernelDebugKit.pkg (Payload dir)
#        TOOLS_BIN directory with jq, gum, xcodes, ipsw, cmake, ninja
#                  (stand-ins are fine for gum/xcodes/ipsw)
#        CLEAN=1   start from fresh xnu sources (re-applies patches)
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:?usage: build-xnu-mac.sh WORKDIR [MACHINE_CONFIG]}
MACHINE=${2:-SWEET}
: "${KDKROOT:?set KDKROOT to the unpacked KDK_13.5_22G74.kdk}"
: "${TOOLS_BIN:?set TOOLS_BIN to the directory with gum/xcodes/ipsw/cmake/ninja}"
DXB_REPO=https://github.com/blacktop/darwin-xnu-build
DXB_SHA=663906528d1cd5ea8fde7de6f16de73db9511549

mkdir -p "$WORK"
cd "$WORK"
if [ ! -d dxb/.git ]; then
	git init -q dxb
	git -C dxb fetch -q --depth 1 "$DXB_REPO" "$DXB_SHA"
	git -C dxb checkout -q FETCH_HEAD
fi
cd dxb
if [ "${CLEAN:-0}" = 1 ]; then
	rm -rf xnu build fakeroot
	git checkout -q -- build.sh
fi
# 1) keep TOOLS_BIN on PATH after build.sh resets it to Xcode/system paths
grep -q EXTRA_TOOL_PATH build.sh ||
	sed -i '' 's|    export PATH="${XCODE_TOOLCHAIN}:${XCODE_USR_BIN}:${SYSTEM_PATHS}${HOMEBREW_PATHS}"|    export PATH="${XCODE_TOOLCHAIN}:${XCODE_USR_BIN}:${SYSTEM_PATHS}${HOMEBREW_PATHS}${EXTRA_TOOL_PATH:+:${EXTRA_TOOL_PATH}}"|' build.sh
# 2) libfirehose_kernel is built arm64e-only; the SWEET kernel is plain arm64 (as in CI)
sed -i '' 's/ARCHS="x86_64 arm64e" VALID_ARCHS="x86_64 arm64e"/ARCHS="x86_64 arm64e arm64" VALID_ARCHS="x86_64 arm64e arm64"/' build.sh
# 3) re-runs must not re-apply patches (the staging scripts refuse to overwrite)
grep -q 'patches-applied' build.sh || /usr/bin/python3 -I - <<'EOF'
s = open('build.sh').read()
old = 'patches() {\n    running "🩹 Patching xnu files"\n'
assert s.count(old) == 1
s = s.replace(old, 'patches() {\n    if [ -f "${WORK_DIR}/xnu/.patches-applied" ]; then\n        info "xnu already patched (xnu/.patches-applied): skipping"\n        return 0\n    fi\n    running "🩹 Patching xnu files"\n')
open('build.sh', 'w').write(s)
EOF
cp "$HERE"/xnu/patches/*.patch "$HERE"/xnu/patches/*.sh patches/

# First run applies the patches; mark the tree once xnu exists and patched.
run() {
	EXTRA_TOOL_PATH="$TOOLS_BIN" PATH="$TOOLS_BIN:$PATH" MACOS_VERSION=13.5 \
	KERNEL_CONFIG=DEVELOPMENT MACHINE_CONFIG="$MACHINE" KDKROOT="$KDKROOT" \
	CMAKE_POLICY_VERSION_MINIMUM=3.5 MAKEFLAGS=-k bash build.sh
}
if [ -d xnu ] && [ ! -f xnu/.patches-applied ]; then
	echo "xnu/ exists but is not marked patched: rerun with CLEAN=1" >&2
	exit 1
fi
if [ ! -d xnu ]; then
	# build.sh clones xnu, then patches it; touch the marker after that step
	run || true
	[ -d xnu ] && touch xnu/.patches-applied
fi
run
ls -l fakeroot/System/Library/Kernels/
