#!/usr/bin/env bash
# Build and run the host unit test with every compiler / standard available.
set -u
cd "$(dirname "$0")"

fail=0
for cc in g++ clang++; do
	for std in c++98 c++11 c++17; do
		command -v "$cc" >/dev/null 2>&1 || continue
		bin="/tmp/amdxhci_${cc}_${std}"
		printf '%-8s %-7s ' "$cc" "$std"
		if "$cc" -std=$std -Wall -Wextra -Wshadow -O2 -o "$bin" test_amdxhci.cpp 2>/tmp/amdxhci_err.txt; then
			if out=$("$bin" 2>&1); then
				echo "$out" | tail -1
			else
				echo "TEST FAIL"
				echo "$out" | grep FAIL | head -20
				fail=1
			fi
		else
			echo "BUILD FAIL"
			head -20 /tmp/amdxhci_err.txt
			fail=1
		fi
	done
done

if command -v clang++ >/dev/null 2>&1; then
	printf '%-16s ' "ubsan+asan"
	if clang++ -std=c++11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o /tmp/amdxhci_san test_amdxhci.cpp 2>/tmp/amdxhci_err.txt; then
		if out=$(/tmp/amdxhci_san 2>&1); then
			echo "$out" | tail -1
		else
			echo "TEST FAIL"
			echo "$out" | tail -30
			fail=1
		fi
	else
		echo "BUILD FAIL"
		head -20 /tmp/amdxhci_err.txt
		fail=1
	fi
fi

exit $fail
