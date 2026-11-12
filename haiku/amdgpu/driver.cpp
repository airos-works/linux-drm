/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Haiku driver around Linux's amdgpu: finds AMD GPUs on PCI, starts
	the DRM core and amdgpu (glue.c), publishes /dev/dri/cardN and
	/dev/dri/renderDN, and forwards open, ioctl, read and select to the DRM
	core. Buffer objects are mapped into teams by DRM_HAIKU_IOCTL_MAP.

	Settings (~/config/settings/kernel/drivers/amdgpu_drm): one Linux
	module parameter per line, "name value" (amdgpu's and drm's, e.g.
	"dc 1" or "debug 0x6"). A running driver takes one with
	DRM_HAIKU_IOCTL_SET_PARAM (tests/drmparam), where Linux reads it again. */

#include <new>
#include <stdio.h>
#include <string.h>

#include <driver_settings.h>
#include <Drivers.h>
#include <graphic_driver.h>
#include <KernelExport.h>
#include <PCI.h>
#include <OS.h>
#include <SupportDefs.h>
#include <errno.h>

#include <condition_variable.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/vm_priv.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>
#include <vm/VMTranslationMap.h>

#include <syscalls.h>

#include "glue.h"


// The kernel's shutdown hooks (shutdown_hooks.h), where it has them: the
// driver still loads on kernels without (and then cannot help a warm
// reboot of an APU).
extern "C" {
	typedef void (*shutdown_hook)(bool reboot, void* cookie);
	status_t register_shutdown_hook(shutdown_hook hook, void* cookie)
		__attribute__((weak));
	status_t unregister_shutdown_hook(shutdown_hook hook, void* cookie)
		__attribute__((weak));
}
// Linked against a kernel without them, the references would stay untyped
// (NOTYPE), and a kernel that has them refuses a symbol whose type differs
// from its own (FUNC): the driver would not load there at all.
__asm__(".type register_shutdown_hook, @function\n"
	".type unregister_shutdown_hook, @function");

// The device manager's power hooks (kdevice_manager.h), likewise: the
// kernel calls them when the system suspends and resumes.
extern "C" {
	typedef status_t (*device_manager_power_hook)(void* cookie, bool resume,
		int32 state);
	status_t device_manager_add_power_hook(device_manager_power_hook hook,
		void* cookie, const char* name) __attribute__((weak));
	status_t device_manager_add_power_hook_etc(device_manager_power_hook hook,
		void* cookie, const char* name, uint32 flags) __attribute__((weak));
	status_t device_manager_remove_power_hook(device_manager_power_hook hook,
		void* cookie) __attribute__((weak));
}
__asm__(".type device_manager_add_power_hook, @function\n"
	".type device_manager_add_power_hook_etc, @function\n"
	".type device_manager_remove_power_hook, @function");
// DEVICE_MANAGER_POWER_HOOK_DEEP_SLEEP: the GPU only sleeps in a deep sleep
static const uint32 kPowerHookDeepSleep = 0x01;


#define DRIVER_NAME "amdgpu_drm"
#define TRACE(x...) dprintf(DRIVER_NAME ": " x)

// the synced log file (log_file), and the syslog
extern "C" void lx_haiku_log(const char* text);
extern "C" void lx_trace_calls_start_stop(void);
extern "C" int64 lx_haiku_log_position(void);
extern "C" int lx_haiku_raw_log_prepare(void);
extern "C" void lx_haiku_raw_log_start(int32 thread);
extern "C" void lx_haiku_raw_log_end(void);

#ifdef AMDGPU_DRM_ARMED_ONLY
// lab builds: steps that may stop the machine go to the synced log file too
extern "C" void lx_trace_calls_start(void);
#	define LAB_LOG(x...) \
	do { \
		char _line[200]; \
		snprintf(_line, sizeof(_line), DRIVER_NAME ": " x); \
		lx_haiku_log(_line); \
	} while (0)
#else
#	define LAB_LOG(x...) do { } while (0)
#	define lx_trace_calls_start() do { } while (0)
#endif

// Linux's DRM ioctl numbers (asm-generic/ioctl.h) for the Haiku additions
#define LX_IOC(dir, type, nr, size) \
	(((uint32)(dir) << 30) | ((uint32)(size) << 16) | ((type) << 8) | (nr))
struct drm_haiku_map {
	uint64	offset;
	uint64	size;
	uint32	flags;
	int32	area;
	uint64	address;
};
struct drm_haiku_unmap {
	uint64	address;
};
static const uint32 kDrmHaikuMap = LX_IOC(3, 'd', 0xf0,
	sizeof(drm_haiku_map));
static const uint32 kDrmHaikuUnmap = LX_IOC(1, 'd', 0xf1,
	sizeof(drm_haiku_unmap));
struct drm_haiku_device_info {
	uint32	minor;
	uint32	node_type;
	uint16	domain;
	uint8	bus;
	uint8	dev;
	uint8	func;
	uint8	revision_id;
	uint16	vendor_id;
	uint16	device_id;
	uint16	subvendor_id;
	uint16	subdevice_id;
	uint32	class_code;
	uint32	file_id[2];
	uint32	reserved[2];
};
#ifdef AMDGPU_DRM_ARMED_ONLY
// lab builds: closes the driver's own device file, so that devfs can unload
// it (to test the teardown: then touch the binary)
static const uint32 kDrmHaikuLabUnpin = LX_IOC(0, 'd', 0xff, 0);
// and measures the CPU's bandwidth to VRAM through a kernel mapping
static const uint32 kDrmHaikuLabBench = LX_IOC(0, 'd', 0xfe, 0);
// and forces a DPM performance level (argument: the level, by value)
static const uint32 kDrmHaikuLabPerfLevel = LX_IOC(0, 'd', 0xfd, 0);
static void lab_unpin();
#endif
struct drm_haiku_wait_hotplug {
	uint32	generation;
	uint32	timeout_ms;
};
static const uint32 kDrmHaikuWaitHotplug = LX_IOC(3, 'd', 0xf3,
	sizeof(drm_haiku_wait_hotplug));
static const uint32 kDrmHaikuDeviceInfo = LX_IOC(2, 'd', 0xf2,
	sizeof(drm_haiku_device_info));
struct drm_haiku_force_connector {
	uint32	connector_id;
	uint32	force;		// 0 detect, 1 off, 2 on
};
static const uint32 kDrmHaikuForceConnector = LX_IOC(1, 'd', 0xf4,
	sizeof(drm_haiku_force_connector));
struct drm_haiku_set_param {
	char	name[64];
	char	value[64];
};
static const uint32 kDrmHaikuSetParam = LX_IOC(1, 'd', 0xf5,
	sizeof(drm_haiku_set_param));
struct drm_haiku_backlight {
	int32	brightness;		// in: the new level, or -1; out: the level
	uint32	max_brightness;	// out
};
static const uint32 kDrmHaikuBacklight = LX_IOC(3, 'd', 0xf6,
	sizeof(drm_haiku_backlight));
