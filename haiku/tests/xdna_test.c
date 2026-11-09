/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The AMD NPU driver (amdxdna) from user space, without XRT: Linux's
	ioctls (uapi/drm/amdxdna_accel.h) on /dev/npu/amdxdna/0, and
	DRM_HAIKU_IOCTL_MAP for what Linux does with mmap().

	xdna_test [-d <device>] info
		the NPU's geometry, versions and clocks (starts the NPU: it is powered
		while the node is open)
	xdna_test [-d <device>] buffers
		buffer objects only: the device heap, buffers in it, command and
		data buffers, their mappings and what becomes of them. Needs no NPU
		behind the node: also runs on the device without hardware of a
		virtual machine (driver settings: "haiku_selftest 2").
	xdna_test [-d <device>] [-t <ms>] [-n <runs>] [-o <file>] [-b]
			[-p <mode>] run <design dir>
		loads a design and runs it: <design dir>/manifest.txt names the
		configuration (a raw PDI, as mlir-aie's aiecc writes it), the
		instruction sequence and the buffers with their contents, the
		expected output among them (haiku/tests/xdna/build-designs.sh makes
		such directories). -o keeps the first output buffer of the first
		run (for "reference.py check"). -b passes the size of the
		instruction sequence in bytes, as mlir-aie's Python host code does;
		the default is in words, as its C++ tests do. -p sets the NPU's
		power mode first (enum amdxdna_power_mode_type: 0 default, 1 low,
		2 medium, 3 high, 4 turbo; it stays until the NPU is powered down).

	"buffers" counts the free buffer memory before and after: run it alone.

	Without an IOMMU the addresses the NPU is given are physical ones: what
	DRM_IOCTL_AMDXDNA_GET_BO_INFO returns as xdna_addr for a data buffer goes
	into the command as it is, exactly as the address of the team's mapping
	does on Linux. Only root can open the node.

	cc -I<linux-drm>/linux/include/uapi -I<linux-drm>/haiku/compat/include \
		-o xdna_test xdna_test.c */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>

#include <drm/drm.h>
#include <drm/amdxdna_accel.h>
#include <drm/drm_haiku.h>


#define PAGE			4096UL
#define HEAP_SIZE		(64UL << 20)
	/* amdxdna's AIE2_DEVM_SIZE: the device heap comes in chunks of it */

/* the command buffer's header (amdxdna_ctx.h) */
#define CMD_STATE(header)	((header) & 0xf)
#define CMD_HEADER(state, count, opcode) \
	((uint32_t)(state) | ((uint32_t)(count) << 12) | ((uint32_t)(opcode) << 23))
#define ERT_START_CU			0
#define ERT_CMD_STATE_NEW		1
#define ERT_CMD_STATE_COMPLETED	4

static const char *const kCmdStates[] = {
	"invalid", "new", "queued", "running", "completed", "error", "abort",
	"submitted", "timeout", "no response"
};


static int sFd = -1;
static int sFailures;


static void
fail(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	printf("FAIL: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	sFailures++;
}


/* \a what says what is checked (and how it came out) */
#define CHECK(condition, what...) \
	do { \
		if (condition) { \
			printf("ok: "); \
			printf(what); \
			printf("\n"); \
		} else \
			fail(what); \
	} while (0)


static void
die(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	printf("FAIL: ");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	exit(1);
}


/* #pragma mark - buffer objects */


struct buffer {
	uint32_t handle;
	uint64_t size;
	uint64_t map_offset;
	uint64_t vaddr;			/* where the driver has it mapped, if it knows */
	uint64_t xdna_addr;		/* the address the NPU uses for it */
	void *map;
};


static int
bo_info(struct buffer *bo)
{
	struct amdxdna_drm_get_bo_info info = { .handle = bo->handle };
	if (ioctl(sFd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &info) != 0)
		return -1;
	bo->map_offset = info.map_offset;
	bo->vaddr = info.vaddr;
	bo->xdna_addr = info.xdna_addr;
	return 0;
}


static int
bo_create(struct buffer *bo, uint32_t type, uint64_t size)
{
	struct amdxdna_drm_create_bo create = { .size = size, .type = type };
	memset(bo, 0, sizeof(*bo));
	if (ioctl(sFd, DRM_IOCTL_AMDXDNA_CREATE_BO, &create) != 0)
		return -1;
	bo->handle = create.handle;
	bo->size = size;
	return bo_info(bo);
}


/*	Maps a buffer into the team; buffers in the device heap (type DEV) are
	part of the heap's mapping instead. */
static void *
bo_map(struct buffer *bo)
{
	struct drm_haiku_map map = {
		.offset = bo->map_offset,
		.size = (bo->size + PAGE - 1) & ~(PAGE - 1),
		.flags = DRM_HAIKU_MAP_WRITE,
	};
	if (ioctl(sFd, DRM_HAIKU_IOCTL_MAP, &map) != 0)
		return NULL;
	bo->map = (void *)(uintptr_t)map.address;
	return bo->map;
}


static int
bo_unmap(struct buffer *bo)
{
	struct drm_haiku_unmap unmap = { .address = (uintptr_t)bo->map };
	if (bo->map == NULL)
		return 0;
	bo->map = NULL;
	return ioctl(sFd, DRM_HAIKU_IOCTL_UNMAP, &unmap);
}


static int
bo_close(struct buffer *bo)
{
	struct drm_gem_close close = { .handle = bo->handle };
	if (bo->handle == 0)
		return 0;
	bo->handle = 0;
	return ioctl(sFd, DRM_IOCTL_GEM_CLOSE, &close);
}


/*	Before the NPU reads a buffer the processor wrote, and before the
	processor reads what the NPU wrote: the driver's sync ioctl flushes the
	buffer from the processor's caches. For buffers of the carveout memory
	(all of them without an IOMMU) Linux's driver does not offer it
	(EOPNOTSUPP); the cache lines are flushed here then, which every program
	may do for memory it has mapped. \a memory is the buffer in the team. */
static int
bo_sync(struct buffer *bo, uint32_t direction, void *memory)
{
	struct amdxdna_drm_sync_bo sync = {
		.handle = bo->handle,
		.direction = direction,
		.size = bo->size,
	};
	uint64_t offset;

	if (ioctl(sFd, DRM_IOCTL_AMDXDNA_SYNC_BO, &sync) == 0)
		return 0;
	if (errno != EOPNOTSUPP || memory == NULL)
		return -1;
	__builtin_ia32_mfence();
	for (offset = 0; offset < bo->size; offset += 64)
		__builtin_ia32_clflush((uint8_t *)memory + offset);
	__builtin_ia32_mfence();
	return 0;
}


/*	The device heap: one buffer of 64 MiB, which has to be mapped before
	buffers can be made in it. They have no mapping of their own. */
static struct buffer sHeap;


static void
heap_create(void)
{
	if (bo_create(&sHeap, AMDXDNA_BO_DEV_HEAP, HEAP_SIZE) != 0)
		die("device heap: %s", strerror(errno));
	if (bo_map(&sHeap) == NULL)
		die("mapping the device heap: %s", strerror(errno));
}


static void *
heap_pointer(const struct buffer *bo)
{
	return (uint8_t *)sHeap.map + (bo->xdna_addr - sHeap.xdna_addr);
}


/* #pragma mark - info */


static int
get_info(uint32_t parameter, void *buffer, uint32_t size)
{
	struct amdxdna_drm_get_info info = {
		.param = parameter,
		.buffer_size = size,
		.buffer = (uintptr_t)buffer,
	};
	return ioctl(sFd, DRM_IOCTL_AMDXDNA_GET_INFO, &info);
}


static int
print_info(struct amdxdna_drm_query_aie_metadata *_metadata)
{
	struct amdxdna_drm_query_aie_metadata metadata = {};
	struct amdxdna_drm_query_aie_version version = {};
	struct amdxdna_drm_query_firmware_version firmware = {};
	struct amdxdna_drm_query_clock_metadata clocks = {};
	struct amdxdna_drm_get_power_mode power = {};
	struct drm_version drm = {};
	char name[64] = "";

	drm.name = name;
	drm.name_len = sizeof(name) - 1;
	if (ioctl(sFd, DRM_IOCTL_VERSION, &drm) == 0) {
		printf("driver: %s %d.%d\n", name, drm.version_major,
			drm.version_minor);
	} else
		fail("DRM_IOCTL_VERSION: %s", strerror(errno));

	if (get_info(DRM_AMDXDNA_QUERY_AIE_METADATA, &metadata, sizeof(metadata))
			!= 0) {
		fail("AIE metadata: %s", strerror(errno));
		return -1;
	}
	printf("AIE array: %u columns, %u rows, column size %u, version %u.%u\n",
		metadata.cols, metadata.rows, metadata.col_size,
		metadata.version.major, metadata.version.minor);
	printf("  core tiles: rows %u from row %u, %u DMA channels, %u locks, "
		"%u events\n", metadata.core.row_count, metadata.core.row_start,
		metadata.core.dma_channel_count, metadata.core.lock_count,
		metadata.core.event_reg_count);
	printf("  memory tiles: rows %u from row %u, %u DMA channels, %u locks\n",
		metadata.mem.row_count, metadata.mem.row_start,
		metadata.mem.dma_channel_count, metadata.mem.lock_count);
	printf("  shim tiles: rows %u from row %u, %u DMA channels, %u locks\n",
		metadata.shim.row_count, metadata.shim.row_start,
		metadata.shim.dma_channel_count, metadata.shim.lock_count);
	if (_metadata != NULL)
		*_metadata = metadata;

	if (get_info(DRM_AMDXDNA_QUERY_AIE_VERSION, &version, sizeof(version)) == 0)
		printf("AIE version: %u.%u\n", version.major, version.minor);
	else
		fail("AIE version: %s", strerror(errno));
	if (get_info(DRM_AMDXDNA_QUERY_FIRMWARE_VERSION, &firmware,
			sizeof(firmware)) == 0) {
		printf("firmware: %u.%u.%u.%u\n", firmware.major, firmware.minor,
			firmware.patch, firmware.build);
	} else
		fail("firmware version: %s", strerror(errno));
	if (get_info(DRM_AMDXDNA_QUERY_CLOCK_METADATA, &clocks, sizeof(clocks))
			== 0) {
		printf("clocks: %.16s %u MHz, %.16s %u MHz\n",
			(const char *)clocks.mp_npu_clock.name,
			clocks.mp_npu_clock.freq_mhz, (const char *)clocks.h_clock.name,
			clocks.h_clock.freq_mhz);
	} else
		printf("clocks: %s\n", strerror(errno));
	if (get_info(DRM_AMDXDNA_GET_POWER_MODE, &power, sizeof(power)) == 0)
		printf("power mode: %u\n", power.power_mode);
	return 0;
}


/* #pragma mark - buffers */


static uint32_t
pattern(uint32_t seed, uint32_t index)
{
	return (seed * 2654435761u) ^ (index * 40503u + 0x9e3779b9u);
}


static void
fill(void *memory, size_t size, uint32_t seed)
{
	uint32_t *words = memory;
	size_t i;
	for (i = 0; i < size / 4; i++)
		words[i] = pattern(seed, i);
}


static int
filled(const void *memory, size_t size, uint32_t seed)
{
	const uint32_t *words = memory;
	size_t i;
	for (i = 0; i < size / 4; i++) {
		if (words[i] != pattern(seed, i))
			return 0;
	}
	return 1;
}


static int
zero(const void *memory, size_t size)
{
	const uint8_t *bytes = memory;
	size_t i;
	for (i = 0; i < size; i++) {
		if (bytes[i] != 0)
			return 0;
	}
	return 1;
}


/* how many 1 MiB data buffers the buffer memory still has room for */
static int
room(void)
{
	static struct buffer buffers[2048];
	int count = 0, i;
	while (count < 2048 && bo_create(&buffers[count], AMDXDNA_BO_SHARE, 1 << 20)
			== 0)
		count++;
	for (i = 0; i < count; i++)
		bo_close(&buffers[i]);
	return count;
}


static void
test_buffers(void)
{
	struct buffer dev, dev2, share, again, cmd, kept, taken;
	void *first, *second;
	uint64_t address;
	int before, after;

	/* a buffer in the heap before there is a heap */
	CHECK(bo_create(&dev, AMDXDNA_BO_DEV, 65536) != 0,
		"no buffer in the device heap before there is a heap");

	heap_create();
	printf("device heap: handle %u, NPU address %#llx, mapped at %p\n",
		sHeap.handle, (unsigned long long)sHeap.xdna_addr, sHeap.map);
	CHECK(bo_info(&sHeap) == 0 && sHeap.vaddr == (uintptr_t)sHeap.map,
		"heap: the driver knows the team's mapping (%#llx, %p)",
		(unsigned long long)sHeap.vaddr, sHeap.map);
	CHECK(zero(sHeap.map, 1 << 20)
		&& zero((uint8_t *)sHeap.map + HEAP_SIZE - (1 << 20), 1 << 20),
		"heap: zeroed");
	before = room();
	printf("room for %d MiB of data buffers besides the heap\n", before);
	CHECK(before >= 4, "at least 4 MiB of buffer memory besides the heap");

	/* buffers in the heap: at the NPU's addresses, in the heap's mapping */
	CHECK(bo_create(&dev, AMDXDNA_BO_DEV, 65536) == 0
		&& bo_create(&dev2, AMDXDNA_BO_DEV, 3 * 4096) == 0,
		"two buffers in the device heap");
	printf("heap buffers at NPU addresses %#llx and %#llx\n",
		(unsigned long long)dev.xdna_addr, (unsigned long long)dev2.xdna_addr);
	CHECK(dev.xdna_addr >= sHeap.xdna_addr
		&& dev.xdna_addr + 65536 <= sHeap.xdna_addr + HEAP_SIZE
		&& (dev.xdna_addr & 0x7fff) == 0 && (dev2.xdna_addr & 0x7fff) == 0
		&& dev2.xdna_addr != dev.xdna_addr,
		"heap buffers: in the heap, 32 KiB aligned, apart (%#llx, %#llx)",
		(unsigned long long)dev.xdna_addr, (unsigned long long)dev2.xdna_addr);
	CHECK(dev.vaddr == (uintptr_t)heap_pointer(&dev),
		"heap buffer: the driver's address for the team is the team's "
		"(%#llx, %p)", (unsigned long long)dev.vaddr, heap_pointer(&dev));
	fill(heap_pointer(&dev), 65536, 1);
	fill(heap_pointer(&dev2), 3 * 4096, 2);
	CHECK(filled(heap_pointer(&dev), 65536, 1)
		&& filled(heap_pointer(&dev2), 3 * 4096, 2),
		"heap buffers: each keeps its contents");

	/* a data buffer: memory of its own, at a physical address */
	CHECK(bo_create(&share, AMDXDNA_BO_SHARE, 1 << 20) == 0,
		"data buffer made");
	first = bo_map(&share);
	CHECK(first != NULL, "data buffer mapped");
	if (first == NULL)
		return;
	CHECK(bo_info(&share) == 0 && (share.xdna_addr & (PAGE - 1)) == 0
		&& share.xdna_addr != 0 && share.xdna_addr != AMDXDNA_INVALID_ADDR,
		"data buffer: page aligned NPU address (%#llx)",
		(unsigned long long)share.xdna_addr);
	printf("data buffer: NPU address %#llx, mapped at %p\n",
		(unsigned long long)share.xdna_addr, first);
	CHECK(zero(first, 1 << 20), "data buffer: zeroed");
	fill(first, 1 << 20, 3);
	CHECK(bo_sync(&share, SYNC_DIRECT_TO_DEVICE, first) == 0
		&& filled(first, 1 << 20, 3), "data buffer synced, contents kept");

	/* a second mapping shows the same memory, and outlives the first */
	again = share;
	second = bo_map(&again);
	CHECK(second != NULL && second != first && filled(second, 1 << 20, 3),
		"second mapping: elsewhere, same contents");
	share.map = first;
	CHECK(bo_unmap(&share) == 0 && filled(second, 1 << 20, 3),
		"second mapping outlives the first");

	/* the handle closed: the mapping keeps the memory, and nobody else
	 * gets it */
	address = share.xdna_addr;
	CHECK(bo_close(&share) == 0, "data buffer's handle closed");
	CHECK(bo_create(&taken, AMDXDNA_BO_SHARE, 1 << 20) == 0
		&& taken.xdna_addr != address,
		"a new buffer does not get the memory of a mapped one (%#llx, %#llx)",
		(unsigned long long)taken.xdna_addr, (unsigned long long)address);
	CHECK(filled(second, 1 << 20, 3), "mapping keeps its contents after its "
		"handle was closed");
	again.handle = 0;
	CHECK(bo_unmap(&again) == 0, "unmapped");
	bo_close(&taken);

	/* freed memory comes back zeroed */
	CHECK(bo_create(&kept, AMDXDNA_BO_SHARE, 1 << 20) == 0
		&& bo_map(&kept) != NULL && zero(kept.map, 1 << 20),
		"a buffer made of freed memory is zeroed");

	/* a command buffer */
	CHECK(bo_create(&cmd, AMDXDNA_BO_CMD, 4096) == 0 && bo_map(&cmd) != NULL,
		"command buffer made and mapped");
	if (cmd.map != NULL)
		memset(cmd.map, 0, 4096);

	/* nothing lost: as much room as before, with what is still held */
	bo_unmap(&cmd);
	bo_close(&cmd);
	bo_unmap(&kept);
	bo_close(&kept);
	after = room();
	CHECK(after == before, "no memory lost: room for %d MiB before, %d MiB "
		"after", before, after);

	/* heap buffers and the heap stay for close() to clean up */
	CHECK(filled(heap_pointer(&dev), 65536, 1), "heap buffer still intact");
}


/* #pragma mark - running a design */


/*	Waits for a command. The NPUs of today's processors (the driver's
	"AIE2" devices) signal a command's end on the hardware context's sync
	object, at the point of the command's sequence number: Linux's
	DRM_IOCTL_AMDXDNA_WAIT_CMD is for newer devices only and answers
	EOPNOTSUPP here. The timeout of a sync object wait is a point in time
	of the monotonic clock, which is system_time() on Haiku. */
static int
wait_command(uint32_t context, uint32_t syncObject, uint64_t sequence,
	unsigned timeout)
{
	struct amdxdna_drm_wait_cmd wait = {
		.hwctx = context,
		.timeout = timeout,
		.seq = sequence,
	};
	struct drm_syncobj_timeline_wait timeline = {};
	uint64_t point = sequence;

	if (syncObject == 0)
		return ioctl(sFd, DRM_IOCTL_AMDXDNA_WAIT_CMD, &wait);

	timeline.handles = (uintptr_t)&syncObject;
	timeline.points = (uintptr_t)&point;
	timeline.timeout_nsec = (system_time() + (bigtime_t)timeout * 1000) * 1000;
	timeline.count_handles = 1;
	timeline.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
	return ioctl(sFd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &timeline);
}


#define MAX_BUFFERS		5
	/* mlir-aie's kernels take up to five buffers */

struct design_buffer {
	int output;
	char file[256];
	uint64_t size;
	uint8_t *data;			/* input, or the expected output */
	struct buffer bo;
};

struct design {
	char name[64];
	char pdi[256];
	char insts[256];
	unsigned columns;
	unsigned opcode;
	unsigned ops_per_cycle;
	int buffer_count;
	struct design_buffer buffers[MAX_BUFFERS];
};


static uint8_t *
read_file(const char *directory, const char *name, uint64_t *_size)
{
	char path[1024];
	struct stat st;
	uint8_t *data;
	int fd;

	snprintf(path, sizeof(path), "%s/%s", directory, name);
	fd = open(path, O_RDONLY);
	if (fd < 0 || fstat(fd, &st) != 0)
		die("%s: %s", path, strerror(errno));
	data = malloc(st.st_size + 1);
	if (data == NULL || read(fd, data, st.st_size) != st.st_size)
		die("%s: cannot read", path);
	close(fd);
	data[st.st_size] = '\0';
	*_size = st.st_size;
	return data;
}


/*	manifest.txt: "key=value" lines; "buffer=in|out:<file>:<bytes>" once per
	buffer, in the order of the kernel's buffer arguments. */
static void
read_manifest(const char *directory, struct design *design)
{
	uint64_t size;
	char *text = (char *)read_file(directory, "manifest.txt", &size);
	char *line, *next;

	memset(design, 0, sizeof(*design));
	design->opcode = 3;
	design->ops_per_cycle = 2048;
	for (line = text; line != NULL && *line != '\0'; line = next) {
		char *value;
		next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (line[0] == '#' || (value = strchr(line, '=')) == NULL)
			continue;
		*value++ = '\0';
		value[strcspn(value, "\r")] = '\0';
		if (!strcmp(line, "name"))
			snprintf(design->name, sizeof(design->name), "%s", value);
		else if (!strcmp(line, "pdi"))
			snprintf(design->pdi, sizeof(design->pdi), "%s", value);
		else if (!strcmp(line, "insts"))
			snprintf(design->insts, sizeof(design->insts), "%s", value);
		else if (!strcmp(line, "columns"))
			design->columns = strtoul(value, NULL, 0);
		else if (!strcmp(line, "opcode"))
			design->opcode = strtoul(value, NULL, 0);
		else if (!strcmp(line, "ops_per_cycle"))
			design->ops_per_cycle = strtoul(value, NULL, 0);
		else if (!strcmp(line, "buffer")) {
			struct design_buffer *buffer;
			char *file = strchr(value, ':');
			char *bytes = file != NULL ? strchr(file + 1, ':') : NULL;
			if (bytes == NULL || design->buffer_count == MAX_BUFFERS)
				die("manifest: buffer=%s", value);
			*file++ = '\0';
			*bytes++ = '\0';
			buffer = &design->buffers[design->buffer_count++];
			buffer->output = !strcmp(value, "out");
			snprintf(buffer->file, sizeof(buffer->file), "%s", file);
			buffer->size = strtoull(bytes, NULL, 0);
		}
	}
	if (design->pdi[0] == '\0' || design->insts[0] == '\0'
		|| design->columns == 0 || design->buffer_count == 0)
		die("manifest: pdi, insts, columns and buffers are needed");
	free(text);
}


static int
compare(const struct design_buffer *buffer, const char *name)
{
	const uint8_t *actual = buffer->bo.map;
	uint64_t i, wrong = 0, first = 0;

	for (i = 0; i < buffer->size; i++) {
		if (actual[i] != buffer->data[i] && wrong++ == 0)
			first = i;
	}
	if (wrong == 0) {
		printf("ok: %s: %llu bytes as expected\n", name,
			(unsigned long long)buffer->size);
		return 0;
	}
	fail("%s: %llu of %llu bytes differ, the first at %llu:", name,
		(unsigned long long)wrong, (unsigned long long)buffer->size,
		(unsigned long long)first);
	printf("  expected");
	for (i = first; i < first + 16 && i < buffer->size; i++)
		printf(" %02x", buffer->data[i]);
	printf("\n  got     ");
	for (i = first; i < first + 16 && i < buffer->size; i++)
		printf(" %02x", actual[i]);
	printf("\n");
	return -1;
}


static void
run_design(const char *directory, unsigned timeout, int runs,
	const char *outputFile, int sizeInBytes)
{
	struct amdxdna_drm_query_aie_metadata metadata;
	struct amdxdna_qos_info qos = { .priority = AMDXDNA_QOS_NORMAL_PRIORITY };
	struct amdxdna_drm_create_hwctx context = {};
	struct amdxdna_drm_destroy_hwctx destroy = {};
	struct amdxdna_drm_config_hwctx config = {};
	struct {
		struct amdxdna_hwctx_param_config_cu header;
		struct amdxdna_cu_config cu;
	} units = {};
	struct design design;
	struct buffer pdi, insts, cmd;
	uint64_t pdiSize, instsSize;
	uint8_t *pdiData, *instsData;
	uint32_t handles[MAX_BUFFERS + 1];
	uint32_t *command;
	int i, run;

	read_manifest(directory, &design);
	pdiData = read_file(directory, design.pdi, &pdiSize);
	instsData = read_file(directory, design.insts, &instsSize);
	for (i = 0; i < design.buffer_count; i++) {
		uint64_t size;
		design.buffers[i].data = read_file(directory, design.buffers[i].file,
			&size);
		if (size != design.buffers[i].size) {
			die("%s has %llu bytes, the manifest says %llu",
				design.buffers[i].file, (unsigned long long)size,
				(unsigned long long)design.buffers[i].size);
		}
	}
	printf("design %s: %u column(s), configuration %llu bytes, %llu "
		"instruction words, %d buffer(s)\n", design.name, design.columns,
		(unsigned long long)pdiSize, (unsigned long long)instsSize / 4,
		design.buffer_count);

	if (print_info(&metadata) != 0)
		exit(1);
	if (design.columns > metadata.cols)
		die("the design needs %u columns, the NPU has %u", design.columns,
			metadata.cols);

	/* the device heap, and in it the configuration and the instructions */
	heap_create();
	if (bo_create(&pdi, AMDXDNA_BO_DEV, pdiSize) != 0
		|| bo_create(&insts, AMDXDNA_BO_DEV, instsSize) != 0)
		die("buffers in the device heap: %s", strerror(errno));
	memcpy(heap_pointer(&pdi), pdiData, pdiSize);
	memcpy(heap_pointer(&insts), instsData, instsSize);
	bo_sync(&pdi, SYNC_DIRECT_TO_DEVICE, heap_pointer(&pdi));
	bo_sync(&insts, SYNC_DIRECT_TO_DEVICE, heap_pointer(&insts));
	printf("configuration at NPU address %#llx, instructions at %#llx\n",
		(unsigned long long)pdi.xdna_addr, (unsigned long long)insts.xdna_addr);

	/* the context: a share of the NPU's columns */
	context.qos_p = (uintptr_t)&qos;
	context.max_opc = design.ops_per_cycle;
	context.num_tiles = design.columns * metadata.core.row_count;
	bigtime_t before = system_time();
	if (ioctl(sFd, DRM_IOCTL_AMDXDNA_CREATE_HWCTX, &context) != 0)
		die("creating the hardware context: %s", strerror(errno));
	printf("hardware context %u, sync object %u (%lld us)\n", context.handle,
		context.syncobj_handle, (long long)(system_time() - before));

	/* the configuration becomes its one compute unit */
	units.header.num_cus = 1;
	units.cu.cu_bo = pdi.handle;
	units.cu.cu_func = 0;
	config.handle = context.handle;
	config.param_type = DRM_AMDXDNA_HWCTX_CONFIG_CU;
	config.param_val = (uintptr_t)&units;
	config.param_val_size = sizeof(units);
	before = system_time();
	if (ioctl(sFd, DRM_IOCTL_AMDXDNA_CONFIG_HWCTX, &config) != 0)
		die("configuring the compute unit: %s", strerror(errno));
	printf("ok: design loaded (%lld us)\n",
		(long long)(system_time() - before));

	/* the data buffers */
	for (i = 0; i < design.buffer_count; i++) {
		struct design_buffer *buffer = &design.buffers[i];
		if (bo_create(&buffer->bo, AMDXDNA_BO_SHARE, buffer->size) != 0
			|| bo_map(&buffer->bo) == NULL)
			die("data buffer %d: %s", i, strerror(errno));
		printf("buffer %d (%s, %s, %llu bytes) at NPU address %#llx\n", i,
			buffer->output ? "out" : "in", buffer->file,
			(unsigned long long)buffer->size,
			(unsigned long long)buffer->bo.xdna_addr);
	}

	if (bo_create(&cmd, AMDXDNA_BO_CMD, 4096) != 0 || bo_map(&cmd) == NULL)
		die("command buffer: %s", strerror(errno));
	command = cmd.map;

	for (run = 0; run < runs; run++) {
		struct amdxdna_drm_exec_cmd exec = {};
		bigtime_t start, submitted, end;
		int count = 0, bad = 0;

		for (i = 0; i < design.buffer_count; i++) {
			struct design_buffer *buffer = &design.buffers[i];
			if (buffer->output)
				memset(buffer->bo.map, 0xa5, buffer->size);
			else
				memcpy(buffer->bo.map, buffer->data, buffer->size);
			bo_sync(&buffer->bo, SYNC_DIRECT_TO_DEVICE, buffer->bo.map);
		}

		/* The command, as XRT writes it for mlir-aie's kernel: the compute
		 * unit's mask, then the kernel's arguments as they lie in its
		 * register map - opcode (64 bits), the instructions' address (64),
		 * their count in words (32) and five buffer addresses (64 each),
		 * unused ones zero. The instructions name the buffers by their
		 * position among the arguments. */
		memset(command, 0, 4096);
		command[1 + count++] = 0x1;
		command[1 + count++] = design.opcode;
		command[1 + count++] = 0;
		command[1 + count++] = (uint32_t)insts.xdna_addr;
		command[1 + count++] = (uint32_t)(insts.xdna_addr >> 32);
		command[1 + count++] = sizeInBytes ? instsSize : instsSize / 4;
		for (i = 0; i < MAX_BUFFERS; i++) {
			uint64_t address = i < design.buffer_count
				? design.buffers[i].bo.xdna_addr : 0;
			command[1 + count++] = (uint32_t)address;
			command[1 + count++] = (uint32_t)(address >> 32);
		}
		command[0] = CMD_HEADER(ERT_CMD_STATE_NEW, count, ERT_START_CU);

		/* the buffers the command refers to */
		handles[0] = insts.handle;
		for (i = 0; i < design.buffer_count; i++)
			handles[1 + i] = design.buffers[i].bo.handle;
		exec.hwctx = context.handle;
		exec.type = AMDXDNA_CMD_SUBMIT_EXEC_BUF;
		exec.cmd_handles = cmd.handle;
		exec.args = (uintptr_t)handles;
		exec.cmd_count = 1;
		exec.arg_count = 1 + design.buffer_count;

		start = system_time();
		if (ioctl(sFd, DRM_IOCTL_AMDXDNA_EXEC_CMD, &exec) != 0)
			die("submitting the command: %s", strerror(errno));
		submitted = system_time();
		if (wait_command(context.handle, context.syncobj_handle, exec.seq,
				timeout) != 0) {
			fail("waiting for command %llu: %s (state: %s)",
				(unsigned long long)exec.seq, strerror(errno),
				kCmdStates[CMD_STATE(command[0]) % 10]);
			break;
		}
		end = system_time();

		if (CMD_STATE(command[0]) != ERT_CMD_STATE_COMPLETED) {
			fail("command %llu ended in state %s; its buffer: %#x %#x %#x "
				"%#x", (unsigned long long)exec.seq,
				kCmdStates[CMD_STATE(command[0]) % 10], command[0],
				command[1], command[2], command[3]);
			break;
		}
		printf("run %d: command %llu completed in %lld us (%lld to submit)\n",
			run, (unsigned long long)exec.seq, (long long)(end - start),
			(long long)(submitted - start));

		for (i = 0; i < design.buffer_count; i++) {
			struct design_buffer *buffer = &design.buffers[i];
			if (!buffer->output)
				continue;
			bo_sync(&buffer->bo, SYNC_DIRECT_FROM_DEVICE, buffer->bo.map);
			if (compare(buffer, buffer->file) != 0)
				bad = 1;
			if (outputFile != NULL) {
				int fd = open(outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
				if (fd < 0 || write(fd, buffer->bo.map, buffer->size)
						!= (ssize_t)buffer->size)
					fail("writing %s", outputFile);
				if (fd >= 0)
					close(fd);
				outputFile = NULL;
			}
		}
		if (bad)
			break;
	}

	destroy.handle = context.handle;
	CHECK(ioctl(sFd, DRM_IOCTL_AMDXDNA_DESTROY_HWCTX, &destroy) == 0,
		"hardware context destroyed");
}


int
main(int argc, char **argv)
{
	const char *device = "/dev/npu/amdxdna/0";
	const char *outputFile = NULL;
	unsigned timeout = 10000;
	int runs = 1;
	int sizeInBytes = 0;
	int powerMode = -1;
	int option;

	setvbuf(stdout, NULL, _IOLBF, 0);
	while ((option = getopt(argc, argv, "d:t:n:o:bp:")) != -1) {
		switch (option) {
			case 'd': device = optarg; break;
			case 't': timeout = strtoul(optarg, NULL, 0); break;
			case 'n': runs = atoi(optarg); break;
			case 'o': outputFile = optarg; break;
			case 'b': sizeInBytes = 1; break;
			case 'p': powerMode = atoi(optarg); break;
			default: argc = 0; break;
		}
	}
	if (optind >= argc) {
		printf("usage: xdna_test [-d <device>] info | buffers | "
			"[-t <ms>] [-n <runs>] [-o <file>] [-b] [-p <mode>] "
			"run <design dir>\n");
		return 2;
	}

	/* opening starts the NPU: firmware, power */
	sFd = open(device, O_RDWR);
	if (sFd < 0) {
		printf("FAIL: %s: %s\n", device, strerror(errno));
		return 1;
	}

	if (powerMode >= 0) {
		struct amdxdna_drm_set_power_mode mode = {
			.power_mode = (uint8_t)powerMode
		};
		struct amdxdna_drm_set_state state = {
			.param = DRM_AMDXDNA_SET_POWER_MODE,
			.buffer_size = sizeof(mode),
			.buffer = (uintptr_t)&mode
		};
		if (ioctl(sFd, DRM_IOCTL_AMDXDNA_SET_STATE, &state) != 0)
			fail("power mode %d: %s", powerMode, strerror(errno));
	}

	if (!strcmp(argv[optind], "info"))
		print_info(NULL);
	else if (!strcmp(argv[optind], "buffers"))
		test_buffers();
	else if (!strcmp(argv[optind], "run") && optind + 1 < argc)
		run_design(argv[optind + 1], timeout, runs, outputFile, sizeInBytes);
	else {
		printf("unknown command %s\n", argv[optind]);
		return 2;
	}

	/* everything left - buffers, mappings, contexts - goes with the file;
	 * the NPU is powered down when the last one is closed */
	close(sFd);
	printf("%s\n", sFailures == 0 ? "PASS" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
