#!/bin/bash
# Builds the Haiku packages of i915_drm, Linux's i915 driver for Intel GPUs
# from Gen 9 on (pkgman install ./<file>):
#
#   i915_drm-<version>-1-x86_64.hpkg    the driver (a release build, out-
#       i915-release/i915_drm) as bin/i915_drm, linked from dev/graphics;
#       requires the haiku package it was built against and the firmware
#   i915_firmwares-<date>-<rev>-any.hpkg    the GuC, HuC, DMC and GSC
#       firmware of linux-firmware's i915 directory (FIRMWARE_DIR/i915, e.g.
#       airos-works/airos-firmware), with its licence
#
#   haiku/tools/package-i915.sh [OUTPUT_DIR]
#
# Default: out-packages. HAIKU_BUILD is the configured Haiku build directory
# (for the package tool and the haiku version). BIN is the driver to package
# (make -f haiku/build/Makefile i915 OUT=out-i915-release, without
# ARMED_ONLY). DRIVER_REVISION (default 1) is the package's revision;
# FIRMWARE_VERSION (the linux-firmware date) and FIRMWARE_REVISION name the
# firmware package, which is not made without FIRMWARE_DIR.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUTPUT=${1:-$ROOT/out-packages}
HAIKU_BUILD=${HAIKU_BUILD:-/mnt/HaikuWork/build/master-x86_64}
TOOL=$HAIKU_BUILD/objects/linux/x86_64/release/tools/package/package
export LD_LIBRARY_PATH=$HAIKU_BUILD/objects/linux/lib
BIN=${BIN:-$ROOT/out-i915-release/i915_drm}

[ -x "$TOOL" ] || { echo "no package tool in $HAIKU_BUILD" >&2; exit 1; }
[ -f "$BIN" ] || { echo "no driver at $BIN (make -f haiku/build/Makefile i915)" >&2; exit 1; }
if ! grep -aq i915_drm.starting "$BIN" || grep -aq i915_drm.arm "$BIN"; then
	echo "$BIN is not a release build" >&2
	exit 1
fi
mkdir -p "$OUTPUT"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

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
	"$drv/documentation/packages/i915_drm"
cp "$BIN" "$drv/add-ons/kernel/drivers/bin/i915_drm"
chmod 755 "$drv/add-ons/kernel/drivers/bin/i915_drm"
# One link, as amdgpu_drm's (package.sh): the driver publishes dri/card<n>
# and dri/renderD<n> itself; app_server's search of /dev/graphics loads it.
ln -s ../../bin/i915_drm "$drv/add-ons/kernel/drivers/dev/graphics/i915_drm"
cat > "$drv/documentation/packages/i915_drm/README" <<EOF
i915_drm $driverVersion (linux-drm $commit): Linux $linuxVersion's i915
driver with the DRM core, running on Haiku through a Linux compatibility
layer. Built against $haikuVersion.

Licence: MIT (i915) and GNU GPL v2 (the add-on combines GPL-2.0 and MIT
sources). Its complete source is https://github.com/airos-works/linux-drm at
commit $commit (COPYING there says which files are under which licence).

It drives the Intel GPUs of Gen 9's big cores (Skylake, Kaby Lake, Amber
Lake, Coffee Lake, Whiskey Lake, Comet Lake) and Gemini Lake; with
"haiku_probe all" in its settings every Gen 9 and later GPU i915 knows (not
tried on hardware yet).
Haiku's intel_extreme leaves those GPUs alone while this driver is installed
and drives the older ones. It publishes /dev/dri/card<n> and renderD<n> (the
DRM interface of libdrm, Mesa's iris OpenGL driver, ANV Vulkan and Intel's
VA-API driver) and /dev/graphics/i915_drm_<n> for app_server, which drives
it through the amdgpu_kms accelerant (kernel mode setting, a cursor plane,
copies by the blitter).

The GuC, HuC and display (DMC) firmware is in the i915_firmwares package.

Module parameters: one "name value" line each in
~/config/settings/kernel/drivers/i915_drm (Linux i915's parameters;
"haiku_gpus <bus:device.function>..." limits the driver to those GPUs).
A start that brings the machine down is not repeated:
~/config/settings/kernel/drivers/i915_drm.starting lives until the GPU has
run for 30 seconds, and after two starts that did not get there the GPU is
left to intel_extreme until it is deleted.
EOF
cat > "$drv/.PackageInfo" <<EOF
name			i915_drm
version			$driverVersion-${DRIVER_REVISION:-1}
architecture	x86_64
summary			"Linux's i915 driver for Intel GPUs from Gen 9 on"
description		"i915_drm runs Linux $linuxVersion's i915 and DRM on Haiku: \
kernel mode setting for app_server (amdgpu_kms), and the DRM interface for \
OpenGL (Mesa's iris), Vulkan (ANV) and video decoding and encoding \
(VA-API). Intel Skylake to Comet Lake and Gemini Lake integrated graphics."
packager		"air/OS"
vendor			"air/OS"
copyrights		{ "Intel Corporation" "The Linux kernel contributors" "air/OS" }
licenses		{ "MIT" "GNU GPL v2" }
provides		{ i915_drm = $linuxVersion }
source-urls		{ "https://github.com/airos-works/linux-drm" }
requires		{
	haiku >= $haikuVersion
	i915_firmwares
}
EOF
driverPackage=$OUTPUT/i915_drm-$driverVersion-${DRIVER_REVISION:-1}-x86_64.hpkg
rm -f "$driverPackage"
(cd "$drv" && "$TOOL" create -q "$driverPackage")
echo "$driverPackage (requires haiku >= $haikuVersion)"

# the firmware: linux-firmware's i915 directory
if [ -n "${FIRMWARE_DIR:-}" ]; then
	[ -d "$FIRMWARE_DIR/i915" ] || { echo "no i915 firmware in $FIRMWARE_DIR" >&2; exit 1; }
	fw=$STAGE/firmware
	mkdir -p "$fw/data/firmware/i915" "$fw/data/licenses"
	cp "$FIRMWARE_DIR"/i915/*.bin "$fw/data/firmware/i915/"
	licence=$(ls "$FIRMWARE_DIR"/LICENSE.i915 "$FIRMWARE_DIR"/LICENSES/LICENSE.i915 \
		2> /dev/null | head -1 || true)
	[ -n "$licence" ] || { echo "no LICENSE.i915 in $FIRMWARE_DIR" >&2; exit 1; }
	cp "$licence" "$fw/data/licenses/i915 firmware"
	firmwareVersion=${FIRMWARE_VERSION:-$(date +%Y%m%d)}
	cat > "$fw/.PackageInfo" <<EOF
name			i915_firmwares
version			$firmwareVersion-${FIRMWARE_REVISION:-1}
architecture	any
summary			"Firmware for Intel GPUs (i915_drm)"
description		"The GuC, HuC, DMC (display) and GSC firmware of \
linux-firmware's i915 directory, which i915_drm loads into Intel GPUs."
packager		"air/OS"
vendor			"Intel Corporation"
copyrights		{ "Intel Corporation" }
licenses		{ "i915 firmware" }
provides		{ i915_firmwares = $firmwareVersion }
EOF
	firmwarePackage=$OUTPUT/i915_firmwares-$firmwareVersion-${FIRMWARE_REVISION:-1}-any.hpkg
	rm -f "$firmwarePackage"
	(cd "$fw" && "$TOOL" create -q "$firmwarePackage")
	echo "$firmwarePackage ($(ls "$fw/data/firmware/i915" | wc -l) files)"
fi
