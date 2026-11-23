#!/bin/bash
# Builds Haiku packages of amdgpu_drm, for installing it on any air/OS
# machine with an AMD GPU (pkgman install ./<file>...):
#
#   amdgpu_firmwares-<date>-1-any.hpkg     every amdgpu firmware file of a
#       linux-firmware tree, in data/firmware/amdgpu (what amdgpu_drm loads
#       for Southern Islands to RDNA 4, APUs included); replaces the WX 5100
#       trials' amdgpu_polaris10_firmware
#   amdgpu_drm-<version>-1-x86_64.hpkg     the driver (a release build, out-
#       release/amdgpu_drm) as bin/amdgpu_drm, linked from dev/graphics only
#       (see below); requires the firmware and the haiku package it was built
#       against. On an image that still has the native amdgpu driver, its
#       post-install script blocks that driver and its amduvd decoder
#       (/boot/system/settings/packages): two drivers must never start the
#       same GPU.
#
# The accelerant (amdgpu_kms) comes with haiku.hpkg; the GPU user land
# (libdrm, libva, Mesa) is built on the machine (gpu-userland-build.sh).
#
#   haiku/tools/package.sh [OUTPUT_DIR [LINUX_FIRMWARE_DIR [LICENSE]]]
#
# Defaults: out-packages, /lib/firmware (the host's linux-firmware, files
# compressed or not) and its LICENSE.amdgpu. FIRMWARE_VERSION=YYYYMMDD (and
# FIRMWARE_REVISION, default 1) name the firmware package for a tree other
# than the host's linux-firmware package. DRIVER_REVISION (default 1) is the
# driver package's revision, for a rebuild of the same commit date that has
# to replace an installed package. HAIKU_BUILD is the configured
# Haiku build directory the driver was linked in (for the package tool and
# the haiku version). BIN is the driver to package (default out-release/
# amdgpu_drm).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUTPUT=${1:-$ROOT/out-packages}
FIRMWARE=${2:-/lib/firmware}
LICENSE=${3:-/usr/share/doc/linux-firmware/licenses/LICENSE.amdgpu.gz}
HAIKU_BUILD=${HAIKU_BUILD:-/mnt/HaikuWork/build/master-x86_64}
TOOL=$HAIKU_BUILD/objects/linux/x86_64/release/tools/package/package
export LD_LIBRARY_PATH=$HAIKU_BUILD/objects/linux/lib
BIN=${BIN:-$ROOT/out-release/amdgpu_drm}

[ -x "$TOOL" ] || { echo "no package tool in $HAIKU_BUILD" >&2; exit 1; }
if ! grep -aq amdgpu_drm.starting "$BIN" || grep -aq amdgpu_drm.arm "$BIN"; then
	echo "$BIN is not a release build" >&2
	exit 1
fi
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

# --- firmware ----------------------------------------------------------------
# FIRMWARE_VERSION (the linux-firmware release, YYYYMMDD) for a tree that is
# not the host's package; FIRMWARE_REVISION is the package's revision
if [ -n "${FIRMWARE_VERSION:-}" ]; then
	version=$FIRMWARE_VERSION
else
	version=$(dpkg-query -W -f '${Version}' linux-firmware 2>/dev/null \
		| sed -n 's/^\([0-9]\{8\}\).*/\1/p')
	version=${version:-$(date -u +%Y%m%d)}
