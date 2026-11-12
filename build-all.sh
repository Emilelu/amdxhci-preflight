#!/bin/bash
#
# build-all.sh — one-shot build for everything in this folder, run ON macOS.
#
#   ./build-all.sh                 # tests + standalone kext + GUX kext
#   ./build-all.sh --core-only     # host-side unit tests only (works on Linux too)
#
# Produces:
#   out/AMDXHCIPreflight.kext
#   out/GenericUSBXHCI.kext
#   out/build-report.txt           <- paste this back if anything fails
#
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
OUT="$HERE/out"
GUX_REPO="${GUX_REPO:-https://github.com/RattletraPM/GUX-RyzenXHCIFix.git}"
LOG="$OUT/build-report.txt"
CORE_ONLY=0
[ "${1:-}" = "--core-only" ] && CORE_ONLY=1

rm -rf "$OUT"; mkdir -p "$OUT"
: > "$LOG"
say() { echo "$@" | tee -a "$LOG"; }
run() { echo "\$ $*" >> "$LOG"; "$@" >> "$LOG" 2>&1; return $?; }

say "=== build-all $(date -u +%Y-%m-%dT%H:%M:%SZ) on $(uname -sm) ==="
say "clang: $(clang --version 2>/dev/null | head -1)"

# ---------------------------------------------------------------- 1. core tests
say ""
say "### [1/3] host-side unit tests"
if run bash core/build_and_test.sh; then
	say "core tests: PASS"
else
	say "core tests: FAIL — see $LOG"
	exit 1
fi

[ $CORE_ONLY -eq 1 ] && { say ""; say "core-only run finished."; exit 0; }

if [ "$(uname -s)" != "Darwin" ]; then
	say ""
	say "NOT macOS — cannot build kernel extensions here."
	say "Run this script on the Mac (or mount the folder over SMB/scp) and rerun."
	exit 2
fi

# ------------------------------------------------------------ 2. standalone kext
say ""
say "### [2/3] AMDXHCIPreflight.kext (standalone, IOService based)"
if run bash kext/build.sh; then
	cp -R kext/AMDXHCIPreflight.kext "$OUT/" 2>/dev/null
	say "standalone kext: OK -> out/AMDXHCIPreflight.kext"
else
	say "standalone kext: FAIL — tail of log:"
	tail -25 "$LOG" | tee -a /dev/null
fi

# --------------------------------------------------------------- 3. GUX kext
say ""
say "### [3/3] GenericUSBXHCI.kext (patched GUX fork)"
WORK="$HERE/.guxbuild"
rm -rf "$WORK"
if [ -n "${GUX_SRC:-}" ] && [ -d "${GUX_SRC:-}" ]; then
	say "using local source: $GUX_SRC"
	rsync -a --exclude .git "$GUX_SRC"/ "$WORK"/
else
	if ! run git clone --depth 1 "$GUX_REPO" "$WORK"; then
		say "clone failed"
	fi
fi

if [ -d "$WORK/GenericUSBXHCI" ]; then
	(cd "$WORK" && git init -q . 2>/dev/null; git add -A >/dev/null 2>&1)
	APPLIED=0
	if (cd "$WORK" && git apply --check "$HERE/gux/GUX-Preflight.patch" 2>>"$LOG"); then
		(cd "$WORK" && git apply "$HERE/gux/GUX-Preflight.patch" 2>>"$LOG") && APPLIED=1
	elif (cd "$WORK" && git apply --check --reverse "$HERE/gux/GUX-Preflight.patch" 2>/dev/null); then
		say "patch already applied in source tree — continuing"
		APPLIED=1
	fi
	if [ $APPLIED -eq 1 ]; then
		say "patch applied cleanly"
		if xcodebuild -version >/dev/null 2>&1; then
			if (cd "$WORK" && xcodebuild -configuration "El Capitan" -arch x86_64 \
				CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO build >>"$LOG" 2>&1) ; then
				BUILT="$(find "$WORK" -name 'GenericUSBXHCI.kext' -maxdepth 6 -type d | head -1)"
				if [ -n "$BUILT" ]; then
					cp -R "$BUILT" "$OUT/"
					say "GUX kext: OK -> out/GenericUSBXHCI.kext"
				else
					say "GUX kext: build finished but no .kext found"
				fi
			else
				say "GUX kext: xcodebuild FAILED (tail below)"
				tail -30 "$LOG"
			fi
		else
			# No Xcode: gux/build_gux.sh drives clang++ directly and also does
			# the three things xcodebuild used to (plist template expansion, CRLF
			# strip, kmod_info/__realmain for a hand-linked kext).
			say "GUX kext: xcodebuild not found — using gux/build_gux.sh"
			if bash "$HERE/gux/build_gux.sh" "$WORK" "$OUT/gux" >>"$LOG" 2>&1; then
				cp -R "$OUT/gux/GenericUSBXHCI.kext" "$OUT/" && say "GUX kext: OK -> out/GenericUSBXHCI.kext"
			else
				say "GUX kext: FAIL — tail below"
				tail -25 "$LOG"
			fi
		fi
	else
		say "GUX kext: patch did NOT apply — upstream changed? see $LOG"
	fi
else
	say "GUX kext: source tree missing"
fi

# ---------------------------------------------------------------- verification
say ""
say "### artifacts"
for k in "$OUT"/*.kext; do
	[ -d "$k" ] || continue
	BIN="$k/Contents/MacOS/$(basename "$k" .kext)"
	say "--- $(basename "$k")"
	{ file "$BIN"; otool -hv "$BIN" | tail -2; plutil -lint "$k/Contents/Info.plist"; } 2>&1 | tee -a "$LOG"
done

say ""
say "done. report: $LOG"