struct drm_haiku_crc {
	uint32	crtc_index;		// in
	uint32	op;				// in: 0 stop, 1 start, 2 read
	uint32	frame;			// out: the vblank count
	uint32	crc[3];			// out
	uint16	window[4];		// in, op 1: x0, y0, x1, y1; all zero: all
};
static const uint32 kDrmHaikuCrc = LX_IOC(3, 'd', 0xf7,
	sizeof(drm_haiku_crc));
// the screen's frame buffer: lx_glue_scanout_args (glue.h) is its layout
static const uint32 kDrmHaikuScanout = LX_IOC(3, 'd', 0xf8,
	sizeof(lx_glue_scanout_args));


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const int kMaxDevices = 16;
static char* sDeviceNames[kMaxDevices + 1];
static int sDeviceMinors[kMaxDevices];
static int sDeviceCount;
static bool sStarted;


// #pragma mark - errors


static status_t
haiku_status(long linuxError)
{
	if (linuxError >= 0)
		return B_OK;
	switch (-linuxError) {
		case 1: return B_NOT_ALLOWED;			// EPERM
		case 2: return B_ENTRY_NOT_FOUND;		// ENOENT
		case 4: return B_INTERRUPTED;			// EINTR
		case 5: return B_IO_ERROR;				// EIO
		case 6: return ENXIO;
		case 7: return E2BIG;
		case 9: return B_FILE_ERROR;			// EBADF
		case 11: return B_WOULD_BLOCK;			// EAGAIN
		case 12: return B_NO_MEMORY;			// ENOMEM
		case 13: return B_PERMISSION_DENIED;	// EACCES
		case 14: return B_BAD_ADDRESS;			// EFAULT
		case 16: return B_BUSY;					// EBUSY
		case 17: return B_FILE_EXISTS;			// EEXIST
		case 19: return ENODEV;
		case 22: return B_BAD_VALUE;			// EINVAL
		case 24: return EMFILE;
		case 25: return ENOTTY;
		case 28: return B_DEVICE_FULL;			// ENOSPC
		case 34: return ERANGE;
		case 35: return EDEADLK;
		case 38: return ENOSYS;
		case 61: return ENODATA;
		case 62: return ETIME;
		case 71: return EPROTO;
		case 75: return EOVERFLOW;
		case 95: return EOPNOTSUPP;
		case 110: return B_TIMED_OUT;			// ETIMEDOUT
		case 114: return EALREADY;
		case 125: return ECANCELED;
		case 512: return B_INTERRUPTED;			// ERESTARTSYS
		case 524: return B_NOT_SUPPORTED;		// ENOTSUPP
	}
	return B_ERROR;
}


// #pragma mark - buffer mappings


/*!	The cache of a buffer mapped into teams (DRM_HAIKU_IOCTL_MAP). It has no
	pages of its own: Fault() runs the buffer's Linux fault handler and maps
	the page frames it finds into the area that faulted, and when TTM moves
	the buffer, Invalidate() unmaps them from every area again, so that the
	next access finds the buffer at its new place - what Linux's
	unmap_mapping_range() does for a mmap()ed buffer. Areas cloned from the
	area, or inherited by fork(), share the cache (B_SHARED_AREA).

	Locking: the VM calls Fault() with the address space read locked and the
	cache locked. The Linux fault handler waits for the buffer's
	reservation, which TTM holds while it calls Invalidate(), which locks the
	cache; so the handler runs with the cache unlocked, and the generation
	tells afterwards whether the buffer moved meanwhile.
*/
class BufferCache final : public VMCache {
public:
								BufferCache();

			status_t			Init(page_num_t pageCount);
			void				SetMapping(void* mapping);
			void				SetMappedUpTo(page_num_t end);

	virtual	status_t			Fault(VMAddressSpace* addressSpace,
									off_t offset);
			status_t			FaultIn(VMAddressSpace* addressSpace,
									page_num_t page, page_num_t end);

			void				Invalidate(page_num_t first, page_num_t count);
			void*				Detach();

protected:
	virtual	void				DeleteObject();

private:
			void				_Unmap(page_num_t first, page_num_t count);

private:
			void*				fMapping;
				// the glue's mapping; NULL once detached
			page_num_t			fPageCount;
			uint32				fGeneration;
				// changes whenever pages are unmapped
			int32				fFaulting;
			ConditionVariable	fFaultsDone;
			page_num_t			fFaultEnd;
				// where the pages the last fault mapped end
			page_num_t			fFaultWindow;
				// how many pages the last fault mapped
};


static const page_num_t kFaultPages = 64;
	// pages mapped at once: the one touched and the ones after it, and
	// pages the glue looks up at a time
static const page_num_t kMaxFaultPages = 512;
	// ... up to as many when a team goes through a buffer in order


static uint32
memory_type(int caching)
{
	switch (caching) {
		case 0:
			return B_UNCACHED_MEMORY;
		case 1:
			return B_WRITE_COMBINING_MEMORY;
		default:
			return B_WRITE_BACK_MEMORY;
	}
}


BufferCache::BufferCache()
	:
	fMapping(NULL),
	fPageCount(0),
	fGeneration(0),
	fFaulting(0),
	fFaultEnd(0),
	fFaultWindow(kFaultPages)
{
}


status_t
BufferCache::Init(page_num_t pageCount)
{
	status_t status = VMCache::Init("drm buffer", CACHE_TYPE_DEVICE, 0);
	if (status != B_OK)
		return status;
	fPageCount = pageCount;
	virtual_end = (off_t)pageCount * B_PAGE_SIZE;
	fFaultsDone.Init(this, "drm buffer faults");
	return B_OK;
}


void
BufferCache::SetMapping(void* mapping)
{
	AutoLocker<VMCache> locker(this);
	fMapping = mapping;
}


/*!	The pages [0, end) were mapped when the area was made: a fault at \a end
	continues them (see Fault()).
*/
void
BufferCache::SetMappedUpTo(page_num_t end)
{
	AutoLocker<VMCache> locker(this);
	fFaultEnd = end;
	fFaultWindow = kFaultPages;
}


status_t
BufferCache::Fault(VMAddressSpace* addressSpace, off_t offset)
{
	page_num_t page = offset / B_PAGE_SIZE;
	if (page >= fPageCount)
		return B_BAD_ADDRESS;

	// The page touched and the ones after it; twice as many as last time
	// when the team goes on where the last fault's pages end (a buffer
	// written or read from start to end), up to kMaxFaultPages.
	page_num_t window = kFaultPages;
	if (page == fFaultEnd)
		window = min_c(fFaultWindow * 2, kMaxFaultPages);
	page_num_t end = min_c(page + window, fPageCount);
	fFaultWindow = window;
	fFaultEnd = end;

	Unlock();
	status_t status = FaultIn(addressSpace, page, end);
	Lock();
	return status;
}


