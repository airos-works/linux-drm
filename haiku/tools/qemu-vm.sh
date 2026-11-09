#!/bin/bash
# A Haiku x86_64 virtual machine for testing amdgpu_drm without a GPU
# (settings "haiku_selftest 1"). The serial console, where a kernel panic
# shows, is logged to $DIR/serial.log.
#
#   haiku/tools/qemu-vm.sh start|stop|ssh <command>|deploy
#
# For the NPU driver: ADDON=amdxdna (its binary is out-xdna/amdxdna, its
# device directory dev/npu). A machine of one's own next to someone else's:
# DIR, PORT and VNC (the display number). QEMU_EXTRA: more arguments for
# QEMU ("-device megasas-gen2" gives the NPU driver's stand-in for the
# hardware, "haiku_selftest 3", a PCI function with MSI-X to dress up).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BASE=${BASE:-/mnt/HaikuWork/x399/evidence/wx5100/qemu-isolated/disk.qcow2}
DIR=${DIR:-/mnt/HaikuWork/x399/build/amdgpu-drm-qemu}
PORT=${PORT:-2240}
VNC=${VNC:-40}
ADDON=${ADDON:-amdgpu_drm}
if [ "$ADDON" = amdxdna ]; then
	BIN=${BIN:-$ROOT/out-xdna/amdxdna}
	DEVDIR=dev/npu
else
	BIN=${BIN:-$ROOT/out/amdgpu_drm}
	DEVDIR=dev/dri
fi
KEY=/mnt/HaikuWork/apps/burrow/.vm/id_ed25519
SSH="ssh -p $PORT -i $KEY -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 user@127.0.0.1"
mkdir -p "$DIR"

case "${1:-}" in
	start)
		[ -f "$DIR/overlay.qcow2" ] || qemu-img create -q -f qcow2 -b "$BASE" -F qcow2 "$DIR/overlay.qcow2"
		qemu-system-x86_64 -enable-kvm -cpu host -m 4096 -smp 4 \
			-drive file="$DIR/overlay.qcow2",format=qcow2,if=ide \
			-netdev user,id=n,hostfwd=tcp:127.0.0.1:$PORT-:22 -device e1000,netdev=n \
			-vga std -display none -vnc 127.0.0.1:$VNC \
			-qmp unix:"$DIR/qmp.sock",server=on,wait=off \
			-serial file:"$DIR/serial.log" -pidfile "$DIR/qemu.pid" -daemonize \
			${QEMU_EXTRA:-}
		for i in $(seq 1 60); do
			$SSH true 2>/dev/null && { echo "up"; exit 0; }
			sleep 2
		done
		echo "no ssh" >&2; exit 1 ;;
	stop)
		[ -f "$DIR/qemu.pid" ] && kill "$(cat "$DIR/qemu.pid")" 2>/dev/null || true ;;
	ssh)
		shift; $SSH "$@" ;;
	deploy)
		NP=/boot/home/config/non-packaged/add-ons/kernel/drivers
		if [ "$ADDON" = amdxdna ]; then
			# devfs loads a driver when its binary appears in bin/ (it does
			# not watch dev/npu, and /dev/npu itself only exists from the
			# next boot on): staged outside, removed, moved in
			$SSH "mkdir -p $NP/bin $NP/$DEVDIR && cat > /boot/home/$ADDON.staged && chmod 755 /boot/home/$ADDON.staged && ln -sf ../../bin/$ADDON $NP/$DEVDIR/$ADDON && rm -f $NP/bin/$ADDON && sync && sleep 3 && mv -f /boot/home/$ADDON.staged $NP/bin/$ADDON && sync" < "$BIN"
			exit 0
		fi
		$SSH "mkdir -p $NP/bin $NP/$DEVDIR && cat > $NP/bin/$ADDON.tmp && chmod 755 $NP/bin/$ADDON.tmp && mv -f $NP/bin/$ADDON.tmp $NP/bin/$ADDON && ln -sf ../../bin/$ADDON $NP/$DEVDIR/$ADDON && sync" < "$BIN" ;;
	selftest)
		# deploy, start once with "haiku_selftest 1" (plus any parameters
		# given), then show the self test or the kernel debugger's report
		shift
		offset=$(stat -c %s "$DIR/serial.log")
		S=/boot/home/config/settings/kernel/drivers
		NP=/boot/home/config/non-packaged/add-ons/kernel/drivers
		params="haiku_selftest 1"
		for p in "$@"; do params="$params
$p"; done
		if [ "$ADDON" = amdxdna ]; then
			# the settings first: the driver starts as it is deployed
			$SSH "mkdir -p $S && cat > $S/$ADDON && sync" <<< "$params"
			mark=$($SSH "wc -l < /var/log/syslog")
			"$0" deploy
			for i in $(seq 1 40); do
				sleep 2
				if tail -c +$((offset + 1)) "$DIR/serial.log" | grep -aq "Kernel Debugging Land"; then
					tail -c +$((offset + 1)) "$DIR/serial.log" | strings | grep -a -A40 "PANIC\|Welcome to Kernel"
					exit 1
				fi
				if $SSH "tail -n +$((mark + 1)) /var/log/syslog | grep -aq 'self test: stopped'" 2>/dev/null; then
					$SSH "tail -n +$((mark + 1)) /var/log/syslog | grep -a -E 'amdxdna|npu'"
					exit 0
				fi
			done
			echo "no result" >&2; exit 1
		fi
		"$0" deploy
		$SSH "mkdir -p $S && cat > $S/$ADDON && touch $S/$ADDON.arm && rm -f $NP/$DEVDIR/$ADDON && sync && sleep 2 && ln -s ../../bin/$ADDON $NP/$DEVDIR/$ADDON && sync" <<< "$params"
		for i in $(seq 1 40); do
			sleep 2
			if tail -c +$((offset + 1)) "$DIR/serial.log" | grep -aq "Kernel Debugging Land"; then
				tail -c +$((offset + 1)) "$DIR/serial.log" | strings | grep -a -A40 "PANIC\|Welcome to Kernel"
				exit 1
			fi
			if $SSH "grep -aq 'self test: stopped' /var/log/syslog" 2>/dev/null; then
				$SSH "grep -a -E '${ADDON%_drm}' /var/log/syslog | tail -60"
				exit 0
			fi
		done
		echo "no result" >&2; exit 1 ;;
	*)
		echo "usage: $0 start|stop|ssh <command>|deploy|selftest [param...]" >&2; exit 1 ;;
esac
