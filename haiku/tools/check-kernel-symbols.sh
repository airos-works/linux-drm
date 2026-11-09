#!/bin/bash
# Checks an add-on's imports against a kernel: every undefined symbol of the
# add-on must be exported by the kernel, with the same type (Haiku's loader
# refuses a function where the kernel has an object, and the other way
# round). Weak imports the kernel lacks are listed, not counted.
#   haiku/tools/check-kernel-symbols.sh <add-on> <kernel_x86_64>
# The kernel of a machine: /boot/system/kernel_x86_64 there.
set -euo pipefail
ADDON=$1
KERNEL=$2
READELF=${READELF:-readelf}
$READELF --dyn-syms -W "$KERNEL" | awk '$7 != "UND" && $8 != "" { sub(/@.*/, "", $8); print $8, $4 }' \
	| sort -u > /tmp/kernel-symbols.$$
missing=0
while read -r name type bind; do
	have=$(awk -v n="$name" '$1 == n { print $2; exit }' /tmp/kernel-symbols.$$)
	if [ -z "$have" ]; then
		if [ "$bind" = WEAK ]; then
			echo "weak, not in this kernel: $name"
		else
			echo "MISSING: $name"; missing=$((missing + 1))
		fi
	elif [ "$type" != NOTYPE ] && [ "$have" != "$type" ]; then
		echo "TYPE MISMATCH: $name (add-on $type, kernel $have)"; missing=$((missing + 1))
	elif [ "$type" = NOTYPE ]; then
		echo "UNTYPED import: $name (kernel $have)"; missing=$((missing + 1))
	fi
done < <($READELF --dyn-syms -W "$ADDON" | awk '$7 == "UND" && $8 != "" { sub(/@.*/, "", $8); print $8, $4, $5 }')
rm -f /tmp/kernel-symbols.$$
echo "$(basename "$ADDON") against $(basename "$KERNEL"): $missing problem(s)"
[ $missing -eq 0 ]