/*!	Maps the pages [page, end) of the buffer into the team's areas of the
	cache, as far as they are in them. The address space must be read locked
	(its areas then stay), the cache must not be locked.
*/
status_t
BufferCache::FaultIn(VMAddressSpace* addressSpace, page_num_t page,
	page_num_t end)
{
	VMTranslationMap* map = addressSpace->TranslationMap();
	int retries = 0;
	while (page < end) {
		VMArea* targets[4];
		int targetCount = 0;
		Lock();
		void* mapping = fMapping;
		uint32 generation = fGeneration;
		for (VMArea* area = areas.First(); area != NULL && targetCount < 4;
				area = areas.GetNext(area)) {
			if (area->address_space == addressSpace
				&& (off_t)page * B_PAGE_SIZE >= area->cache_offset
				&& (off_t)page * B_PAGE_SIZE
					< area->cache_offset + (off_t)area->Size()) {
				targets[targetCount++] = area;
			}
		}
		if (mapping == NULL || targetCount == 0) {
			Unlock();
			return B_BAD_ADDRESS;
		}
		fFaulting++;
		Unlock();

		unsigned long pfns[kFaultPages];
		int caching;
		long count = lx_glue_map_fault(mapping, page, pfns,
			min_c(end - page, kFaultPages), &caching);

		// page tables for them, while nothing is locked
		size_t reservePages = 0;
		for (int i = 0; i < targetCount && count > 0; i++) {
			VMArea* area = targets[i];
			addr_t first = area->Base() + page * B_PAGE_SIZE
				- area->cache_offset;
			addr_t last = min_c(first + count * B_PAGE_SIZE,
				area->Base() + area->Size()) - 1;
			reservePages += map->MaxPagesNeededToMap(first, last);
		}
		vm_page_reservation reservation;
		vm_page_reserve_pages(&reservation, reservePages, VM_PRIORITY_USER);

		Lock();
		if (--fFaulting == 0 && fMapping == NULL)
			fFaultsDone.NotifyAll();
		bool current = generation == fGeneration;
		if (count > 0 && current) {
			uint32 type = memory_type(caching);
			map->Lock();
			for (int i = 0; i < targetCount; i++) {
				VMArea* area = targets[i];
				// fork() doesn't copy the type, and a "device" area's
				// pages are unmapped without page bookkeeping. (The area's
				// memory type stays the default: the pages have theirs,
				// and deleting an area with one looks for a range that
				// arch_vm_set_memory_type() would have registered.)
				area->cache_type = CACHE_TYPE_DEVICE;
				for (long index = 0; index < count; index++) {
					off_t offset = (off_t)(page + index) * B_PAGE_SIZE;
					if (offset >= area->cache_offset + (off_t)area->Size())
						break;
					addr_t address = area->Base() + offset - area->cache_offset;
					phys_addr_t physical;
					uint32 flags;
					if (map->Query(address, &physical, &flags) == B_OK
						&& (flags & PAGE_PRESENT) != 0) {
						continue;
					}
					map->Map(address, (phys_addr_t)pfns[index] * B_PAGE_SIZE,
						area->protection, type, &reservation);
				}
			}
			map->Unlock();
		}
		Unlock();
		vm_page_unreserve_pages(&reservation);

		if (count < 0)
			return B_BAD_ADDRESS;
		if (count == 0) {
			// a signal interrupted a wait: the access faults again
			snooze(1000);
			return B_OK;
		}
		if (!current && ++retries < 3) {
			// the buffer moved meanwhile (the fault handler may have moved
			// it itself, to where the CPU can reach it)
			continue;
		}
		page += count;
		retries = 0;
	}
	return B_OK;
}


/*!	The buffer moves: unmaps the pages [first, first + count) from all areas.
	Called by TTM through unmap_mapping_range(), with the buffer reserved.
*/
void
BufferCache::Invalidate(page_num_t first, page_num_t count)
{
	AutoLocker<VMCache> locker(this);
	fGeneration++;
	_Unmap(first, count);
}


/*!	Ends the faults: unmaps everything, waits for faults in progress and
	returns the glue's mapping for lx_glue_unmap(). Areas left then fault
	with B_BAD_ADDRESS.
*/
void*
BufferCache::Detach()
{
	Lock();
	void* mapping = fMapping;
	fMapping = NULL;
	fGeneration++;
	_Unmap(0, fPageCount);
	while (fFaulting > 0) {
		ConditionVariableEntry entry;
		fFaultsDone.Add(&entry);
		Unlock();
		entry.Wait();
		Lock();
	}
	Unlock();
	return mapping;
}


void
BufferCache::DeleteObject()
{
	delete this;
}


/*!	The cache must be locked. Its areas stay in their address spaces while
	they are in the cache's list (delete_area() removes them from the list
	first).
*/
void
BufferCache::_Unmap(page_num_t first, page_num_t count)
{
	off_t start = (off_t)first * B_PAGE_SIZE;
	off_t end = (off_t)(first + count) * B_PAGE_SIZE;
	for (VMArea* area = areas.First(); area != NULL;
			area = areas.GetNext(area)) {
		off_t from = max_c(start, area->cache_offset);
		off_t to = min_c(end, area->cache_offset + (off_t)area->Size());
		if (from >= to)
			continue;
		VMTranslationMap* map = area->address_space->TranslationMap();
		map->Lock();
		map->Unmap(area->Base() + (from - area->cache_offset),
			area->Base() + (to - area->cache_offset) - 1);
		map->Unlock();
	}
}


static void
invalidate_buffer(void* owner, unsigned long first, unsigned long count)
{
	((BufferCache*)owner)->Invalidate(first, count);
}


struct BufferMapping : DoublyLinkedListLinkImpl<BufferMapping> {
	team_id		team;
	area_id		area;
	addr_t		address;
	BufferCache* cache;
};

typedef DoublyLinkedList<BufferMapping> MappingList;


struct SelectState : DoublyLinkedListLinkImpl<SelectState> {
	selectsync*	sync;
	uint8		event;
	void*		glueSelect;
};

typedef DoublyLinkedList<SelectState> SelectList;


struct DeviceCookie {
	void*		file;
	int			minor;
	mutex		lock;
	MappingList	mappings;
	SelectList	selects;
};


/*!	Ends the faults of a cache and releases the reference the mapping has.
*/
static void
release_cache(BufferCache* cache)
{
	void* mapping = cache->Detach();
	if (mapping != NULL)
		lx_glue_unmap(mapping);
	cache->ReleaseRef();
}


