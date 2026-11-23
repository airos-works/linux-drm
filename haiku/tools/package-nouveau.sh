#!/bin/bash
# Builds the Haiku package of nouveau_drm, Linux's nouveau driver for NVIDIA
# GPUs before Maxwell (pkgman install ./<file>):
#
#   nouveau_drm-<version>-1-x86_64.hpkg    the driver (a release build, out-
#       nouveau-release/nouveau_drm) as bin/nouveau_drm, linked from
#       dev/graphics; requires the haiku package it was built against
#
#   haiku/tools/package-nouveau.sh [OUTPUT_DIR]
#
# Default: out-packages. HAIKU_BUILD is the configured Haiku build directory
# (for the package tool and the haiku version). BIN is the driver to package
# (make -f haiku/build/Makefile nouveau OUT=out-nouveau-release, without
# LAB or ARMED_ONLY). DRIVER_REVISION (default 1) is the package's revision.
#
# No firmware: the graphics engines of Tesla, Fermi and Kepler GPUs run
# nouveau's own microcode, which is built into the driver. Video decoding
# also takes NVIDIA's video firmware, which may not be passed on: it is
# extracted from NVIDIA's Linux driver where it is used, into
# /boot/system/non-packaged/data/firmware/nouveau (see the README below).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUTPUT=${1:-$ROOT/out-packages}
HAIKU_BUILD=${HAIKU_BUILD:-/mnt/HaikuWork/build/master-x86_64}
TOOL=$HAIKU_BUILD/objects/linux/x86_64/release/tools/package/package
export LD_LIBRARY_PATH=$HAIKU_BUILD/objects/linux/lib
BIN=${BIN:-$ROOT/out-nouveau-release/nouveau_drm}

[ -x "$TOOL" ] || { echo "no package tool in $HAIKU_BUILD" >&2; exit 1; }
[ -f "$BIN" ] || { echo "no driver at $BIN (make -f haiku/build/Makefile nouveau)" >&2; exit 1; }
if ! grep -aq nouveau_drm.starting "$BIN" || grep -aq nouveau_drm.arm "$BIN"; then
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
	"$drv/documentation/packages/nouveau_drm"
cp "$BIN" "$drv/add-ons/kernel/drivers/bin/nouveau_drm"
chmod 755 "$drv/add-ons/kernel/drivers/bin/nouveau_drm"
# One link, as amdgpu_drm's (package.sh): the driver publishes dri/card<n>
# and dri/renderD<n> itself; app_server's search of /dev/graphics loads it.
ln -s ../../bin/nouveau_drm "$drv/add-ons/kernel/drivers/dev/graphics/nouveau_drm"
cat > "$drv/documentation/packages/nouveau_drm/README" <<EOF
nouveau_drm $driverVersion (linux-drm $commit): Linux $linuxVersion's nouveau
driver with the DRM core and TTM, running on Haiku through a Linux
compatibility layer.

Licence: MIT (nouveau) and GNU GPL v2 (the add-on combines GPL-2.0 and MIT
sources). Its complete source is https://github.com/airos-works/linux-drm at
commit $commit (COPYING there says which files are under which licence).

It drives NVIDIA GPUs before Maxwell (PCI device IDs below 0x1340: Tesla,
Fermi and Kepler; nvidia_rm drives the newer ones), except those Haiku's own
nvidia driver lists (TNT to GeForce 7, a few G98s). It publishes
/dev/dri/card<n> and renderD<n> (the DRM interface libdrm, Mesa's nouveau
OpenGL driver, NVK and libva use) and /dev/graphics/nouveau_drm_<n> for
app_server, which drives it through the amdgpu_kms accelerant, also with
GPUs of both kinds in one desktop.

Video decoding (VA-API, H.264 and older codecs on the VP4 and VP5 video
engines) takes NVIDIA's video firmware, which may not be passed on and is
not in this package. Extract it from NVIDIA's Linux driver 340.108 with envytools'
extract_firmware.py and put the files (nv<chipset>_fuc084, _fuc085, _fuc086;
on GT21x also vuc-*) into /boot/system/non-packaged/data/firmware/nouveau.

Module parameters: one "name value" line each in
~/config/settings/kernel/drivers/nouveau_drm (Linux nouveau's parameters;
"haiku_gpus <bus:device.function>..." limits the driver to those GPUs).
A start that brings the machine down is not repeated:
~/config/settings/kernel/drivers/nouveau_drm.starting lives until the GPU
has run for 30 seconds, and after two starts that did not get there the GPU
is left to the framebuffer driver until it is deleted.
EOF
cat > "$drv/.PackageInfo" <<EOF
name			nouveau_drm
version			$driverVersion-${DRIVER_REVISION:-1}
architecture	x86_64
summary			"Linux's nouveau driver for NVIDIA GPUs before Maxwell"
description		"nouveau_drm runs Linux $linuxVersion's nouveau, DRM and TTM on \
Haiku: kernel mode setting for app_server (amdgpu_kms), and the DRM interface \
for OpenGL (Mesa's nouveau), Vulkan (NVK, Kepler) and video decoding \
(VA-API). NVIDIA Tesla, Fermi and Kepler GPUs (GeForce 8 to GTX 700)."
packager		"air/OS"
vendor			"air/OS"
copyrights		{ "Red Hat Inc." "The Linux kernel contributors" "air/OS" }
licenses		{ "MIT" "GNU GPL v2" }
provides		{ nouveau_drm = $linuxVersion }
source-urls		{ "https://github.com/airos-works/linux-drm" }
requires		{
	haiku >= $haikuVersion
}
EOF
driverPackage=$OUTPUT/nouveau_drm-$driverVersion-${DRIVER_REVISION:-1}-x86_64.hpkg
rm -f "$driverPackage"
(cd "$drv" && "$TOOL" create -q "$driverPackage")
echo "$driverPackage (requires haiku >= $haikuVersion)"
