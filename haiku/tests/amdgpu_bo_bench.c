/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	What creating a buffer object costs: amdgpu_bo_alloc() (GEM_CREATE)
	and amdgpu_bo_free() timed call by call, from one or more threads at
	once, while a probe thread of the same team touches fresh pages and
	records how long each page fault takes (jmgasper/haiku#36).

	amdgpu_bo_bench [-s KiB] [-d vram|vramonly|gtt] [-w] [-c] [-l] [-t threads]
		[-n count] [-k keep] [-v] [-m] [-T] [-p]

	-s	buffer size in KiB (1024)
	-d	vram: VRAM|GTT, as RADV asks for images (default); vramonly; gtt
	-w	write-combined GTT (CPU_GTT_USWC), as RADV's staging buffers
	-t	threads allocating at once (1)
	-n	buffers per thread (200)
	-k	buffers each thread keeps alive before freeing its oldest (0)
	-c	VRAM cleared at creation (VRAM_CLEARED), as RADV asks
	-l	local to the VM (VM_ALWAYS_VALID), as RADV makes most buffers
	-v	also map each buffer into the GPU's address space (GEM_VA)
	-m	also map each buffer for the CPU (amdgpu_bo_cpu_map), and unmap
		it before freeing it; times both (jmgasper/haiku#110)
	-T	with -m: write one byte to every page after mapping it
	-p	run the page fault probe, alone for a second first

	build on the machine: gcc -O2 -Wall -o amdgpu_bo_bench amdgpu_bo_bench.c
		-L/boot/system/non-packaged/lib -ldrm_amdgpu -lpthread */

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>


/* libdrm_amdgpu, declared here so that no headers are needed */
typedef struct amdgpu_device* amdgpu_device_handle;
typedef struct amdgpu_bo* amdgpu_bo_handle;
typedef struct amdgpu_va* amdgpu_va_handle;

struct amdgpu_bo_alloc_request {
	uint64_t alloc_size;
	uint64_t phys_alignment;
	uint32_t preferred_heap;
	uint64_t flags;
};

int amdgpu_device_initialize(int fd, uint32_t* major, uint32_t* minor,
	amdgpu_device_handle* device);
int amdgpu_device_deinitialize(amdgpu_device_handle device);
int amdgpu_bo_alloc(amdgpu_device_handle device,
	struct amdgpu_bo_alloc_request* request, amdgpu_bo_handle* bo);
int amdgpu_bo_free(amdgpu_bo_handle bo);
int amdgpu_bo_cpu_map(amdgpu_bo_handle bo, void** cpu);
int amdgpu_bo_cpu_unmap(amdgpu_bo_handle bo);
int amdgpu_va_range_alloc(amdgpu_device_handle device, int type,
	uint64_t size, uint64_t alignment, uint64_t base_required,
	uint64_t* va_base_allocated, amdgpu_va_handle* va_range_handle,
	uint64_t flags);
int amdgpu_va_range_free(amdgpu_va_handle va_range_handle);
int amdgpu_bo_va_op(amdgpu_bo_handle bo, uint64_t offset, uint64_t size,
	uint64_t address, uint64_t flags, uint32_t ops);

struct amdgpu_heap_info {
	uint64_t heap_size;
	uint64_t heap_usage;
	uint64_t max_allocation;
};

int amdgpu_query_heap_info(amdgpu_device_handle device, uint32_t heap,
	uint32_t flags, struct amdgpu_heap_info* info);

#define DOMAIN_GTT			0x2
#define DOMAIN_VRAM			0x4
#define CREATE_NO_CPU_ACCESS	(1 << 1)
#define CREATE_CPU_GTT_USWC		(1 << 2)
#define CREATE_VRAM_CLEARED		(1 << 3)
#define CREATE_VM_ALWAYS_VALID	(1 << 6)
#define CREATE_EXPLICIT_SYNC	(1 << 7)
#define VA_OP_MAP			1
#define VA_OP_UNMAP			2
#define VA_RANGE_HIGH		(1 << 2)


static amdgpu_device_handle sDevice;
static uint64_t sSize = 1024 * 1024;
static uint32_t sDomain = DOMAIN_VRAM | DOMAIN_GTT;
static uint64_t sFlags = CREATE_NO_CPU_ACCESS | CREATE_EXPLICIT_SYNC;
static int sCount = 200, sKeep = 0;
static int sMapVA = 0;
static int sMapCPU = 0, sTouch = 0;
static volatile int sProbeStop;


static double
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}


static int
compare_double(const void* a, const void* b)
{
	double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}


static void
report(const char* name, double* samples, int count)
{
	double sum = 0;
	int i;
	if (count == 0)
		return;
	qsort(samples, count, sizeof(double), compare_double);
	for (i = 0; i < count; i++)
		sum += samples[i];
	printf("  %-6s n %5d  mean %7.3f  p50 %7.3f  p90 %7.3f  p99 %7.3f  "
		"max %7.3f ms\n", name, count, sum / count, samples[count / 2],
		samples[count * 9 / 10], samples[count * 99 / 100],
		samples[count - 1]);
}


struct buffer {
	amdgpu_bo_handle bo;
	amdgpu_va_handle va_handle;
	uint64_t va;
	void* cpu;
};

struct worker {
	pthread_t thread;
	double* alloc;
	double* map;
	double* cpumap;
	double* touch;
	double* cpuunmap;
	double* release;
	int allocated, mapped, cpumapped, touched, cpuunmapped, released;
	int failed;
};


static void
release_buffer(struct worker* worker, struct buffer* buffer)
{
	double t0;
	if (buffer->cpu != NULL) {
		t0 = now_ms();
		amdgpu_bo_cpu_unmap(buffer->bo);
		worker->cpuunmap[worker->cpuunmapped++] = now_ms() - t0;
	}
	t0 = now_ms();
	if (sMapVA) {
		amdgpu_bo_va_op(buffer->bo, 0, sSize, buffer->va, 0, VA_OP_UNMAP);
		amdgpu_va_range_free(buffer->va_handle);
	}
	amdgpu_bo_free(buffer->bo);
	worker->release[worker->released++] = now_ms() - t0;
}


static void*
allocate_loop(void* data)
{
	struct worker* worker = data;
	struct buffer* kept = calloc(sKeep + 1, sizeof(struct buffer));
	int head = 0, live = 0, i;
	for (i = 0; i < sCount; i++) {
		struct amdgpu_bo_alloc_request request;
		struct buffer buffer = {};
		double t0, t1;
		memset(&request, 0, sizeof(request));
		request.alloc_size = sSize;
		request.phys_alignment = 4096;
		request.preferred_heap = sDomain;
		request.flags = sFlags;
		t0 = now_ms();
		if (amdgpu_bo_alloc(sDevice, &request, &buffer.bo) != 0) {
			worker->failed++;
			continue;
		}
		t1 = now_ms();
		worker->alloc[worker->allocated++] = t1 - t0;
		if (sMapVA) {
			if (amdgpu_va_range_alloc(sDevice, 0, sSize, 4096, 0, &buffer.va,
					&buffer.va_handle, VA_RANGE_HIGH) != 0
				|| amdgpu_bo_va_op(buffer.bo, 0, sSize, buffer.va, 0,
					VA_OP_MAP) != 0) {
				worker->failed++;
			}
			worker->map[worker->mapped++] = now_ms() - t1;
		}
		if (sMapCPU) {
			double t2 = now_ms();
			if (amdgpu_bo_cpu_map(buffer.bo, &buffer.cpu) != 0) {
				buffer.cpu = NULL;
				worker->failed++;
			} else {
				double t3 = now_ms();
				worker->cpumap[worker->cpumapped++] = t3 - t2;
				if (sTouch) {
					uint64_t offset;
					for (offset = 0; offset < sSize; offset += 4096)
						((volatile char*)buffer.cpu)[offset] = 1;
					worker->touch[worker->touched++] = now_ms() - t3;
				}
			}
		}
		if (sKeep == 0) {
			release_buffer(worker, &buffer);
			continue;
		}
		if (live == sKeep) {
			release_buffer(worker, &kept[head]);
			head = (head + 1) % sKeep;
			live--;
		}
		kept[(head + live) % sKeep] = buffer;
		live++;
	}
	while (live > 0) {
		release_buffer(worker, &kept[head]);
		head = (head + 1) % sKeep;
		live--;
	}
	free(kept);
	return NULL;
}


static void
print_heaps(const char* when)
{
	struct amdgpu_heap_info vram, gtt;
	if (amdgpu_query_heap_info(sDevice, DOMAIN_VRAM, 0, &vram) != 0
		|| amdgpu_query_heap_info(sDevice, DOMAIN_GTT, 0, &gtt) != 0)
		return;
	printf("%s: VRAM %llu of %llu MiB used, GTT %llu of %llu MiB used\n", when,
		(unsigned long long)(vram.heap_usage >> 20),
		(unsigned long long)(vram.heap_size >> 20),
		(unsigned long long)(gtt.heap_usage >> 20),
		(unsigned long long)(gtt.heap_size >> 20));
}


/* page fault probe: fresh anonymous memory, one write per page */

#define PROBE_PAGES		4096
#define PROBE_MAX		2000000

struct probe {
	pthread_t thread;
	double* samples;
	int count;
	double worst_map;
};


static void*
probe_loop(void* data)
{
	struct probe* probe = data;
	while (!sProbeStop && probe->count + PROBE_PAGES <= PROBE_MAX) {
		double t0 = now_ms(), t1;
		char* memory = mmap(NULL, PROBE_PAGES * 4096,
			PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		int i;
		if (memory == MAP_FAILED)
			break;
		t1 = now_ms();
		if (t1 - t0 > probe->worst_map)
			probe->worst_map = t1 - t0;
		for (i = 0; i < PROBE_PAGES && !sProbeStop; i++) {
			double before = now_ms();
			memory[i * 4096] = 1;
			probe->samples[probe->count++] = now_ms() - before;
		}
		munmap(memory, PROBE_PAGES * 4096);
	}
	return NULL;
}


static void
probe_report(const char* name, struct probe* probe)
{
	int slow = 0, slower = 0, i;
	for (i = 0; i < probe->count; i++) {
		if (probe->samples[i] >= 0.1)
			slow++;
		if (probe->samples[i] >= 1)
			slower++;
	}
	printf("%s: %d faults, %d >= 0.1 ms, %d >= 1 ms, worst mmap %.3f ms\n",
		name, probe->count, slow, slower, probe->worst_map);
	report("fault", probe->samples, probe->count);
}


static void
probe_start(struct probe* probe)
{
	memset(probe, 0, sizeof(*probe));
	probe->samples = malloc(PROBE_MAX * sizeof(double));
	sProbeStop = 0;
	pthread_create(&probe->thread, NULL, probe_loop, probe);
}


static void
probe_stop(struct probe* probe)
{
	sProbeStop = 1;
	pthread_join(probe->thread, NULL);
}


int
main(int argc, char** argv)
{
	int threads = 1, probe = 0, option, fd, i;
	uint32_t major, minor;
	struct worker* workers;
	struct probe faults;
	double start, elapsed;
	const char* domain = "vram";

	while ((option = getopt(argc, argv, "s:d:wclt:n:k:vmTp")) != -1) {
		switch (option) {
			case 's':
				sSize = strtoull(optarg, NULL, 0) * 1024;
				break;
			case 'd':
				domain = optarg;
				if (strcmp(optarg, "gtt") == 0) {
					sDomain = DOMAIN_GTT;
					sFlags &= ~CREATE_NO_CPU_ACCESS;
				} else if (strcmp(optarg, "vramonly") == 0)
					sDomain = DOMAIN_VRAM;
				break;
			case 'w':
				sFlags |= CREATE_CPU_GTT_USWC;
				break;
			case 'c':
				sFlags |= CREATE_VRAM_CLEARED;
				break;
			case 'l':
				sFlags |= CREATE_VM_ALWAYS_VALID;
				break;
			case 'm':
				sMapCPU = 1;
				sFlags &= ~CREATE_NO_CPU_ACCESS;
				break;
			case 'T':
				sTouch = 1;
				break;
			case 't':
				threads = atoi(optarg);
				break;
			case 'n':
				sCount = atoi(optarg);
				break;
			case 'k':
				sKeep = atoi(optarg);
				break;
			case 'v':
				sMapVA = 1;
				break;
			case 'p':
				probe = 1;
				break;
			default:
				fprintf(stderr, "usage: %s [-s KiB] [-d vram|vramonly|gtt] "
					"[-w] [-c] [-l] [-t threads] [-n count] [-k keep] [-v] [-m] [-T] [-p]\n",
					argv[0]);
				return 1;
		}
	}

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) {
		perror("renderD128");
		return 1;
	}
	if (amdgpu_device_initialize(fd, &major, &minor, &sDevice) != 0) {
		fprintf(stderr, "amdgpu_device_initialize failed\n");
		return 1;
	}

	print_heaps("before");
	if (probe) {
		probe_start(&faults);
		usleep(1000000);
		probe_stop(&faults);
		probe_report("probe alone", &faults);
		free(faults.samples);
		probe_start(&faults);
	}

	workers = calloc(threads, sizeof(struct worker));
	start = now_ms();
	for (i = 0; i < threads; i++) {
		workers[i].alloc = malloc(sCount * sizeof(double));
		workers[i].map = malloc(sCount * sizeof(double));
		workers[i].cpumap = malloc(sCount * sizeof(double));
		workers[i].touch = malloc(sCount * sizeof(double));
		workers[i].cpuunmap = malloc(sCount * sizeof(double));
		workers[i].release = malloc(sCount * sizeof(double));
		pthread_create(&workers[i].thread, NULL, allocate_loop, &workers[i]);
	}
	for (i = 0; i < threads; i++)
		pthread_join(workers[i].thread, NULL);
	elapsed = now_ms() - start;
	if (probe)
		probe_stop(&faults);
	print_heaps("after");

	printf("%llu KiB %s flags %#llx, %d thread(s) x %d, keep %d%s: %.1f ms\n",
		(unsigned long long)(sSize / 1024), domain,
		(unsigned long long)sFlags, threads, sCount, sKeep,
		sMapVA ? ", GEM_VA" : "", elapsed);
	if (sMapCPU)
		printf("  CPU mapped%s\n", sTouch ? ", every page touched" : "");
	{
		double* all = malloc(threads * sCount * sizeof(double));
		int n, failed = 0;
#define GATHER(field, counter, label) \
		n = 0; \
		for (i = 0; i < threads; i++) { \
			memcpy(all + n, workers[i].field, \
				workers[i].counter * sizeof(double)); \
			n += workers[i].counter; \
		} \
		report(label, all, n);
		GATHER(alloc, allocated, "alloc")
		GATHER(map, mapped, "va")
		GATHER(cpumap, cpumapped, "cpumap")
		GATHER(touch, touched, "touch")
		GATHER(cpuunmap, cpuunmapped, "unmap")
		GATHER(release, released, "free")
		for (i = 0; i < threads; i++)
			failed += workers[i].failed;
		if (failed > 0)
			printf("  %d failed\n", failed);
		free(all);
	}
	if (probe)
		probe_report("probe during", &faults);

	amdgpu_device_deinitialize(sDevice);
	close(fd);
	return 0;
}