static status_t
map_buffer(DeviceCookie* cookie, drm_haiku_map* request)
{
	if (request->size == 0 || request->size % B_PAGE_SIZE != 0)
		return B_BAD_VALUE;
	page_num_t pageCount = request->size / B_PAGE_SIZE;

	BufferCache* cache = new(std::nothrow) BufferCache;
	if (cache == NULL)
		return B_NO_MEMORY;
	status_t status = cache->Init(pageCount);
	if (status != B_OK) {
		cache->Delete();
		return status;
	}
	void* mapping = lx_glue_map(cookie->file, request->offset, request->size,
		(request->flags & 1) != 0, invalidate_buffer, cache);
	if (int error = lx_glue_is_error(mapping)) {
		cache->ReleaseRef();
		return haiku_status(error);
	}
	cache->SetMapping(mapping);

	uint32 protection = B_READ_AREA | B_KERNEL_READ_AREA | B_SHARED_AREA
		| ((request->flags & 1) ? B_WRITE_AREA | B_KERNEL_WRITE_AREA : 0)
		| ((request->flags & 2) ? B_CLONEABLE_AREA : 0);
	team_id team = team_get_current_team_id();
	VMAddressSpace* addressSpace = VMAddressSpace::Get(team);
	if (addressSpace == NULL) {
		release_cache(cache);
		return B_BAD_TEAM_ID;
	}

	void* address = (void*)(addr_t)request->address;
	virtual_address_restrictions restrictions = {};
	restrictions.address = address;
	restrictions.address_specification
		= address != NULL ? B_EXACT_ADDRESS : B_ANY_ADDRESS;
	VMArea* area;
	addressSpace->WriteLock();
	cache->Lock();
	status = vm_map_cache(addressSpace, cache, 0, "drm buffer", request->size,
		B_NO_LOCK, protection, 0, REGION_NO_PRIVATE_MAP,
		CREATE_AREA_DONT_COMMIT_MEMORY, &restrictions, true, &area, &address);
	area_id areaID = -1;
	if (status == B_OK) {
		// the area has the cache's first reference, the record another
		area->cache_type = CACHE_TYPE_DEVICE;
		areaID = area->id;
		cache->AcquireRefLocked();
	}
	cache->Unlock();
	addressSpace->WriteUnlock();
	if (status != B_OK) {
		addressSpace->Put();
		release_cache(cache);
		return status;
	}

	// Map the start of the buffer now, the rest when the team first touches
	// it (Fault()); what moves later faults in again. Mapping the start runs
	// the buffer's fault handler, which moves a buffer the CPU cannot reach
	// to where it can or fails: here, as before, not at the team's first
	// access. Not while the GPU still clears or moves the buffer, though:
	// that is not waited for (see lx_glue_map_prefault()).
	page_num_t prefault = min_c(
		(page_num_t)lx_glue_map_prefault(mapping, pageCount), pageCount);
	cache->SetMappedUpTo(prefault);
	addressSpace->ReadLock();
	if (prefault > 0 && addressSpace->LookupArea((addr_t)address) == area
		&& area->id == areaID) {
		status = cache->FaultIn(addressSpace, 0, prefault);
	}
	addressSpace->ReadUnlock();
	addressSpace->Put();
	if (status != B_OK) {
		vm_delete_area(team, areaID, true);
		release_cache(cache);
		return status;
	}

	BufferMapping* record = new(std::nothrow) BufferMapping;
	if (record == NULL) {
		vm_delete_area(team, areaID, true);
		release_cache(cache);
		return B_NO_MEMORY;
	}
	record->team = team;
	record->area = areaID;
	record->address = (addr_t)address;
	record->cache = cache;
	{
		MutexLocker locker(cookie->lock);
		cookie->mappings.Add(record);
	}

	LAB_LOG("map 0x%" B_PRIx64 " size 0x%" B_PRIx64 ": area %" B_PRId32
		" at 0x%lx\n", request->offset, request->size, areaID,
		(addr_t)address);
	request->area = areaID;
	request->address = (addr_t)address;
	return B_OK;
}


static void
remove_mapping(BufferMapping* record)
{
	// the team may have deleted the area itself already
	area_info info;
	if (get_area_info(record->area, &info) == B_OK
		&& info.team == record->team
		&& (addr_t)info.address == record->address)
		vm_delete_area(record->team, record->area, true);
	release_cache(record->cache);
	delete record;
}


static status_t
unmap_buffer(DeviceCookie* cookie, addr_t address)
{
	MutexLocker locker(cookie->lock);
	team_id team = team_get_current_team_id();
	for (MappingList::Iterator it = cookie->mappings.GetIterator();
			BufferMapping* record = it.Next();) {
		if (record->team != team || record->address != address)
			continue;
		it.Remove();
		locker.Unlock();
		lx_trace_calls_start();
		LAB_LOG("unmap 0x%lx: area %" B_PRId32 "\n", address, record->area);
		remove_mapping(record);
		LAB_LOG("unmap 0x%lx: done\n", address);
		return B_OK;
	}
	return B_BAD_VALUE;
}


static void
unmap_all(DeviceCookie* cookie)
{
	MutexLocker locker(cookie->lock);
	while (BufferMapping* record = cookie->mappings.RemoveHead())
		remove_mapping(record);
}


// #pragma mark - device hooks


static int
minor_of(const char* name)
{
	for (int i = 0; i < sDeviceCount; i++) {
		if (strcmp(sDeviceNames[i], name) == 0)
			return sDeviceMinors[i];
	}
	return -1;
}


static status_t
device_open(const char* name, uint32 flags, void** _cookie)
{
	int minor = minor_of(name);
	if (minor < 0)
		return B_ENTRY_NOT_FOUND;

	DeviceCookie* cookie = new(std::nothrow) DeviceCookie;
	if (cookie == NULL)
		return B_NO_MEMORY;
	mutex_init(&cookie->lock, "amdgpu file");
	cookie->minor = minor;

	int error;
	cookie->file = lx_glue_open(minor, flags & O_ACCMODE, &error);
	if (cookie->file == NULL) {
		mutex_destroy(&cookie->lock);
		delete cookie;
		return haiku_status(error);
	}
	*_cookie = cookie;
	return B_OK;
}


static status_t
device_close(void* _cookie)
{
	return B_OK;
}


static status_t
device_free(void* _cookie)
{
	DeviceCookie* cookie = (DeviceCookie*)_cookie;
	LAB_LOG("free minor %d: mappings\n", cookie->minor);
	unmap_all(cookie);
	LAB_LOG("free minor %d: close\n", cookie->minor);
	lx_glue_close(cookie->file);
	LAB_LOG("free minor %d: done\n", cookie->minor);
	mutex_destroy(&cookie->lock);
	delete cookie;
	return B_OK;
}


// While the GPUs sleep (suspend to idle), DRM calls wait at the door; the
// ones under way get a while to finish before the GPUs go to sleep. (Linux
// freezes every task first; Haiku's keep running.)
static mutex sSleepLock = MUTEX_INITIALIZER("amdgpu_drm sleep");
static ConditionVariable sSleepCondition;
static bool sSleeping;
static int32 sCallsUnderway;


static void
enter_call()
{
	MutexLocker locker(sSleepLock);
	while (sSleeping) {
		ConditionVariableEntry entry;
		sSleepCondition.Add(&entry);
		locker.Unlock();
		entry.Wait();
		locker.Lock();
	}
	sCallsUnderway++;
}


static void
leave_call()
{
	MutexLocker locker(sSleepLock);
	if (--sCallsUnderway == 0 && sSleeping)
		sSleepCondition.NotifyAll();
}


static void
close_door()
{
	MutexLocker locker(sSleepLock);
	sSleeping = true;

	bigtime_t end = system_time() + 2000000;
	while (sCallsUnderway > 0 && system_time() < end) {
		ConditionVariableEntry entry;
		sSleepCondition.Add(&entry);
		locker.Unlock();
		entry.Wait(B_ABSOLUTE_TIMEOUT, end);
		locker.Lock();
	}
	if (sCallsUnderway > 0)
		TRACE("sleeping with %" B_PRId32 " calls under way\n", sCallsUnderway);
}


