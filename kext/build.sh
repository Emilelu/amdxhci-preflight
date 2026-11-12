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

# Two symbols the kernel requires and that only the Xcode kernel-extension
# product type normally emits:
#
#   _kmod_info   libkmod's c_start.o only holds an undefined reference to it.
#   __realmain   c_start.o's __start does `rax = __realmain; jmp [rax]`, i.e. it
#                reads a pointer to a struct whose first field is the real entry
#                point. Nothing in the SDK defines it, so we do, pointing at our
#                own entry stub: the kernel drives IOService matching from the
#                personality table, the stub only has to report success.
cat > "$OUT/kmod_info.c" <<'EOF'
#include <mach/kmod.h>

/*
 * Nothing in macOS exports __realmain (verified: `nm -u` on the kernel and on
 * libkmod/libkmodc++/libcc_kext shows no definition, and a kext that leaves the
 * reference open fails with "could not find a kext which exports this symbol").
 * It is normally synthesised by the Xcode kernel-extension product type, and
 * its job is to run the C++ static constructors before the kext is used.
 *
 * OSMetaClass registration lives in exactly those constructors
 * (__DATA,__mod_init_func), so a stub that returned success without running
 * them would load, match nothing, and silently do nothing. Walk the section.
 */
typedef void (* pf_init_fn)(void);
extern pf_init_fn pf_init_begin[] __asm("section$start$__DATA$__mod_init_func");
extern pf_init_fn pf_init_end[]   __asm("section$end$__DATA$__mod_init_func");
extern void IOLog(const char * fmt, ...);   /* com.apple.kpi.iokit */

static kern_return_t pf_realmain(kmod_info_t * ki, void * d)
{
	static int done;
	pf_init_fn * p;
	int n = 0;

	(void) ki; (void) d;
	if (!done) {
		done = 1;
		if (pf_init_begin && pf_init_end) {
			for (p = pf_init_begin; p < pf_init_end; p++) {
				if (*p) { (*p)(); n++; }
			}
		}
		/* Greppable proof that the entry point really ran, with the
		 * number of C++ constructors (i.e. OSMetaClass registrations). */
		IOLog("AMDXHCIPreflight: entry ok, %d static ctor(s) run\n", n);
	}
	return 0;                       /* KERN_SUCCESS */
}

/* Symbol __realmain (C: `_realmain`); __start jumps through [0] of it. */
typedef struct { kern_return_t (*entry)(kmod_info_t *, void *); } pf_realm_t;
pf_realm_t _realmain = { pf_realmain };

kmod_info_t kmod_info = {
	0, KMOD_INFO_VERSION, -1U,
	{ "com.yg.AMDXHCIPreflight" },
	{ "1.0.0" },
	-1, 0, 0, 0, 0,
	pf_realmain, 0
};
EOF
clang -arch x86_64 -mkernel -O2 -w \
	-DKERNEL -DKERNEL_PRIVATE -D__KERNEL__ -I"$KHEADERS" \
	-c "$OUT/kmod_info.c" -o "$OUT/kmod_info.o"

echo "[2/4] linking"
# -Xlinker -kext produces an MH_KEXT_BUNDLE; libkmod supplies the kmod entry
# points, libcc_kext the compiler runtime pieces the kernel does not export.
clang++ -arch x86_64 -mkernel -nostdlib -r -Xlinker -kext \
	-o "$OUT/AMDXHCIPreflight" \
	"$OUT/AMDXHCIPreflight.o" "$OUT/kmod_info.o" \
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
