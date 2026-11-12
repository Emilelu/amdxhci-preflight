#!/bin/bash
#
# Type-check AMDXHCIPreflight.cpp without a Mac.
#
#   ./check.sh
#
# Uses the stub headers next to this script instead of the real kernel SDK,
# so it proves the file parses, names resolve and types line up - it does not
# prove the kext links or loads. Useful as a pre-flight before handing the
# source to a machine that has Xcode.
#
set -eu
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
CORE=../../core
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

for std in c++11 c++14 c++17; do
	printf '%-10s ' "$std"
	$CXX -std=$std -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable \
		-I stub -I "$CORE" \
		-c ../AMDXHCIPreflight.cpp -o "$OUT/AMDXHCIPreflight.o"
	echo PASS
done