static void
open_door()
{
	MutexLocker locker(sSleepLock);
	sSleeping = false;
	sSleepCondition.NotifyAll();
}


static bool sGPUsSleeping;


/*!	Suspend to idle (state 0, ACPI_POWER_STATE_ON) puts the APUs' GPUs to
	sleep, as Linux does for S0ix; see lx_glue_suspend(). */
extern "C" void lx_haiku_suspend_trace(bool enabled);


static status_t
power_hook(void* cookie, bool resume, int32 state)
{
	if (!resume) {
		if (state != 0)
			return B_OK;

		close_door();
		int result = lx_glue_suspend();
		if (result <= 0) {
			open_door();
			return haiku_status(result);
		}
		sGPUsSleeping = true;
		return B_OK;
	}

	if (sGPUsSleeping) {
		lx_haiku_suspend_trace(true);
		lx_glue_resume();
		lx_haiku_suspend_trace(false);
		sGPUsSleeping = false;
		open_door();
	}
	return B_OK;
}


static status_t
device_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	DeviceCookie* cookie = (DeviceCookie*)_cookie;

	if (op == kDrmHaikuMap) {
		drm_haiku_map request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		status_t status = map_buffer(cookie, &request);
		if (status != B_OK)
			return status;
		if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}
	if (op == kDrmHaikuUnmap) {
		drm_haiku_unmap request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return unmap_buffer(cookie, request.address);
	}

	if (op == kDrmHaikuWaitHotplug) {
		drm_haiku_wait_hotplug request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		unsigned int generation = request.generation;
		int error = lx_glue_wait_hotplug(&generation, request.timeout_ms);
		request.generation = generation;
		if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return haiku_status(error);
	}

	if (op == kDrmHaikuForceConnector) {
		// monitors are the primary node's
		if (cookie->minor >= 128)
			return B_NOT_ALLOWED;
		drm_haiku_force_connector request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return haiku_status(lx_glue_force_connector(cookie->file,
			request.connector_id, request.force));
	}

	if (op == kDrmHaikuSetParam) {
		// what writing /sys/module/<module>/parameters/<name> does: takes
		// effect where the value is looked at again (drm's "debug" is)
		if (cookie->minor >= 128)
			return B_NOT_ALLOWED;
		drm_haiku_set_param request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		request.name[sizeof(request.name) - 1] = '\0';
		request.value[sizeof(request.value) - 1] = '\0';
		return haiku_status(lx_glue_set_param(request.name, request.value));
	}

	if (op == kDrmHaikuBacklight) {
		// the panel's, which the display (primary) node owns
		if (cookie->minor >= 128)
			return B_NOT_ALLOWED;
		drm_haiku_backlight request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		int current, maximum;
		int error = lx_glue_backlight(cookie->file, request.brightness,
			&current, &maximum);
		if (error != 0)
			return haiku_status(error);
		request.brightness = current;
		request.max_brightness = maximum;
		if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}

	if (op == kDrmHaikuCrc) {
		// the frames a CRTC scans out, which the display (primary) node owns
		if (cookie->minor >= 128)
			return B_NOT_ALLOWED;
		drm_haiku_crc request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		int error = lx_glue_crc(cookie->file, request.crtc_index,
			request.op, request.crc, &request.frame, request.window);
		if (error != 0)
			return haiku_status(error);
		if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}

	if (op == kDrmHaikuScanout) {
		// publishing is the display (primary) node's, getting anyone's;
		// glue.c checks
		lx_glue_scanout_args request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		int error = lx_glue_scanout(cookie->file, &request);
		if (error != 0)
			return haiku_status(error);
		if (user_memcpy(buffer, &request, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}

	if (op == kDrmHaikuDeviceInfo) {
		drm_haiku_device_info info = {};
		int bus, device, function;
		if (lx_glue_minor_location(cookie->minor, &bus, &device, &function)
				!= 0)
			return ENODEV;
		info.minor = cookie->minor;
		info.node_type = cookie->minor >= 128 ? 2 : 0;
		// one cookie per open file: dup()ed descriptors share it
		info.file_id[0] = (uint32)(addr_t)cookie;
		info.file_id[1] = (uint32)((uint64)(addr_t)cookie >> 32);
		info.bus = bus;
		info.dev = device;
		info.func = function;
		pci_module_info* pci;
		if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) == B_OK) {
			pci_info pciInfo;
			for (int32 i = 0; pci->get_nth_pci_info(i, &pciInfo) == B_OK; i++) {
				if (pciInfo.bus != bus || pciInfo.device != device
					|| pciInfo.function != function)
					continue;
				info.vendor_id = pciInfo.vendor_id;
				info.device_id = pciInfo.device_id;
				info.revision_id = pciInfo.revision;
				info.class_code = (pciInfo.class_base << 16)
					| (pciInfo.class_sub << 8) | pciInfo.class_api;
				info.subvendor_id = pciInfo.u.h0.subsystem_vendor_id;
				info.subdevice_id = pciInfo.u.h0.subsystem_id;
				break;
			}
			put_module(B_PCI_MODULE_NAME);
		}
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(buffer, &info, sizeof(info)) != B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}

#ifdef AMDGPU_DRM_ARMED_ONLY
	if (op == kDrmHaikuLabUnpin) {
		lab_unpin();
		return B_OK;
	}
	if (op == kDrmHaikuLabBench)
		return haiku_status(lx_glue_lab_bench(cookie->file));
	if (op == kDrmHaikuLabPerfLevel) {
		return haiku_status(lx_glue_lab_perf_level(cookie->file,
			(unsigned int)(addr_t)buffer));
	}
