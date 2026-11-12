#!/bin/bash
#
# Build GenericUSBXHCI.kext from the patched fork WITHOUT Xcode.
#
#   ./build_gux.sh <patched-source-tree> <output-dir>
#
# The fork ships an Xcode project, but Command Line Tools alone can produce the
# same bundle: 26 translation units, one link, three fixes that xcodebuild used
# to do for us.
#
#   1. Info.plist is an Xcode *template*: ${PRODUCT_NAME} and friends must be
#      expanded or the bundle gets a literal identifier containing "${".
#   2. The template is CRLF in a Windows checkout, and IOCFUnserialize refuses
#      CRLF inside the plist.
#   3. A bare clang++ link has no _kmod_info, which the kernel requires; see
#      kmod_info.c below.
#
set -u

SRC="${1:-$PWD}"
OUT="${2:-$PWD/out}"
VERSION="1.3.0b2"
BUNDLE_ID="net.osx86.kexts.GenericUSBXHCI"

SDK=$(xcrun --sdk macosx --show-sdk-path)
KH="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
[ -d "$KH" ] || { echo "no kernel headers in $SDK"; exit 1; }

cd "$SRC" || exit 1
rm -rf obj "$OUT"; mkdir -p obj "$OUT"

CFLAGS="-arch x86_64 -mkernel -nostdinc++ -fno-rtti -fno-exceptions -fno-builtin
 -fno-common -fno-stack-protector -std=gnu++14 -O2 -w
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -D__KERNEL__
 -I$KH -IGenericUSBXHCI"

echo "[1/4] compiling $(ls GenericUSBXHCI/*.cpp | wc -l | tr -d ' ') sources"
fail=0
for f in GenericUSBXHCI/*.cpp; do
	b=$(basename "$f" .cpp)
	if ! clang++ $CFLAGS -c "$f" -o "obj/$b.o" 2> "obj/$b.err"; then
		echo "FAILED: $f"; head -25 "obj/$b.err"; fail=1
	fi
done
[ $fail -eq 0 ] || { echo "compile errors; see obj/*.err"; exit 2; }

cat > obj/kmod_info.c <<EOF
#include <mach/kmod.h>
/* __realmain is not exported by anything in macOS; supply it and run the C++
   static constructors it is responsible for (see kext/build.sh for the long
   version of this note). */
typedef void (* pf_init_fn)(void);
extern pf_init_fn pf_init_begin[] __asm("section\$start\$__DATA\$__mod_init_func");
extern pf_init_fn pf_init_end[]   __asm("section\$end\$__DATA\$__mod_init_func");
extern void IOLog(const char * fmt, ...);
static kern_return_t pf_realmain(kmod_info_t * ki, void * d)
{
	static int done; pf_init_fn * p; int n = 0;
	(void)ki; (void)d;
	if (!done) {
		done = 1;
		if (pf_init_begin && pf_init_end)
			for (p = pf_init_begin; p < pf_init_end; p++)
				if (*p) { (*p)(); n++; }
		IOLog("GenericUSBXHCI: entry ok, %d static ctor(s) run\n", n);
	}
	return 0;
}
typedef struct { kern_return_t (*entry)(kmod_info_t *, void *); } pf_realm_t;
pf_realm_t _realmain = { pf_realmain };          /* symbol __realmain */
kmod_info_t kmod_info = {
	0, KMOD_INFO_VERSION, -1U,
	{ "$BUNDLE_ID" },
	{ "$VERSION" },
	-1, 0, 0, 0, 0,
	pf_realmain, 0
};
EOF
clang -arch x86_64 -mkernel -w -DKERNEL -DKERNEL_PRIVATE -D__KERNEL__ \
	-I"$KH" -c obj/kmod_info.c -o obj/kmod_info.o || exit 3

echo "[2/4] linking"
clang++ -arch x86_64 -mkernel -nostdlib -r -Xlinker -kext \
	-o "$OUT/GenericUSBXHCI" obj/*.o \
	-L"$SDK/usr/lib" -lcc_kext -lkmod -lkmodc++ || { echo "link failed"; exit 4; }

echo "[3/4] assembling"
K="$OUT/GenericUSBXHCI.kext"
mkdir -p "$K/Contents/MacOS"
sed 's/\r$//' GenericUSBXHCI/GenericUSBXHCI-Info.plist > "$K/Contents/Info.plist"

python3 - "$K/Contents/Info.plist" "$VERSION" <<'PYEOF' || exit 5
import sys
p, ver = sys.argv[1], sys.argv[2]
s = open(p, encoding='utf-8').read()
for k, v in [
    ('${EXECUTABLE_NAME}', 'GenericUSBXHCI'),
    ('${PRODUCT_NAME:rfc1034identifier}', 'GenericUSBXHCI'),
    ('${PRODUCT_NAME}', 'GenericUSBXHCI'),
    ('${MODULE_VERSION}', ver),
    ('${COPYRIGHT_NOTICE}', '(c) 2012-2014 Zenith432, GPLv2'),
    ('${DYLIB_COMPATIBILITY_VERSION}', '1.0.0'),
    ('${CONFIGURATION}', 'Release'),
    ('${LOGNAME}', 'build'),
    # Renamed in the 10.15 era: the USB stack is IOUSBHostFamily now, and the
    # old identifier makes the kext unloadable ("dependency ... was not found").
    ('com.apple.iokit.IOUSBFamily', 'com.apple.iokit.IOUSBHostFamily'),
    ('<string>5.0.0a1</string>', '<string>1.0.0</string>'),
]:
    s = s.replace(k, v)
assert '${' not in s, 'unexpanded variable left in Info.plist'
open(p, 'w', encoding='utf-8').write(s)
PYEOF

cp "$OUT/GenericUSBXHCI" "$K/Contents/MacOS/GenericUSBXHCI"
plutil -lint "$K/Contents/Info.plist" || exit 6

echo "[4/4] signing (ad-hoc)"
codesign --force --deep --sign - "$K" 2>&1 | tail -1

echo
echo "built: $K"
file "$K/Contents/MacOS/GenericUSBXHCI"
plutil -p "$K/Contents/Info.plist" | grep -E 'CFBundleIdentifier|IOProbeScore' | head -2
