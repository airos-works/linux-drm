#!/bin/bash
# Copy Linux's AMD NPU driver (drivers/accel/amdxdna) into linux/.
#   tools/import-amdxdna.sh <linux source tree>
#   tools/import-amdxdna.sh --fetch <tag> <directory>
# The driver comes from a newer Linux than the DRM core in linux/ (see
# linux/drivers/accel/amdxdna/VERSION and the README): the release the core
# is from has no way to run the NPU without an IOMMU. With --fetch, the files
# are downloaded from kernel.org's GitHub mirror at <tag> into <directory>
# first (a whole tree is not needed).
# Like import-linux.sh, only files are added: the Haiku changes are separate
# commits on top. The one exception is the driver's UAPI header, which the
# DRM import brought in its older version and which is replaced.
set -euo pipefail

FILES_FIXED="include/uapi/drm/amdxdna_accel.h include/trace/events/amdxdna.h
include/linux/amd-pmf-io.h Documentation/accel/amdxdna/amdnpu.rst Makefile"

if [ "${1:-}" = "--fetch" ]; then
	TAG=$2
	SRC=$3
	BASE=https://raw.githubusercontent.com/torvalds/linux/$TAG
	mkdir -p "$SRC/drivers/accel/amdxdna"
	for name in $(curl -sfL \
			"https://api.github.com/repos/torvalds/linux/contents/drivers/accel/amdxdna?ref=$TAG" \
			| sed -n 's/^ *"name": "\(.*\)",$/\1/p'); do
		curl -sfL "$BASE/drivers/accel/amdxdna/$name" \
			-o "$SRC/drivers/accel/amdxdna/$name"
	done
	for file in $FILES_FIXED; do
		mkdir -p "$SRC/$(dirname "$file")"
		curl -sfL "$BASE/$file" -o "$SRC/$file"
	done
	curl -sfL "https://api.github.com/repos/torvalds/linux/commits/$TAG" \
		| sed -n '0,/^  "sha": "\(.*\)",$/s//\1/p' > "$SRC/.commit"
else
	SRC=$1
fi

SRC=$(realpath "$SRC")
DST=$(dirname "$(realpath "$0")")/../linux
cd "$SRC"
{
	find drivers/accel/amdxdna -maxdepth 1 -type f \( -name '*.[ch]' \
		-o -name Makefile -o -name Kconfig -o -name TODO \)
	ls include/uapi/drm/amdxdna_accel.h include/trace/events/amdxdna.h \
		include/linux/amd-pmf-io.h Documentation/accel/amdxdna/amdnpu.rst
} | sort -u > "$DST/.files-amdxdna"
tar -cf - -T "$DST/.files-amdxdna" | tar -xf - --skip-old-files -C "$DST"
cp include/uapi/drm/amdxdna_accel.h "$DST/include/uapi/drm/amdxdna_accel.h"

VERSION=$(make -s kernelversion 2>/dev/null \
	|| sed -n 's/^VERSION = //p;s/^PATCHLEVEL = //p;s/^SUBLEVEL = //p;s/^EXTRAVERSION = //p' Makefile \
		| paste -sd. | sed 's/\.\(-.*\)$/\1/;s/\.$//')
COMMIT=$(git rev-parse HEAD 2>/dev/null || cat .commit 2>/dev/null || true)
echo "$VERSION${COMMIT:+ ($COMMIT)}" > "$DST/drivers/accel/amdxdna/VERSION"
echo "imported $(wc -l < "$DST/.files-amdxdna") files from Linux $(cat "$DST/drivers/accel/amdxdna/VERSION")"