#endif

	if (op == B_GET_ACCELERANT_SIGNATURE) {
		static const char kSignature[] = "amdgpu_kms.accelerant";
		if (!IS_USER_ADDRESS(buffer)
			|| user_strlcpy((char*)buffer, kSignature, length) < B_OK)
			return B_BAD_ADDRESS;
		return B_OK;
	}

	// everything of type 'd' is a DRM ioctl
	if (((op >> 8) & 0xff) == 'd') {
		enter_call();
		long result = lx_glue_ioctl(cookie->file, op, buffer);
		leave_call();
		return haiku_status(result);
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
device_read(void* _cookie, off_t position, void* buffer, size_t* _length)
{
	DeviceCookie* cookie = (DeviceCookie*)_cookie;
	long result = lx_glue_read(cookie->file, buffer, *_length);
	if (result < 0) {
		*_length = 0;
		return haiku_status(result);
	}
	*_length = result;
	return B_OK;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static void
select_notify(void* _state, unsigned int events)
{
	SelectState* state = (SelectState*)_state;
	// POLLIN 0x1, POLLRDNORM 0x40
	if (state->event == B_SELECT_READ && (events & (0x1 | 0x40)) != 0)
		notify_select_event(state->sync, state->event);
}


static status_t
device_select(void* _cookie, uint8 event, uint32 ref, selectsync* sync)
{
	DeviceCookie* cookie = (DeviceCookie*)_cookie;
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	SelectState* state = new(std::nothrow) SelectState;
	if (state == NULL)
		return B_NO_MEMORY;
	state->sync = sync;
	state->event = event;

	unsigned int ready = 0;
	state->glueSelect = lx_glue_select(cookie->file, select_notify, state,
		&ready);
	if (state->glueSelect == NULL) {
		delete state;
		return B_NO_MEMORY;
	}
	// the selectsync identifies the state when deselecting
	MutexLocker locker(cookie->lock);
	cookie->selects.Add(state);
	locker.Unlock();

	if ((ready & (0x1 | 0x40)) != 0)
		notify_select_event(sync, event);
	return B_OK;
}


static status_t
device_deselect(void* _cookie, uint8 event, selectsync* sync)
{
	DeviceCookie* cookie = (DeviceCookie*)_cookie;
	MutexLocker locker(cookie->lock);
	SelectState* state = NULL;
	for (SelectList::Iterator it = cookie->selects.GetIterator();
			SelectState* candidate = it.Next();) {
		if (candidate->sync == sync && candidate->event == event) {
			it.Remove();
			state = candidate;
			break;
		}
	}
	locker.Unlock();
	if (state == NULL)
		return B_BAD_VALUE;
	lx_glue_deselect(state->glueSelect);
	delete state;
	return B_OK;
}


static device_hooks sDeviceHooks = {
	device_open,
	device_close,
	device_free,
	device_control,
	device_read,
	device_write,
	device_select,
	device_deselect,
	NULL,
	NULL
};


// #pragma mark - driver


static int sSelfTest;


/* 0: none, 1: test the compat layer and stop, 2: also run a fake DRM device */
static int
self_test_requested()
{
	void* handle = load_driver_settings(DRIVER_NAME);
	if (handle == NULL)
		return 0;
	const char* value = get_driver_parameter(handle, "haiku_selftest", "0",
		"1");
	int selfTest = strtol(value, NULL, 10);
	unload_driver_settings(handle);
	return selfTest;
}


static void
apply_settings()
{
	void* handle = load_driver_settings(DRIVER_NAME);
	if (handle == NULL)
		return;
	const driver_settings* settings = get_driver_settings(handle);
	for (int i = 0; settings != NULL && i < settings->parameter_count; i++) {
		const driver_parameter& parameter = settings->parameters[i];
		if (parameter.value_count > 0
			&& strcmp(parameter.name, "haiku_selftest") != 0)
			lx_glue_set_param(parameter.name, parameter.values[0]);
	}
	unload_driver_settings(handle);
}


static bool
is_amd_gpu(const pci_info& info)
{
	return info.vendor_id == 0x1002 && info.class_base == PCI_display;
}


/*	Makes the marker and arm file changes durable. init_driver() and
	uninit_driver() run in devfs' driver lock, and uninit_driver() waits for
	the stable timer's thread. A sync waits for every volume, network ones
	(userlandfs: smbfs) too, and their mounting may wait for devfs (net_server
	opening a network device); the X399 stopped in such a sync when devfs
	reloaded the driver early in a boot. So the sync runs in a thread of its
	own that is waited for a few seconds at most, and that runs kernel code
	only (_kern_sync() itself, which ignores the argument): this add-on may
	be unloaded before it ends. The boot volume is synced before any network
	volume, and BFS writes its log after two idle seconds anyway. */
static void
sync_volumes()
{
	thread_id thread = spawn_kernel_thread((thread_func)(void*)&_kern_sync,
		DRIVER_NAME " sync", B_NORMAL_PRIORITY, NULL);
	if (thread < 0) {
		TRACE("cannot sync: %s\n", strerror(thread));
		return;
	}
	resume_thread(thread);
	status_t result;
	if (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 3000000, &result)
			== B_TIMED_OUT) {
		lx_haiku_log(DRIVER_NAME ": the sync takes long (a network volume?); "
			"not waiting for it\n");
	}
}


/*	Lab builds (AMDGPU_DRM_ARMED_ONLY) start only when the arm file was made
	during this boot: devfs may reload the driver any number of times in the
	boot it was armed in, but a start that brought the machine down cannot
	repeat on the next boot, when nobody may be there to pick another boot
	option. A stale arm file is deleted. */
#ifdef AMDGPU_DRM_ARMED_ONLY
static const char* kArmFile
	= "/boot/home/config/settings/kernel/drivers/amdgpu_drm.arm";

static bool
armed_for_this_boot()
{
	struct stat st;
	if (stat(kArmFile, &st) != 0)
		return false;
	time_t bootTime = real_time_clock() - system_time() / 1000000;
	if (st.st_mtime + 5 >= bootTime)
		return true;
	_kern_unlink(-1, kArmFile);
	sync_volumes();
	return false;
}
#endif


#ifndef AMDGPU_DRM_ARMED_ONLY
/*	A start that brings the machine down must not repeat at every boot: there
	may be nobody at the keyboard to pick another boot option (the lab X399
	has none that works). A marker file exists from the start until the GPU
	has run for half a minute; finding it means that the last start did not
	get that far, and the driver stays off (the framebuffer driver drives the
	display) until the marker is deleted.
	One such start is forgiven, though: a boot can end within the half
	minute without the driver's doing (the power button, a power cut, a
	quick test reboot), and on machines whose firmware frame buffer shows
	nothing after the boot splash's third icon (the AMD Framework 13 and
	Framework Desktop) the driver staying off looks like a hang that never
	goes away. So the marker counts the starts in a row that did not finish,
	and the driver stays off only once kMaxUnfinishedStarts of them have; a
	start that brings the machine down costs one more boot before that.
	Older markers have no count and stand for one start.
	Only a marker of an earlier boot counts: devfs may unload the driver and
	start it again in the same boot (when its binary changed), and the
	stopping driver leaves the marker alone, since it must not touch the
	file system then (see sync_volumes()). The next start keeps it, and its
	stable timer deletes it. */
static const char* kStartMarker
	= "/boot/home/config/settings/kernel/drivers/amdgpu_drm.starting";
static const char kUnfinishedLabel[] = "unfinished starts: ";
static const int kMaxUnfinishedStarts = 2;
static const bigtime_t kStableAfter = 30000000;
static sem_id sStableSem = -1;
static thread_id sStableThread = -1;
static int sUnfinishedStarts = 0;
	// of earlier boots, found by init_driver()


static bool
marked_this_boot(const struct stat& st)
{
	time_t bootTime = real_time_clock() - system_time() / 1000000;
	return st.st_mtime + 5 >= bootTime;
}


/*	The starts of earlier boots in a row that did not finish: 0 without a
	marker or with one of this boot (see above). */
static int
unfinished_starts()
{
	struct stat st;
	if (stat(kStartMarker, &st) != 0 || marked_this_boot(st))
		return 0;

	int count = 1;
	int fd = _kern_open(-1, kStartMarker, O_RDONLY, 0);
	if (fd < 0)
		return count;
	char buffer[256];
	ssize_t length = _kern_read(fd, 0, buffer, sizeof(buffer) - 1);
	_kern_close(fd);
	if (length <= 0)
		return count;
	buffer[length] = '\0';
	const char* number = strstr(buffer, kUnfinishedLabel);
	if (number == NULL)
		return count;
	number += sizeof(kUnfinishedLabel) - 1;
	int value = 0;
	while (*number >= '0' && *number <= '9' && value < 1000)
		value = value * 10 + *number++ - '0';
	return value > 0 ? value : count;
}


static void
mark_start()
{
	struct stat st;
	if (stat(kStartMarker, &st) == 0 && marked_this_boot(st)) {
		// an earlier start of this boot made it, and it stays
		return;
	}
	int fd = _kern_open(-1, kStartMarker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		// a read-only boot volume: no protection, but no harm either
		TRACE("cannot create %s: %s\n", kStartMarker, strerror(fd));
		return;
	}
	char text[160];
	int length = snprintf(text, sizeof(text), "amdgpu_drm was starting; "
		"delete this file to let it start again\n%s%d\n", kUnfinishedLabel,
		sUnfinishedStarts + 1);
	_kern_write(fd, 0, text, min_c(length, (int)sizeof(text) - 1));
	_kern_fsync(fd, false);
	_kern_close(fd);
	sync_volumes();
}


static status_t
stable_thread(void*)
{
	// released early when the driver is unloaded
	if (acquire_sem_etc(sStableSem, 1, B_RELATIVE_TIMEOUT, kStableAfter)
			== B_TIMED_OUT) {
		_kern_unlink(-1, kStartMarker);
		sync_volumes();
		TRACE("running stably\n");
	}
	return B_OK;
}


static void
start_stable_timer()
{
	sStableSem = create_sem(0, "amdgpu_drm stable");
	if (sStableSem < 0)
		return;
	sStableThread = spawn_kernel_thread(stable_thread, "amdgpu_drm stable",
		B_LOW_PRIORITY, NULL);
	if (sStableThread >= 0)
		resume_thread(sStableThread);
}


static void
stop_stable_timer()
{
	if (sStableSem < 0)
		return;
	release_sem(sStableSem);
	if (sStableThread >= 0) {
		status_t result;
		wait_for_thread(sStableThread, &result);
		sStableThread = -1;
	}
	delete_sem(sStableSem);
	sStableSem = -1;
}
#endif


/*	devfs reloads a driver none of whose devices is open once it takes its
	binary for changed: after a file system event in the drivers'
	directories, and also when it scans a directory that links the same
	binary again (amdgpu_drm was linked from dev/dri too, and the first open
	of a /dev/dri path scanned it). A reload stops the GPU and starts it
	again, in devfs' driver lock; on the X399 the stop panicked or froze the
	machine (AMDGPU-DRM.md). Like app_server, keep a device open for as long
	as the GPU runs: devfs then never reloads the driver, and a changed
	binary starts at the next boot. Only the moment before this thread has
	the device open is left (a stop then would unload the driver under the
	thread). The render node: the first opener of card0 becomes DRM master,
	which is for app_server's accelerant to be. */
static int sPinFile = -1;

static status_t
pin_thread(void*)
{
	// devfs publishes the devices once init_driver() returned
	int fd = B_ENTRY_NOT_FOUND;
	for (int attempt = 0; attempt < 500; attempt++) {
		fd = _kern_open(-1, "/dev/dri/renderD128", O_RDWR, 0);
		if (fd >= 0 || fd != B_ENTRY_NOT_FOUND)
			break;
		snooze(20000);
	}
	if (fd < 0) {
		TRACE("cannot keep /dev/dri/renderD128 open: %s\n", strerror(fd));
		return B_OK;
	}
	sPinFile = fd;
	TRACE("keeping /dev/dri/renderD128 open: devfs does not reload a running "
		"GPU's driver\n");
	return B_OK;
}


#ifdef AMDGPU_DRM_ARMED_ONLY
static status_t
unpin_thread(void*)
{
	// a kernel thread: the descriptor is the kernel team's
	if (sPinFile >= 0) {
		_kern_close(sPinFile);
		sPinFile = -1;
		LAB_LOG("unpinned\n");
	}
	return B_OK;
}


static void
lab_unpin()
{
	thread_id thread = spawn_kernel_thread(unpin_thread, "amdgpu_drm unpin",
		B_NORMAL_PRIORITY, NULL);
	if (thread >= 0) {
		resume_thread(thread);
		status_t result;
		wait_for_thread(thread, &result);
	}
}
#endif


status_t
init_hardware(void)
{

	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return B_ERROR;
	pci_info info;
	status_t status = B_ENTRY_NOT_FOUND;
	if (self_test_requested())
		status = B_OK;
	for (int32 i = 0; pci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (is_amd_gpu(info)) {
			status = B_OK;
			break;
		}
	}
	put_module(B_PCI_MODULE_NAME);
	return status;
}


static bool sShutdownHookRegistered;
static bool sPowerHookRegistered;


/*	The GPUs are brought to rest before the machine is powered off or
	rebooted, as Linux does: a reboot does not reset an APU's GPU. */
static void
shutdown_gpus(bool reboot, void* cookie)
{
	TRACE("%s: shutting the GPUs down\n", reboot ? "reboot" : "power off");
	lx_glue_shutdown();
}


static void start_watch();


#ifndef AMDGPU_DRM_ARMED_ONLY
/*	devfs' kernel daemon loads a driver whose binary changed while the
	system runs (an installation, a package activated). If app_server runs
	already, it drives the screens through another driver then (the
	framebuffer driver: this one was not running), and starting the GPU
	underneath it would be two drivers on one GPU: the GPU starts at the
	next boot instead. At a boot, app_server's own search for displays loads
	the driver. */
static bool
desktop_runs_on_another_driver()
{
	thread_info info;
	return get_thread_info(find_thread(NULL), &info) == B_OK
		&& strcmp(info.name, "kernel daemon") == 0
		&& find_thread("app_server") >= 0;
}
#endif


status_t
init_driver(void)
{
#ifdef AMDGPU_DRM_ARMED_ONLY
	// every start, also devfs reloading the driver later
	if (!armed_for_this_boot()) {
		TRACE("not armed (%s), not starting\n", kArmFile);
		return B_ERROR;
	}
#else
	sUnfinishedStarts = unfinished_starts();
	if (sUnfinishedStarts >= kMaxUnfinishedStarts) {
		dprintf(DRIVER_NAME ": the last %d starts did not finish (%s exists); "
			"not starting\n", sUnfinishedStarts, kStartMarker);
		return B_ERROR;
	}
	if (sUnfinishedStarts > 0) {
		dprintf(DRIVER_NAME ": the last start did not finish (%s exists); "
			"trying once more\n", kStartMarker);
	}
	if (desktop_runs_on_another_driver()) {
		dprintf(DRIVER_NAME ": installed while app_server drives the screens "
			"through another driver; the GPU starts at the next boot\n");
		return B_ERROR;
	}
#endif

	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return B_ERROR;

	sSelfTest = self_test_requested();
	TRACE("starting Linux DRM and amdgpu\n");
	apply_settings();
	int error = lx_glue_init();
	if (error != 0) {
		TRACE("start failed: %d\n", error);
		put_module(B_PCI_MODULE_NAME);
		return haiku_status(error);
	}
	sStarted = true;

	pci_info info;
	int gpus = 0;
	if (sSelfTest != 0) {
		int failures = lx_glue_selftest();
		if (sSelfTest == 1) {
			TRACE("self test: %d failure(s); stopping again\n", failures);
			lx_glue_uninit();
			sStarted = false;
			put_module(B_PCI_MODULE_NAME);
			TRACE("self test: stopped\n");
			return B_ERROR;
		}
		if (lx_glue_fake_device_start() == 0)
			gpus++;
	}

#ifndef AMDGPU_DRM_ARMED_ONLY
	bool marked = false;
#endif
	for (int32 i = 0; sSelfTest == 0
			&& pci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (!is_amd_gpu(info))
			continue;
#ifndef AMDGPU_DRM_ARMED_ONLY
		if (!marked) {
			mark_start();
			marked = true;
			lx_haiku_log(DRIVER_NAME ": starting: marker made\n");
		}
#endif
		if (lx_glue_probe(info.bus, info.device, info.function) == 0)
			gpus++;
	}
	put_module(B_PCI_MODULE_NAME);
	lx_haiku_log(DRIVER_NAME ": starting: probed\n");

	if (gpus == 0) {
		// nothing to drive: stop again, so that devfs unloads the driver
		TRACE("no GPU started\n");
		lx_glue_uninit();
		sStarted = false;
#ifndef AMDGPU_DRM_ARMED_ONLY
		if (marked) {
			// it failed, but it did not bring the machine down; no sync after
			// the GPU was touched (BFS writes its log after two idle seconds)
			_kern_unlink(-1, kStartMarker);
		}
#endif
		return B_ENTRY_NOT_FOUND;
	}

	int minors[kMaxDevices / 2];
	int count = lx_glue_minors(minors, kMaxDevices / 2);
	for (int i = 0; i < count; i++) {
		char name[32];
		if (minors[i] >= 128)
			snprintf(name, sizeof(name), "dri/renderD%d", minors[i]);
		else
			snprintf(name, sizeof(name), "dri/card%d", minors[i]);
		sDeviceNames[sDeviceCount] = strdup(name);
		sDeviceMinors[sDeviceCount] = minors[i];
		sDeviceCount++;
		if (minors[i] < 128) {
			// app_server looks for displays in /dev/graphics only: the same
			// primary node there, which names the KMS accelerant
			snprintf(name, sizeof(name), "graphics/amdgpu_drm_%d", minors[i]);
			sDeviceNames[sDeviceCount] = strdup(name);
			sDeviceMinors[sDeviceCount] = minors[i];
			sDeviceCount++;
		}
	}
	sDeviceNames[sDeviceCount] = NULL;
	TRACE("%d GPU(s), %d device(s)\n", gpus, sDeviceCount);
	if (register_shutdown_hook != NULL
		&& register_shutdown_hook(&shutdown_gpus, NULL) == B_OK) {
		sShutdownHookRegistered = true;
	}
	sSleepCondition.Init(&sSleepCondition, "amdgpu_drm sleep");
	if (device_manager_add_power_hook_etc != NULL) {
		sPowerHookRegistered = device_manager_add_power_hook_etc(&power_hook,
			NULL, DRIVER_NAME, kPowerHookDeepSleep) == B_OK;
	} else if (device_manager_add_power_hook != NULL) {
		sPowerHookRegistered = device_manager_add_power_hook(&power_hook,
			NULL, DRIVER_NAME) == B_OK;
	}
	// after publish_devices(), which follows init_driver()
	thread_id pin = spawn_kernel_thread(pin_thread, "amdgpu_drm pin",
		B_NORMAL_PRIORITY, NULL);
	if (pin >= 0)
		resume_thread(pin);
#ifndef AMDGPU_DRM_ARMED_ONLY
	start_stable_timer();
#endif
	start_watch();
	return B_OK;
}


/*	Lab aid, with a log file only (log_file): from the end of the start
	until the driver stops, this thread writes a heartbeat around the file
	system into the log file's last MiB (lx_haiku_raw_log_prepare(), a ring;
	not into the syslog), every second for two minutes and every 10 s after
	that: the newest line dates a freeze of the whole machine, which leaves
	no other trace on the lab X399. A panic shows in kdl_capture's file
	instead (the Haiku tree's src/add-ons/kernel/debugger/kdl_capture). */
static sem_id sWatchSem = -1;
static thread_id sWatchThread = -1;

static status_t
watch_thread(void*)
{
	lx_haiku_raw_log_start(find_thread(NULL));
	for (int beat = 0;; beat++) {
		bigtime_t interval = beat < 120 ? 1000000 : 10000000;
		if (acquire_sem_etc(sWatchSem, 1, B_RELATIVE_TIMEOUT, interval)
				!= B_TIMED_OUT) {
			break;
		}
		char line[96];
		snprintf(line, sizeof(line), DRIVER_NAME ": watch: log at %" B_PRId64
			"\n", lx_haiku_log_position());
		lx_haiku_log(line);
	}
	return B_OK;
}


static void
start_watch()
{
	if (lx_haiku_raw_log_prepare() != 0)
		return;
	sWatchSem = create_sem(0, DRIVER_NAME " watch");
	if (sWatchSem < 0)
		return;
	sWatchThread = spawn_kernel_thread(watch_thread, DRIVER_NAME " watch",
		B_URGENT_DISPLAY_PRIORITY, NULL);
	if (sWatchThread >= 0)
		resume_thread(sWatchThread);
}


static void
end_watch()
{
	if (sWatchSem >= 0) {
		release_sem(sWatchSem);
		if (sWatchThread >= 0) {
			status_t result;
			wait_for_thread(sWatchThread, &result);
			sWatchThread = -1;
		}
		delete_sem(sWatchSem);
		sWatchSem = -1;
	}
	lx_haiku_raw_log_end();
}


void
uninit_driver(void)
{
	// devfs unloads the driver in its driver lock, when none of its devices
	// is open (see pin_thread()): no file system work here (see
	// sync_volumes()); the log file has the steps.
	lx_haiku_log(DRIVER_NAME ": uninit_driver\n");
	lx_trace_calls_start_stop();
	if (sShutdownHookRegistered) {
		unregister_shutdown_hook(&shutdown_gpus, NULL);
		sShutdownHookRegistered = false;
	}
	if (sPowerHookRegistered) {
		device_manager_remove_power_hook(&power_hook, NULL);
		sPowerHookRegistered = false;
	}
	// unpinned already (devfs unloads no driver with an open device), or
	// never pinned (a start that failed)
	if (sPinFile >= 0) {
		_kern_close(sPinFile);
		sPinFile = -1;
	}
#ifndef AMDGPU_DRM_ARMED_ONLY
	// a start marker stays (see unfinished_starts())
	stop_stable_timer();
#endif
	for (int i = 0; i < sDeviceCount; i++)
		free(sDeviceNames[i]);
	sDeviceCount = 0;
	if (sStarted)
		lx_glue_uninit();
	sStarted = false;
	end_watch();
	// the log file is closed now: the syslog only
	TRACE("uninit_driver: done\n");
}


const char**
publish_devices(void)
{
	return (const char**)sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return minor_of(name) >= 0 ? &sDeviceHooks : NULL;
}
