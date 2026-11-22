/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Steps through i915_drm one operation at a time, each logged and synced
	to a file before it runs, so that a machine that stops tells which one
	stopped it:

	i915_smoke [log file] [last stage]

	stages: 1 open + getparam, 2 buffer object, 3 CPU map (WB), 4 CPU map
	(WC), 5 store-dword batch on each engine, 6 1000 batches on each engine,
	7 aperture (GTT) map, 8 aperture map of an X-tiled buffer (fence),
	11 a user pointer object (the caller's memory), written by each engine

	Built on the machine: gcc -O2 -o i915_smoke i915_smoke.c
	-I<dir with drm.h, drm_haiku.h, i915_drm.h> */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <OS.h>

#include "drm.h"
#include "drm_haiku.h"
#include "i915_drm.h"


static int sLog = -1;
static int sFd = -1;


static void
step(const char* format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	if (length < 0)
		return;
	if (length > (int)sizeof(line) - 2)
		length = sizeof(line) - 2;
	line[length++] = '\n';
	fwrite(line, 1, length, stdout);
	fflush(stdout);
	if (sLog >= 0) {
		write(sLog, line, length);
		fsync(sLog);
	}
}


static int
xioctl(unsigned long request, void* argument)
{
	int result;
	do {
		result = ioctl(sFd, request, argument, 0);
	} while (result == -1 && (errno == EINTR || errno == EAGAIN));
	return result == -1 ? -errno : 0;
}


static uint32_t
create_bo(uint64_t size)
{
	struct drm_i915_gem_create create = {};
	create.size = size;
	int error = xioctl(DRM_IOCTL_I915_GEM_CREATE, &create);
	if (error != 0) {
		step("GEM_CREATE %llu failed: %s", (unsigned long long)size,
			strerror(-error));
		return 0;
	}
	return create.handle;
}


static void
close_bo(uint32_t handle)
{
	struct drm_gem_close close = {};
	close.handle = handle;
	xioctl(DRM_IOCTL_GEM_CLOSE, &close);
}


static void*
map_bo(uint32_t handle, uint64_t size, uint32_t type)
{
	struct drm_i915_gem_mmap_offset offset = {};
	offset.handle = handle;
	offset.flags = type;
	int error = xioctl(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &offset);
	if (error != 0) {
		step("MMAP_OFFSET type %u failed: %s", type, strerror(-error));
		return NULL;
	}
	struct drm_haiku_map map = {};
	map.offset = offset.offset;
	map.size = size;
	map.flags = DRM_HAIKU_MAP_WRITE;
	error = xioctl(DRM_HAIKU_IOCTL_MAP, &map);
	if (error != 0) {
		step("HAIKU_MAP type %u failed: %s", type, strerror(-error));
		return NULL;
	}
	return (void*)(uintptr_t)map.address;
}


static void
unmap_bo(void* address)
{
	struct drm_haiku_unmap unmap = {};
	unmap.address = (uintptr_t)address;
	xioctl(DRM_HAIKU_IOCTL_UNMAP, &unmap);
}


static int
wait_bo(uint32_t handle)
{
	struct drm_i915_gem_wait wait = {};
	wait.bo_handle = handle;
	wait.timeout_ns = 2000000000LL;
	return xioctl(DRM_IOCTL_I915_GEM_WAIT, &wait);
}


/* MI_STORE_DWORD_IMM (Gen 8+: 64-bit address) of \a value at offset 0 of
   the target, relocated */
static int
store_dword(uint32_t batch, uint32_t* batchMap, uint32_t target,
	unsigned engine, uint32_t value)
{
	uint32_t n = 0;
	batchMap[n++] = (0x20u << 23) | (4 - 2);	/* PPGTT address */
	uint32_t relocOffset = n * 4;
	batchMap[n++] = 0;
	batchMap[n++] = 0;
	batchMap[n++] = value;
	batchMap[n++] = 0x0au << 23;	/* MI_BATCH_BUFFER_END */
	batchMap[n++] = 0;

	struct drm_i915_gem_relocation_entry reloc = {};
	reloc.target_handle = 0;
	reloc.offset = relocOffset;
	reloc.read_domains = I915_GEM_DOMAIN_INSTRUCTION;
	reloc.write_domain = I915_GEM_DOMAIN_INSTRUCTION;

	struct drm_i915_gem_exec_object2 objects[2] = {};
	objects[0].handle = target;
	objects[0].flags = EXEC_OBJECT_WRITE;
	objects[1].handle = batch;
	objects[1].relocation_count = 1;
	objects[1].relocs_ptr = (uintptr_t)&reloc;

	struct drm_i915_gem_execbuffer2 execbuffer = {};
	execbuffer.buffers_ptr = (uintptr_t)objects;
	execbuffer.buffer_count = 2;
	execbuffer.batch_len = n * 4;
	execbuffer.flags = engine | I915_EXEC_HANDLE_LUT;
	return xioctl(DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuffer);
}


static const struct {
	const char* name;
	unsigned flags;
} kEngines[] = {
	{ "rcs", I915_EXEC_RENDER },
	{ "bcs", I915_EXEC_BLT },
	{ "vcs0", I915_EXEC_BSD | I915_EXEC_BSD_RING1 },
	{ "vcs1", I915_EXEC_BSD | I915_EXEC_BSD_RING2 },
	{ "vecs", I915_EXEC_VEBOX },
};


int
main(int argc, char** argv)
{
	const char* logPath = argc > 1 ? argv[1] : "/boot/home/i915_smoke.log";
	int last = argc > 2 ? atoi(argv[2]) : 99;
	sLog = open(logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
	step("--- i915_smoke, up to stage %d", last);

	step("1 open renderD128");
	sFd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (sFd < 0) {
		step("open failed: %s", strerror(errno));
		return 1;
	}
	int chipset = 0;
	struct drm_i915_getparam param = {};
	param.param = I915_PARAM_CHIPSET_ID;
	param.value = &chipset;
	int error = xioctl(DRM_IOCTL_I915_GETPARAM, &param);
	step("1 getparam: %d, chipset %#x", error, chipset);
	if (getenv("I915_SMOKE_TRACE_ALL") != NULL
		|| getenv("I915_SMOKE_TRACE_OFF") != NULL) {
		struct drm_haiku_set_param traceParam = {};
		strcpy(traceParam.name, "trace_calls");
		strcpy(traceParam.value,
			getenv("I915_SMOKE_TRACE_OFF") != NULL ? "0" : "4");
		int card = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
		int result = card >= 0 ? ioctl(card, DRM_HAIKU_IOCTL_SET_PARAM,
			&traceParam, 0) : -1;
		step("1 call trace on (card0 %d): %d %s", card, result,
			result != 0 ? strerror(errno) : "");
		if (card >= 0)
			close(card);
		if (getenv("I915_SMOKE_TRACE_OFF") != NULL)
			return 0;
	}
	if (last < 2)
		return 0;

	step("2 GEM_CREATE 64 KiB");
	uint32_t target = create_bo(65536);
	uint32_t batch = create_bo(4096);
	step("2 handles %u %u", target, batch);
	if (target == 0 || batch == 0 || last < 3)
		return 0;

	step("3 map WB");
	uint32_t* targetMap = map_bo(target, 65536, I915_MMAP_OFFSET_WB);
	step("3 mapped at %p, writing", targetMap);
	if (targetMap == NULL)
		return 1;
	targetMap[0] = 0x12345678;
	targetMap[16383] = 0x9abcdef0;
	step("3 read back %#x %#x", targetMap[0], targetMap[16383]);
	if (last < 4)
		return 0;

	step("4 map WC (batch)");
	uint32_t* batchMap = map_bo(batch, 4096, I915_MMAP_OFFSET_WC);
	if (batchMap == NULL) {
		step("4 WC refused, WB instead");
		batchMap = map_bo(batch, 4096, I915_MMAP_OFFSET_WB);
	}
	step("4 mapped at %p", batchMap);
	if (batchMap == NULL)
		return 1;
	batchMap[0] = 0;
	step("4 written");
	if (last < 5)
		return 0;

	for (unsigned e = 0; e < sizeof(kEngines) / sizeof(kEngines[0]); e++) {
		uint32_t value = 0xc0de0000 | e;
		targetMap[0] = 0;
		step("5 store-dword on %s: submit", kEngines[e].name);
		error = store_dword(batch, batchMap, target, kEngines[e].flags,
			value);
		step("5 %s: submitted (%d), waiting", kEngines[e].name, error);
		if (error != 0)
			continue;
		error = wait_bo(target);
		step("5 %s: wait %d, value %#x (%s)", kEngines[e].name, error,
			targetMap[0], targetMap[0] == value ? "ok" : "WRONG");
	}
	if (last < 6)
		return 0;

	for (unsigned e = 0; getenv("I915_SMOKE_SKIP6") == NULL
			&& e < sizeof(kEngines) / sizeof(kEngines[0]); e++) {
		step("6 1000 batches on %s", kEngines[e].name);
		bigtime_t start = system_time();
		int failures = 0;
		for (int i = 0; i < 1000; i++) {
			if (wait_bo(batch) != 0)
				failures++;
			if (store_dword(batch, batchMap, target, kEngines[e].flags, i)
					!= 0)
				failures++;
		}
		wait_bo(target);
		step("6 %s: %d failures, %lld us, value %u", kEngines[e].name,
			failures, (long long)(system_time() - start), targetMap[0]);
	}
	if (last < 7)
		return 0;

	step("7 map GTT (aperture)");
	uint32_t* gttMap = map_bo(target, 65536, I915_MMAP_OFFSET_GTT);
	step("7 mapped at %p, reading", gttMap);
	if (gttMap != NULL) {
		step("7 read %#x, writing", gttMap[0]);
		gttMap[1] = 0x5a5a5a5a;
		step("7 written, read back WB %#x", targetMap[1]);
		unmap_bo(gttMap);
		step("7 unmapped");
	}
	if (last < 8)
		goto done;

	step("8 X-tiled buffer, GTT map (a fence)");
	uint32_t tiled = create_bo(1024 * 1024);
	struct drm_i915_gem_set_tiling tiling = {};
	tiling.handle = tiled;
	tiling.tiling_mode = I915_TILING_X;
	tiling.stride = 4096;
	step("8 set_tiling %d", xioctl(DRM_IOCTL_I915_GEM_SET_TILING, &tiling));
	uint32_t* tiledMap = map_bo(tiled, 1024 * 1024, I915_MMAP_OFFSET_GTT);
	step("8 mapped at %p", tiledMap);
	if (tiledMap != NULL) {
		for (int i = 0; i < 1024 * 256; i += 1024)
			tiledMap[i] = i;
		step("8 written, read %u", tiledMap[1024 * 255]);
		unmap_bo(tiledMap);
	}
	close_bo(tiled);

	if (last < 9)
		goto done;

	{
		step("9 pwrite 64 KiB");
		static uint32_t data[16384];
		for (int i = 0; i < 16384; i++)
			data[i] = 0x10000 + i;
		struct drm_i915_gem_pwrite pwrite = {};
		pwrite.handle = target;
		pwrite.offset = 0;
		pwrite.size = sizeof(data);
		pwrite.data_ptr = (uintptr_t)data;
		step("9 pwrite: %d", xioctl(DRM_IOCTL_I915_GEM_PWRITE, &pwrite));
		struct drm_i915_gem_set_domain domain = {};
		domain.handle = target;
		domain.read_domains = I915_GEM_DOMAIN_CPU;
		step("9 set_domain CPU: %d, value %#x %#x", xioctl(
			DRM_IOCTL_I915_GEM_SET_DOMAIN, &domain), targetMap[0],
			targetMap[16383]);
		memset(data, 0, sizeof(data));
		struct drm_i915_gem_pread pread = {};
		pread.handle = target;
		pread.size = sizeof(data);
		pread.data_ptr = (uintptr_t)data;
		step("9 pread: %d, %#x %#x", xioctl(DRM_IOCTL_I915_GEM_PREAD, &pread),
			data[0], data[16383]);

		step("9 context create");
		struct drm_i915_gem_context_create context = {};
		step("9 context: %d, id %u", xioctl(
			DRM_IOCTL_I915_GEM_CONTEXT_CREATE, &context), context.ctx_id);

		struct drm_i915_gem_busy busy = {};
		busy.handle = target;
		step("9 busy: %d, %#x", xioctl(DRM_IOCTL_I915_GEM_BUSY, &busy),
			busy.busy);
		struct drm_i915_gem_madvise madvise = {};
		madvise.handle = target;
		madvise.madv = I915_MADV_DONTNEED;
		step("9 madvise dontneed: %d", xioctl(DRM_IOCTL_I915_GEM_MADVISE,
			&madvise));
		madvise.madv = I915_MADV_WILLNEED;
		step("9 madvise willneed: %d retained %u", xioctl(
			DRM_IOCTL_I915_GEM_MADVISE, &madvise), madvise.retained);
		struct drm_i915_gem_get_aperture aperture = {};
		step("9 aperture: %d, %llu MiB", xioctl(DRM_IOCTL_I915_GEM_GET_APERTURE,
			&aperture), (unsigned long long)aperture.aper_size >> 20);
		struct drm_i915_gem_context_destroy destroy = {};
		destroy.ctx_id = context.ctx_id;
		step("9 context destroy: %d", xioctl(
			DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, &destroy));
	}

	if (last < 10)
		goto done;
	{
		static uint32_t data[16384];
		for (int i = 0; i < 16384; i++)
			data[i] = 0x20000 + i;
		struct drm_i915_gem_pwrite pwrite = {};
		pwrite.size = sizeof(data);
		pwrite.data_ptr = (uintptr_t)data;
		struct drm_i915_gem_pread pread = {};
		pread.size = sizeof(data);
		pread.data_ptr = (uintptr_t)data;
		if (getenv("I915_SMOKE_TRACE") != NULL) {
			struct drm_haiku_set_param param = {};
			strcpy(param.name, "trace_calls");
			strcpy(param.value, "4");
			step("10 call trace on: %d", xioctl(DRM_HAIKU_IOCTL_SET_PARAM,
				&param));
		}
		step("10 pwrite into an X-tiled buffer");
		uint32_t tiled2 = create_bo(1024 * 1024);
		struct drm_i915_gem_set_tiling tiling2 = {};
		tiling2.handle = tiled2;
		tiling2.tiling_mode = I915_TILING_Y;
		tiling2.stride = 4096;
		step("10 set_tiling Y %d", xioctl(DRM_IOCTL_I915_GEM_SET_TILING,
			&tiling2));
		pwrite.handle = tiled2;
		step("10 pwrite tiled: %d", xioctl(DRM_IOCTL_I915_GEM_PWRITE, &pwrite));
		pread.handle = tiled2;
		step("10 pread tiled: %d, %#x", xioctl(DRM_IOCTL_I915_GEM_PREAD,
			&pread), data[1]);
		close_bo(tiled2);

	}
	if (last < 11)
		goto done;

	{
		/* what ANV's VK_EXT_external_memory_host is made of */
		step("11 userptr: 64 KiB of a fully locked area");
		uint32_t* memory = NULL;
		area_id area = create_area("i915_smoke userptr", (void**)&memory,
			B_ANY_ADDRESS, 65536, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (area < 0) {
			step("11 create_area failed: %s", strerror(area));
			goto done;
		}
		memset(memory, 0, 65536);

		struct drm_i915_gem_userptr userptr = {};
		userptr.user_ptr = (uintptr_t)memory + 1;
		userptr.user_size = 4096;
		step("11 GEM_USERPTR of an unaligned address: %d (wanted: %d)",
			xioctl(DRM_IOCTL_I915_GEM_USERPTR, &userptr), -EINVAL);

		userptr.user_ptr = (uintptr_t)memory;
		userptr.user_size = 65536;
		userptr.flags = I915_USERPTR_PROBE;
		error = xioctl(DRM_IOCTL_I915_GEM_USERPTR, &userptr);
		step("11 GEM_USERPTR with PROBE: %d, handle %u", error,
			userptr.handle);
		if (error != 0) {
			userptr.flags = 0;
			error = xioctl(DRM_IOCTL_I915_GEM_USERPTR, &userptr);
			step("11 GEM_USERPTR without: %d, handle %u", error,
				userptr.handle);
		}
		if (error == 0) {
			for (unsigned e = 0; e < sizeof(kEngines) / sizeof(kEngines[0]);
					e++) {
				uint32_t value = 0x05e70000 | e;
				memory[0] = 0;
				wait_bo(batch);
				error = store_dword(batch, batchMap, userptr.handle,
					kEngines[e].flags, value);
				step("11 store-dword into it on %s: submitted (%d)",
					kEngines[e].name, error);
				if (error != 0)
					continue;
				error = wait_bo(userptr.handle);
				step("11 %s: wait %d, the memory reads %#x (%s)",
					kEngines[e].name, error, memory[0],
					memory[0] == value ? "ok" : "WRONG");
			}
			close_bo(userptr.handle);
			step("11 closed; deleting the area (waits until the driver has "
				"let go of its pages)");
		}
		delete_area(area);
		step("11 area deleted");
	}

done:
	unmap_bo(batchMap);
	unmap_bo(targetMap);
	close_bo(batch);
	close_bo(target);
	close(sFd);
	step("done");
	return 0;
}