fi
revision=${FIRMWARE_REVISION:-1}
fw=$STAGE/firmware
mkdir -p "$fw/data/firmware/amdgpu" "$fw/data/licenses"
count=0
strip() {
	local name=${1%.zst}
	name=${name%.xz}
	printf '%s' "${name%.gz}"
}
for file in "$FIRMWARE"/amdgpu/*; do
	name=$(strip "$(basename "$file")")
	if [ -L "$file" ]; then
		# linux-firmware links images that several chips share
		ln -s "$(strip "$(readlink "$file")")" "$fw/data/firmware/amdgpu/$name"
	else
		decompress "$file" < /dev/null > "$fw/data/firmware/amdgpu/$name"
		chmod 0444 "$fw/data/firmware/amdgpu/$name"
	fi
	count=$((count + 1))
done
decompress "$LICENSE" > "$fw/data/licenses/AMD GPU Firmware"
cat > "$fw/.PackageInfo" <<EOF
name			amdgpu_firmwares
version			$version-$revision
architecture	any
summary			"Firmware for AMD Radeon GPUs (amdgpu)"
description		"The amdgpu firmware of linux-firmware $version ($count files), \
for the amdgpu_drm driver: graphics, compute, SDMA, power management, display \
and video engines of AMD GPUs and APUs from Southern Islands to RDNA 4. The \
binaries are distributed unchanged."
packager		"air/OS"
vendor			"Advanced Micro Devices, Inc."
copyrights		{ "Advanced Micro Devices, Inc." }
licenses		{ "AMD GPU Firmware" }
provides		{ amdgpu_firmwares = $version }
replaces		{ amdgpu_polaris10_firmware }
conflicts		{ amdgpu_polaris10_firmware }
urls			{ "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git" }
EOF
firmwarePackage=$OUTPUT/amdgpu_firmwares-$version-$revision-any.hpkg
rm -f "$firmwarePackage"
(cd "$fw" && "$TOOL" create -q "$firmwarePackage")
echo "$firmwarePackage: $count files"

# --- driver ------------------------------------------------------------------
haikuVersion=$("$TOOL" list -i \
	"$HAIKU_BUILD/objects/haiku/x86_64/packaging/packages/haiku.hpkg" \
	| sed -n 's/^\tversion: \(.*\)-[0-9]*$/\1/p' | head -1)
# the Linux release the branch imports (airos-6.18.52)
linuxVersion=$(git -C "$ROOT" rev-parse --abbrev-ref HEAD | sed -n 's/^airos-//p')
linuxVersion=${linuxVersion:-6.18.52}
commit=$(git -C "$ROOT" rev-parse --short HEAD)
stamp=$(git -C "$ROOT" log -1 --format=%cd --date=format:%Y%m%d)
driverVersion="$linuxVersion~git$stamp"
drv=$STAGE/driver
mkdir -p "$drv/add-ons/kernel/drivers/bin" \
	"$drv/add-ons/kernel/drivers/dev/graphics" \
	"$drv/boot/post-install" "$drv/documentation/packages/amdgpu_drm"
cp "$BIN" "$drv/add-ons/kernel/drivers/bin/amdgpu_drm"
chmod 755 "$drv/add-ons/kernel/drivers/bin/amdgpu_drm"
# One link: devfs takes the same binary found in a second directory for a
# changed one, and reloads the running GPU's driver when it scans that
# directory (AMDGPU-DRM.md). The driver publishes dri/card<n> and
# dri/renderD<n> itself; app_server's search of /dev/graphics loads it.
ln -s ../../bin/amdgpu_drm "$drv/add-ons/kernel/drivers/dev/graphics/amdgpu_drm"
cat > "$drv/boot/post-install/amdgpu_drm-block-native.sh" <<'EOF'
#!/bin/sh
# amdgpu_drm drives AMD GPUs: an image that still has the native amdgpu
# driver (WX 5100, air/OS images before its removal) must not start it, nor
# its amduvd decoder. Images without it need nothing here; there amduvd may
# be amdgpu_va, which must not be blocked. Takes effect at the next boot.
settings=/boot/system/settings/packages
[ -e /boot/system/add-ons/kernel/drivers/bin/amdgpu ] || exit 0
grep -qs amdgpu_drm "$settings" && exit 0
cat >> "$settings" <<'BLOCK'
# amdgpu_drm drives the GPU here: the native amdgpu driver and its decoder
# must not start (amdgpu_drm package).
Package haiku {
	BlockedEntries {
		add-ons/kernel/drivers/dev/graphics/amdgpu
		add-ons/kernel/drivers/bin/amdgpu
	}
}
Package haiku_datatranslators {
	BlockedEntries {
		add-ons/media/plugins/amduvd
	}
}
BLOCK
EOF
chmod 755 "$drv/boot/post-install/amdgpu_drm-block-native.sh"
cat > "$drv/documentation/packages/amdgpu_drm/README" <<EOF
amdgpu_drm $driverVersion (linux-drm $commit): Linux $linuxVersion's amdgpu
driver with the DRM core, TTM, the GPU scheduler and Display Core, running
on Haiku through a Linux compatibility layer.

Licence: GNU GPL v2 (the add-on combines GPL-2.0 and MIT sources). Its
complete source is https://github.com/jmgasper/linux-drm at commit $commit
(COPYING there says which files are under which licence).

It publishes /dev/dri/card<n> and renderD<n> (the DRM interface libdrm,
Mesa's radeonsi and RADV, and libva use) and /dev/graphics/amdgpu_drm_<n>
for app_server, which drives it through the amdgpu_kms accelerant.

Module parameters: one "name value" line each in
~/config/settings/kernel/drivers/amdgpu_drm (Linux amdgpu's parameters).
A start that brings the machine down is not repeated: the marker
~/config/settings/kernel/drivers/amdgpu_drm.starting lives until the GPU
has run for 30 seconds, and a boot that finds it leaves the GPU to the
framebuffer driver until it is deleted.
EOF
cat > "$drv/.PackageInfo" <<EOF
name			amdgpu_drm
version			$driverVersion-${DRIVER_REVISION:-1}
architecture	x86_64
summary			"Linux's amdgpu driver for AMD Radeon GPUs"
description		"amdgpu_drm runs Linux $linuxVersion's amdgpu, DRM, TTM and Display \
Core on Haiku: kernel mode setting for app_server (amdgpu_kms), and the DRM \
interface for OpenGL (radeonsi), Vulkan (RADV) and video decoding and \
encoding (VA-API). AMD GPUs and APUs from Southern Islands to RDNA 4."
packager		"air/OS"
vendor			"air/OS"
copyrights		{ "Advanced Micro Devices, Inc." "The Linux kernel contributors" "air/OS" }
licenses		{ "MIT" "GNU GPL v2" }
provides		{ amdgpu_drm = $linuxVersion }
source-urls		{ "https://github.com/jmgasper/linux-drm" }
requires		{
	haiku >= $haikuVersion
	amdgpu_firmwares
}
EOF
driverPackage=$OUTPUT/amdgpu_drm-$driverVersion-${DRIVER_REVISION:-1}-x86_64.hpkg
rm -f "$driverPackage"
(cd "$drv" && "$TOOL" create -q "$driverPackage")
echo "$driverPackage (requires haiku >= $haikuVersion)"
ls -la "$OUTPUT"
