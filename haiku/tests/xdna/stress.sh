#!/bin/sh
# Sessions of xdna_test one after the other, in several processes at once:
# each opens the node, makes a context, runs a design's command a few times
# and closes again.
#
#   stress.sh <processes> <sessions per process> <runs per session> [design dir]
#
# Meant for the driver's stand-in for the hardware in a virtual machine
# ("haiku_selftest 3" in the settings, QEMU with "-device megasas-gen2"; the
# outputs are not checked, -x), next to xdna_test and the designs. What the
# sessions do to the NPU's power is the driver's "autosuspend_ms"
# (xdna_test set autosuspend_ms <ms>): 0 powers it down and up between
# sessions that do not overlap, a few milliseconds have power downs meet
# the next open, 5000 (the default) never powers down while sessions follow
# one another. More than one process at a time needs buffer memory for as
# many clients (carveout_mb 384 for four). The log's "counts at ..." lines
# and the kernel debugger are where trouble shows; a session that fails is
# printed here.
cd "$(dirname "$0")" || exit 1
design=${4:-designs/passthrough_kernel}

loop() {
	i=0
	while [ $i -lt "$2" ]; do
		./xdna_test -x -n "$3" run "$design" > /tmp/xdna.$1.out 2>&1 \
			|| { echo "process $1, session $i:"; tail -n 3 /tmp/xdna.$1.out; }
		i=$((i + 1))
	done
}

p=0
while [ $p -lt "$1" ]; do
	loop $p "$2" "$3" &
	p=$((p + 1))
done
wait
echo "done: $1 x $2 sessions of $3 runs"
