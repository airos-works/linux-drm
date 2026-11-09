#!/bin/bash
# Copy the part of a Linux source tree this port builds into linux/.
#   tools/import-linux.sh <linux source tree>
# Only files are added or replaced; the Haiku changes are separate commits on
# top, so re-importing a newer stable release shows them as a diff.
set -euo pipefail
SRC=$(realpath "$1")
DST=$(dirname "$(realpath "$0")")/../linux
mkdir -p "$DST"
cd "$SRC"
{
	# DRM core, helpers, TTM, the scheduler and the AMD driver
	find drivers/gpu/drm -maxdepth 1 -type f \( -name 'drm_*.[ch]' -o -name '*.h' \)
	find drivers/gpu/drm/ttm drivers/gpu/drm/scheduler drivers/gpu/drm/display \
		-type f -name '*.[ch]' -not -path '*/tests/*'
	find drivers/gpu/drm/amd -type f \( -name '*.[ch]' -o -name 'Makefile' \
		-o -name '*.S' \) -not -path '*/amdkfd/*' -not -path '*/tests/*'
	find drivers/gpu/drm/amd/amdkfd -type f -name '*.h'
	# compute accelerator nodes (for the AMD NPU driver, which
	# tools/import-amdxdna.sh imports from a newer Linux)
	echo drivers/accel/drm_accel.c
	# fences, reservation objects, dma-buf and sync files
	find drivers/dma-buf -maxdepth 1 -type f -name '*.[ch]'
	# DRM headers, their UAPI, and the Linux headers of the subsystems above
	find include/drm include/uapi/drm -type f -name '*.h'
	ls include/linux/dma-fence*.h include/linux/dma-resv.h include/linux/dma-buf.h \
		include/linux/sync_file.h include/linux/iosys-map.h include/linux/dma-heap.h \
		include/uapi/linux/dma-buf.h include/uapi/linux/sync_file.h
	# Linux headers that are plain C (no kernel internals), used unchanged
	for h in list rbtree rbtree_types rbtree_augmented interval_tree \
			interval_tree_generic llist hash hashtable overflow container_of \
			stddef bits bitfield build_bug minmax math log2 gcd units util_macros \
			wordpart average unaligned unaligned/packed_struct kref list_sort sort \
			bsearch err stringify poison sizes hdmi kstrtox ctype limits const \
			array_size args compiler_attributes typecheck kfifo string_choices \
			cleanup kconfig circ_buf types compiler_types compiler-gcc compiler compiler-context-analysis ascii85 scatterlist iosys-map nospec \
			instruction_pointer jhash uuid; do
		echo include/linux/$h.h
	done
	ls include/vdso/bits.h include/vdso/const.h include/vdso/limits.h \
		include/vdso/unaligned.h include/uapi/linux/stddef.h \
		include/uapi/linux/const.h include/uapi/linux/bits.h \
		include/uapi/linux/limits.h include/uapi/linux/kernel.h \
		include/uapi/linux/swab.h include/uapi/linux/magic.h \
		include/uapi/linux/kfd_ioctl.h include/uapi/linux/media-bus-format.h \
		include/uapi/linux/byteorder/little_endian.h \
		include/linux/byteorder/little_endian.h include/linux/byteorder/generic.h \
		include/uapi/linux/ioctl.h include/uapi/asm-generic/ioctl.h \
		include/uapi/asm-generic/errno-base.h include/uapi/asm-generic/errno.h \
		include/uapi/asm-generic/int-ll64.h include/asm-generic/int-ll64.h \
		include/uapi/linux/hdmi.h include/uapi/linux/types.h \
		include/uapi/asm-generic/types.h include/uapi/linux/posix_types.h \
		include/uapi/asm-generic/posix_types.h include/linux/swab.h \
		include/asm-generic/bitops/const_hweight.h include/linux/uuid.h \
		include/uapi/linux/uuid.h include/asm-generic/rwonce.h include/linux/pci_ids.h lib/bitmap.c lib/find_bit.c drivers/i2c/algos/i2c-algo-bit.c include/linux/acpi_amd_wbrf.h include/linux/seq_buf.h lib/seq_buf.c include/linux/gpu_buddy.h drivers/gpu/buddy.c include/uapi/linux/i2c.h include/linux/string_helpers.h include/linux/bvec.h include/trace/events/dma_fence.h include/uapi/linux/mei.h include/uapi/linux/mei_uuid.h include/linux/mod_devicetable.h include/uapi/linux/pci_regs.h include/vdso/align.h include/asm-generic/bitops/arch_hweight.h include/asm-generic/bitsperlong.h include/uapi/asm-generic/bitsperlong.h include/linux/err.h include/linux/kern_levels.h include/linux/once_lite.h include/asm-generic/bug.h include/linux/instrumented.h include/linux/bitmap.h include/linux/find.h include/linux/bitops.h include/asm-generic/bitops/generic-non-atomic.h include/asm-generic/bitops/instrumented-non-atomic.h include/linux/align.h include/linux/math64.h include/vdso/math64.h include/linux/time64.h include/vdso/time64.h include/uapi/linux/time.h include/uapi/linux/time_types.h include/vdso/time.h include/vdso/ktime.h include/vdso/jiffies.h include/linux/string.h include/linux/fortify-string.h include/linux/err.h include/linux/kstrtox.h include/linux/sprintf.h include/linux/hex.h include/linux/static_call_types.h include/linux/kern_levels.h include/asm-generic/bitops/fls64.h include/asm-generic/bitops/hweight.h include/asm-generic/bitops/le.h include/asm-generic/bitops/ext2-atomic.h include/asm-generic/bitops/sched.h 2>/dev/null || true
	# library code the DRM and AMD sources call
	ls lib/sort.c lib/list_sort.c lib/bsearch.c lib/rbtree.c lib/interval_tree.c \
		lib/kstrtox.c lib/kstrtox.h lib/ctype.c lib/hexdump.c lib/math/gcd.c \
		lib/scatterlist.c lib/kfifo.c lib/llist.c lib/refcount.c \
		lib/string_helpers.c lib/uuid.c lib/math/div64.c \
		drivers/video/hdmi.c 2>/dev/null || true
	echo COPYING
	ls LICENSES/preferred/* LICENSES/exceptions/* 2>/dev/null || true
} | sort -u > "$DST/.files"
# Existing files are kept: Haiku changes to them are commits in this
# repository. To move to a new Linux release, import into a clean tree and
# rebase the Haiku commits.
tar -cf - -T "$DST/.files" | tar -xf - --skip-old-files -C "$DST"
make -s kernelversion > "$DST/VERSION" 2>/dev/null || sed -n 's/^VERSION = //p;s/^PATCHLEVEL = //p;s/^SUBLEVEL = //p' Makefile | paste -sd. > "$DST/VERSION"
echo "imported $(wc -l < "$DST/.files") files from Linux $(cat "$DST/VERSION")"
