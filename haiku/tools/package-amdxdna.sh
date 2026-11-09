#!/bin/bash
# Builds Haiku packages of amdxdna, the driver for the NPU of AMD's Ryzen AI
# processors (pkgman install ./<file>...):
#
#   amdnpu_firmwares-<date>-1-any.hpkg     the amdnpu firmware files of a
#       linux-firmware tree, in data/firmware/amdnpu/<device>_<revision>/,
#       under the names the driver asks for (npu_7.sbin, then npu.sbin;
#       linux-firmware has them as links to versioned files)
#   amdxdna-<version>-1-x86_64.hpkg        the driver (out-xdna/amdxdna) as
#       bin/amdxdna, linked from dev/npu, and data/launch/amdxdna, the
#       job that has it loaded at boot; requires the firmware and the haiku
#       package it was built against
#
#   haiku/tools/package-amdxdna.sh [OUTPUT_DIR [LINUX_FIRMWARE_DIR [LICENSE]]]
#
# Defaults: out-packages, /lib/firmware (the host's linux-firmware, files
# compressed or not) and its LICENSE.amdnpu. LINUX_FIRMWARE_DIR may also be
# a checkout of linux-firmware.git (its WHENCE then names the links, and
# LICENSE.amdnpu is taken from it). FIRMWARE_VERSION=YYYYMMDD (and
# FIRMWARE_REVISION, default 1) name the firmware package for a tree other
# than the host's package; DRIVER_REVISION (default 1) is the driver
# package's revision. HAIKU_BUILD is the configured Haiku build directory
# (for the package tool and the haiku version). BIN is the driver to package.
#
# No firmware is kept in this repository: LICENSE.amdnpu allows passing the
# binaries on unchanged, with the licence, which the package carries as
# "AMD NPU Firmware".
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUTPUT=${1:-$ROOT/out-packages}
FIRMWARE=${2:-/lib/firmware}
if [ -n "${3:-}" ]; then
	LICENSE=$3
elif [ -f "$FIRMWARE/LICENSE.amdnpu" ]; then
	LICENSE=$FIRMWARE/LICENSE.amdnpu
else
	LICENSE=/usr/share/doc/linux-firmware/licenses/LICENSE.amdnpu.gz
fi
HAIKU_BUILD=${HAIKU_BUILD:-/mnt/HaikuWork/build/master-x86_64}
TOOL=$HAIKU_BUILD/objects/linux/x86_64/release/tools/package/package
export LD_LIBRARY_PATH=$HAIKU_BUILD/objects/linux/lib
BIN=${BIN:-$ROOT/out-xdna/amdxdna}

[ -x "$TOOL" ] || { echo "no package tool in $HAIKU_BUILD" >&2; exit 1; }
[ -f "$BIN" ] || { echo "no driver at $BIN (make -f haiku/build/Makefile amdxdna)" >&2; exit 1; }
[ -d "$FIRMWARE/amdnpu" ] || { echo "no amdnpu firmware in $FIRMWARE" >&2; exit 1; }
[ -f "$LICENSE" ] || { echo "no licence at $LICENSE" >&2; exit 1; }
mkdir -p "$OUTPUT"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

decompress() {
	case "$1" in
		*.zst) zstd -qdc -- "$1" ;;
		*.xz) xz -dc -- "$1" ;;
		*.gz) gzip -dc -- "$1" ;;
		*) cat -- "$1" ;;
	esac
}
strip() {
	local name=${1%.zst}
	name=${name%.xz}
	printf '%s' "${name%.gz}"
}

# --- firmware ----------------------------------------------------------------
if [ -n "${FIRMWARE_VERSION:-}" ]; then
	version=$FIRMWARE_VERSION
else
	version=$(dpkg-query -W -f '${Version}' linux-firmware 2>/dev/null \
		| sed -n 's/^\([0-9]\{8\}\).*/\1/p')
	version=${version:-$(date -u +%Y%m%d)}
