#!/bin/bash
#
# Build AMDXHCIPreflight.kext on macOS.
#
#   ./build.sh            # x86_64, the only arch an AMD Hackintosh needs
#
# Requires the Xcode command line tools (no full Xcode project needed).
# The kext is ad-hoc signed at the end, which is what OpenCore's kernel
# injection expects; for /Library/Extensions use your usual signing identity.
#
set -eu

cd "$(dirname "$0")"
CORE=../core
HEADER="$CORE/AMDXHCIPreflight.h"
KEXT=AMDXHCIPreflight.kext
OUT=build

[ -f "$HEADER" ] || { echo "missing $HEADER"; exit 1; }

SDK="$(xcrun --sdk macosx --show-sdk-path)"
KHEADERS="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$KHEADERS" ] || { echo "no kernel headers in SDK: $KHEADERS"; exit 1; }

CFLAGS=(
	-arch x86_64
	-mkernel
	-nostdinc++
	-fno-rtti
	-fno-exceptions
	-fno-builtin
	-fno-common
	-fno-stack-protector
	-std=gnu++14
	-O2
	-DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D__KERNEL__
	-I"$KHEADERS"
	-I"$CORE"
	-Wall -Wno-unused-parameter
)

rm -rf "$OUT" "$KEXT"
mkdir -p "$OUT" "$KEXT/Contents/MacOS"

echo "[1/4] compiling"
clang++ "${CFLAGS[@]}" -c AMDXHCIPreflight.cpp -o "$OUT/AMDXHCIPreflight.o"

echo "[2/4] linking"
# -Xlinker -kext produces an MH_KEXT_BUNDLE; libkmod supplies the kmod entry
# points, libcc_kext the compiler runtime pieces the kernel does not export.
clang++ -arch x86_64 -mkernel -nostdlib -r -Xlinker -kext \
	-o "$OUT/AMDXHCIPreflight" \
	"$OUT/AMDXHCIPreflight.o" \
	-L"$SDK/usr/lib" -lcc_kext -lkmod -lkmodc++

echo "[3/4] assembling"
cp Info.plist "$KEXT/Contents/Info.plist"
cp "$OUT/AMDXHCIPreflight" "$KEXT/Contents/MacOS/AMDXHCIPreflight"
plutil -lint "$KEXT/Contents/Info.plist"

echo "[4/4] signing (ad-hoc)"
codesign --force --deep --sign - "$KEXT"
codesign -dv "$KEXT" 2>&1 | sed -n '1,4p'

echo
echo "built: $(pwd)/$KEXT"
echo "verify with: kmutil showloaded --list-only | grep -i amdxhci   (after a reboot)"