fi
revision=${FIRMWARE_REVISION:-1}
fw=$STAGE/firmware
mkdir -p "$fw/data/firmware/amdnpu" "$fw/data/licenses"
count=0
list=
# an installed tree: the files and links as they are
while IFS= read -r file; do
	relative=${file#"$FIRMWARE"/amdnpu/}
	name=$(strip "$relative")
	mkdir -p "$fw/data/firmware/amdnpu/$(dirname "$name")"
	if [ -L "$file" ]; then
		ln -sf "$(strip "$(readlink "$file")")" "$fw/data/firmware/amdnpu/$name"
	else
		decompress "$file" < /dev/null > "$fw/data/firmware/amdnpu/$name"
		chmod 0444 "$fw/data/firmware/amdnpu/$name"
		count=$((count + 1))
		list="$list $name"
	fi
done < <(find "$FIRMWARE/amdnpu" \( -type f -o -type l \) | sort)
# a checkout of linux-firmware.git has the links in WHENCE only
if [ -f "$FIRMWARE/WHENCE" ]; then
	while read -r link target; do
		link=${link#amdnpu/}
		[ -e "$fw/data/firmware/amdnpu/$(dirname "$link")/$target" ] || continue
		ln -sf "$target" "$fw/data/firmware/amdnpu/$link"
	done < <(sed -n 's/^Link: \(amdnpu\/[^ ]*\) -> \(.*\)$/\1 \2/p' "$FIRMWARE/WHENCE")
fi
[ "$count" -gt 0 ] || { echo "no firmware files found" >&2; exit 1; }
decompress "$LICENSE" > "$fw/data/licenses/AMD NPU Firmware"
cat > "$fw/.PackageInfo" <<EOF
name			amdnpu_firmwares
version			$version-$revision
architecture	any
summary			"Firmware for the NPU of AMD Ryzen AI processors (amdxdna)"
description		"The amdnpu firmware of linux-firmware $version ($count files), \
for the amdxdna driver: the firmware the NPU of Ryzen AI processors (Phoenix, \
Hawk Point, Strix Point, Strix Halo, Krackan Point) runs. The binaries are \
distributed unchanged."
packager		"air/OS"
vendor			"Advanced Micro Devices, Inc."
copyrights		{ "Advanced Micro Devices, Inc." }
licenses		{ "AMD NPU Firmware" }
provides		{ amdnpu_firmwares = $version }
urls			{ "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git" }
EOF
firmwarePackage=$OUTPUT/amdnpu_firmwares-$version-$revision-any.hpkg
rm -f "$firmwarePackage"
(cd "$fw" && "$TOOL" create -q "$firmwarePackage")
echo "$firmwarePackage: $count files:$list"

# --- driver ------------------------------------------------------------------
haikuVersion=$("$TOOL" list -i \
	"$HAIKU_BUILD/objects/haiku/x86_64/packaging/packages/haiku.hpkg" \
	| sed -n 's/^\tversion: \(.*\)-[0-9]*$/\1/p' | head -1)
# the Linux release the driver's sources are from
linuxVersion=$(sed -n '1s/ .*//p' "$ROOT/linux/drivers/accel/amdxdna/VERSION")
commit=$(git -C "$ROOT" rev-parse --short HEAD)
stamp=$(git -C "$ROOT" log -1 --format=%cd --date=format:%Y%m%d)
# a Haiku version has one pre-release part: 7.4.0-rc2 -> 7.4.0~rc2_git<date>
case "$linuxVersion" in
	*-*) driverVersion="${linuxVersion%%-*}~${linuxVersion#*-}_git$stamp" ;;
	*) driverVersion="$linuxVersion~git$stamp" ;;
esac
linuxVersion=${linuxVersion%%-*}
drv=$STAGE/driver
mkdir -p "$drv/add-ons/kernel/drivers/bin" \
	"$drv/add-ons/kernel/drivers/dev/npu" \
	"$drv/documentation/packages/amdxdna"
cp "$BIN" "$drv/add-ons/kernel/drivers/bin/amdxdna"
chmod 755 "$drv/add-ons/kernel/drivers/bin/amdxdna"
# the dev/npu directory makes devfs offer /dev/npu; the driver is loaded
# when that is first looked into, and starts the NPU when its node is opened
ln -s ../../bin/amdxdna "$drv/add-ons/kernel/drivers/dev/npu/amdxdna"
# which a job does early in the boot: the driver takes its buffer memory
# when it is loaded, while there is a run of pages long enough
mkdir -p "$drv/data/launch"
cp "$ROOT/haiku/amdxdna/launch" "$drv/data/launch/amdxdna"
cat > "$drv/documentation/packages/amdxdna/README" <<EOF
amdxdna $driverVersion (linux-drm $commit): Linux's amdxdna driver for the
NPU of AMD Ryzen AI processors, with the DRM core, running on Haiku through
a Linux compatibility layer. Built against $haikuVersion.

Licence: GNU GPL v2 (the add-on combines GPL-2.0 and MIT sources). Its
complete source is https://github.com/airos-works/linux-drm at commit
$commit (COPYING there says which files are under which licence).

It publishes /dev/npu/amdxdna/0 with Linux's ioctls (amdxdna_accel.h). The
NPU is started when the node is opened and powered down when the last
client closes it. Only root can open the node: without an IOMMU driver the
NPU is given physical addresses, and a client could have it read and write
any memory.

Settings: one "name value" line each in
~/config/settings/kernel/drivers/amdxdna; "carveout_mb <MiB>" is the memory
for the NPU's buffers (128), which the driver takes when it is loaded: a
launch job (data/launch/amdxdna) has that happen early in the boot, while
such a run of pages is free. "carveout_at_load 0" leaves the memory alone
until the NPU is first started, when there may be none. The NPU is powered
down five seconds after its last client has closed the node
("autosuspend_ms <ms>"; "keep_powered 1": never). A start of the NPU
that stops the machine is not repeated:
~/config/settings/kernel/drivers/amdxdna.starting exists
while the NPU is started for the first time after a boot, and while it
exists the NPU is not started.
EOF
cat > "$drv/.PackageInfo" <<EOF
name			amdxdna
version			$driverVersion-${DRIVER_REVISION:-1}
architecture	x86_64
summary			"Linux's amdxdna driver for the NPU of AMD Ryzen AI processors"
description		"amdxdna runs Linux's driver for the neural processing unit of \
AMD Ryzen AI processors on Haiku, with Linux's ioctl interface at \
/dev/npu/amdxdna/0. The NPU is powered only while a client has the node open."
packager		"air/OS"
vendor			"air/OS"
copyrights		{ "Advanced Micro Devices, Inc." "The Linux kernel contributors" "air/OS" }
licenses		{ "MIT" "GNU GPL v2" }
provides		{ amdxdna = $linuxVersion }
source-urls		{ "https://github.com/airos-works/linux-drm" }
requires		{
	haiku >= $haikuVersion
	amdnpu_firmwares
}
EOF
driverPackage=$OUTPUT/amdxdna-$driverVersion-${DRIVER_REVISION:-1}-x86_64.hpkg
rm -f "$driverPackage"
(cd "$drv" && "$TOOL" create -q "$driverPackage")
echo "$driverPackage"
