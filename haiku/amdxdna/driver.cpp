/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Haiku driver around Linux's amdxdna, for the NPU of AMD's Ryzen AI
	processors: publishes /dev/npu/amdxdna/0 and forwards open and ioctl to
	the DRM core (glue.c). The ioctls are Linux's (uapi/drm/amdxdna_accel.h);
	buffer objects are mapped into teams by DRM_HAIKU_IOCTL_MAP, as for
	haiku/amdgpu.

	Nothing here touches the NPU until a client opens the node. Loading the
	driver (devfs does that when /dev/npu is first looked into, which a
	launch job of the package does early in the boot) finds the PCI device
	in the bus manager's list and takes the memory the NPU's buffers will
	be made of: a long run of pages, which is only there to be had while
	the machine is fresh (glue.c). The first open starts the Linux side,
	loads the firmware and powers the NPU up; five seconds after the last
	client has closed the node (as Linux does it; "autosuspend_ms"), the NPU
	is powered down again, and the next open powers it up.

	The NPU reads and writes physical memory at the addresses its clients
	put into their commands (there is no IOMMU driver to confine it, see
	glue.c and the README): a client of this node can reach all of the
	machine's memory. So only root can open it.

	Settings (~/config/settings/kernel/drivers/amdxdna): one parameter per
	line, "name value": amdxdna's and drm's module parameters, and
	"carveout_mb <MiB>" (the buffer memory, 128), "carveout_at_load 0" (take
	it at the first start of the NPU instead of when the driver is loaded:
	nothing is held on a machine that never uses the NPU, at the risk of
	finding none later), "log_file <path>" (a
	synced copy of the driver's messages, see compat/src/vsprintf.c),
	"autosuspend_ms <ms>" (how long the NPU stays powered without a client,
	5000; 0: not at all), "keep_powered 1" (never power it down once it was
	started: a bring-up aid), "step_log 0|1|2" (lines before and after the
	steps that touch the hardware: none, those of power, contexts and
	interrupt handlers, or every message to the firmware as well),
	"haiku_selftest 1|2|3" (without an NPU: test the compat layer, offer a
	device without hardware, or drive a stand-in for the hardware in a
	virtual machine, see fakefw.c). */

#include <new>
#include <stdio.h>
#include <string.h>

#include <driver_settings.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <PCI.h>
#include <OS.h>
#include <SupportDefs.h>
#include <errno.h>
#include <unistd.h>

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


// The device manager's power hooks (kdevice_manager.h), where the kernel has
// them: it calls them when the system suspends and resumes. Weak, and typed
// (see haiku/amdgpu/driver.cpp): the driver loads on kernels without.
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
// DEVICE_MANAGER_POWER_HOOK_DEEP_SLEEP: an NPU in use only sleeps in a deep
// sleep (it takes seconds to come back: its firmware is loaded again)
static const uint32 kPowerHookDeepSleep = 0x01;


#define DRIVER_NAME "amdxdna"
#define DEVICE_NAME "npu/amdxdna/0"
#define TRACE(x...) dprintf(DRIVER_NAME ": " x)

// the synced log file (log_file), and the syslog
extern "C" void lx_haiku_log(const char* text);

// Linux's DRM ioctl numbers (asm-generic/ioctl.h) for the Haiku additions;
// the same ones, with the same structures, as haiku/amdgpu's
// (compat/include/drm/drm_haiku.h)
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
struct drm_haiku_set_param {
	char	name[64];
	char	value[64];
};
static const uint32 kDrmHaikuMap = LX_IOC(3, 'd', 0xf0,
	sizeof(drm_haiku_map));
static const uint32 kDrmHaikuUnmap = LX_IOC(1, 'd', 0xf1,
	sizeof(drm_haiku_unmap));
static const uint32 kDrmHaikuDeviceInfo = LX_IOC(2, 'd', 0xf2,
	sizeof(drm_haiku_device_info));
static const uint32 kDrmHaikuSetParam = LX_IOC(1, 'd', 0xf5,
	sizeof(drm_haiku_set_param));


int32 api_version = B_CUR_DRIVER_API_VERSION;

static char* sDeviceNames[2];

// the NPU, as the PCI bus manager lists it (nothing is read from the device)
static bool sFound;
static pci_info sInfo;

// 0: none, 1: test the compat layer and stop, 2: a device without hardware,
// 3: a stand-in for the hardware in a virtual machine (power as on the NPU)
static int sSelfTest;

/*	What has been started; sStateLock serializes starting, stopping and the
	NPU's power. */
static mutex sStateLock = MUTEX_INITIALIZER("amdxdna state");
static bool sLinuxUp;
	// the DRM core and the driver, without a device
static bool sDeviceUp;
	// the NPU was started (and is the DRM device files are opened on)
static bool sPowered;
	// ... and is powered up now
static int32 sOpenCount;

/*	The NPU is powered down a while after its last client has gone, by a
	thread that waits for that time (sIdleDeadline, 0: nothing to do; set and
	cleared under sStateLock). */
static thread_id sIdleThread = -1;
static sem_id sIdleSem = -1;
static bigtime_t sIdleDeadline;
static bool sIdleQuit;


// #pragma mark - errors


static status_t
haiku_status(long linuxError)
{
	if (linuxError >= 0)
		return B_OK;
	switch (-linuxError) {
		case 1: return B_NOT_ALLOWED;			// EPERM
		case 2: return B_ENTRY_NOT_FOUND;		// ENOENT
		case 3: return ESRCH;
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
		case 105: return ENOBUFS;
		case 110: return B_TIMED_OUT;			// ETIMEDOUT
		case 114: return EALREADY;
		case 125: return ECANCELED;
		case 512: return B_INTERRUPTED;			// ERESTARTSYS
		case 524: return B_NOT_SUPPORTED;		// ENOTSUPP
	}
	return B_ERROR;
}


// #pragma mark - buffer mappings


/*!	The cache of a buffer mapped into teams (DRM_HAIKU_IOCTL_MAP), as
	haiku/amdgpu/driver.cpp has it, without the part for buffers that move:
	the NPU's buffers stay where they are. It has no pages of its own:
	Fault() runs the buffer's Linux fault handler and maps the page frames it
	finds into the area that faulted. Areas cloned from the area, or
	inherited by fork(), share the cache (B_SHARED_AREA); when the mapping
	ends (DRM_HAIKU_IOCTL_UNMAP, or the file is closed), Detach() unmaps the
	pages from all of them, and they fault with B_BAD_ADDRESS from then on.

	Locking: the VM calls Fault() with the address space read locked and the
	cache locked; the Linux fault handler runs with the cache unlocked.
*/
class BufferCache final : public VMCache {
public:
								BufferCache();

			status_t			Init(page_num_t pageCount);
			void				SetMapping(void* mapping);

	virtual	status_t			Fault(VMAddressSpace* addressSpace,
									off_t offset);
			status_t			FaultIn(VMAddressSpace* addressSpace,
									page_num_t page, page_num_t end);

			void*				Detach();

protected:
	virtual	void				DeleteObject();

private:
			void*				fMapping;
				// the glue's mapping; NULL before it is set and once detached
			page_num_t			fPageCount;
			int32				fFaulting;
			ConditionVariable	fFaultsDone;
};


static const page_num_t kFaultPages = 64;
	// pages mapped at once: the one touched and the ones after it


BufferCache::BufferCache()
	:
	fMapping(NULL),
	fPageCount(0),
	fFaulting(0)
{
}


status_t
BufferCache::Init(page_num_t pageCount)
{
	status_t status = VMCache::Init("npu buffer", CACHE_TYPE_DEVICE, 0);
	if (status != B_OK)
		return status;
	fPageCount = pageCount;
	virtual_end = (off_t)pageCount * B_PAGE_SIZE;
	fFaultsDone.Init(this, "npu buffer faults");
	return B_OK;
}


void
BufferCache::SetMapping(void* mapping)
{
	AutoLocker<VMCache> locker(this);
	fMapping = mapping;
}


status_t
BufferCache::Fault(VMAddressSpace* addressSpace, off_t offset)
{
	page_num_t page = offset / B_PAGE_SIZE;
	if (page >= fPageCount)
		return B_BAD_ADDRESS;

	Unlock();
	status_t status = FaultIn(addressSpace, page,
		min_c(page + kFaultPages, fPageCount));
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
	while (page < end) {
		VMArea* targets[4];
		int targetCount = 0;
		Lock();
		void* mapping = fMapping;
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
		bool attached = fMapping != NULL;
		if (count > 0 && attached) {
			map->Lock();
			for (int i = 0; i < targetCount; i++) {
				VMArea* area = targets[i];
				// fork() doesn't copy the type, and a "device" area's
				// pages are unmapped without page bookkeeping
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
					// ordinary memory: write-back cached (caching says so)
					map->Map(address, (phys_addr_t)pfns[index] * B_PAGE_SIZE,
						area->protection, B_WRITE_BACK_MEMORY, &reservation);
				}
			}
			map->Unlock();
		}
		Unlock();
		vm_page_unreserve_pages(&reservation);

		if (count <= 0 || !attached)
			return B_BAD_ADDRESS;
		page += count;
	}
	return B_OK;
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
	// The areas stay in their address spaces while they are in the cache's
	// list (delete_area() removes them from the list first).
	for (VMArea* area = areas.First(); area != NULL;
			area = areas.GetNext(area)) {
		VMTranslationMap* map = area->address_space->TranslationMap();
		map->Lock();
		map->Unmap(area->Base(), area->Base() + area->Size() - 1);
		map->Unlock();
	}
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


struct BufferMapping : DoublyLinkedListLinkImpl<BufferMapping> {
	team_id		team;
	area_id		area;
	addr_t		address;
	BufferCache* cache;
};

typedef DoublyLinkedList<BufferMapping> MappingList;


struct DeviceCookie {
	void*		file;
	mutex		lock;
	MappingList	mappings;
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

	// The area first: the driver wants to know where the team has the
	// buffer. Until the mapping is set, the area faults with B_BAD_ADDRESS.
	uint32 protection = B_READ_AREA | B_KERNEL_READ_AREA | B_SHARED_AREA
		| ((request->flags & 1) ? B_WRITE_AREA | B_KERNEL_WRITE_AREA : 0);
	team_id team = team_get_current_team_id();
	VMAddressSpace* addressSpace = VMAddressSpace::Get(team);
	if (addressSpace == NULL) {
		cache->ReleaseRef();
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
	status = vm_map_cache(addressSpace, cache, 0, "npu buffer", request->size,
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
		cache->ReleaseRef();
		return status;
	}

	void* mapping = lx_glue_map(cookie->file, request->offset, request->size,
		(request->flags & 1) != 0, (addr_t)address);
	if (int error = lx_glue_is_error(mapping)) {
		addressSpace->Put();
		vm_delete_area(team, areaID, true);
		cache->ReleaseRef();
		return haiku_status(error);
	}
	cache->SetMapping(mapping);

	// map it all now: a buffer that cannot be mapped fails here, not at the
	// team's first access
	addressSpace->ReadLock();
	if (addressSpace->LookupArea((addr_t)address) == area
		&& area->id == areaID) {
		status = cache->FaultIn(addressSpace, 0, pageCount);
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
		remove_mapping(record);
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


// #pragma mark - settings


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


// #pragma mark - start marker


/*	Makes the marker durable: a sync waits for every volume, network ones
	too, so it runs in a thread of its own that is waited for a few seconds
	at most, and that runs kernel code only (see haiku/amdgpu/driver.cpp). */
static void
sync_volumes()
{
	thread_id thread = spawn_kernel_thread((thread_func)(void*)&_kern_sync,
		DRIVER_NAME " sync", B_NORMAL_PRIORITY, NULL);
	if (thread < 0)
		return;
	resume_thread(thread);
	status_t result;
	if (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 3000000, &result)
			== B_TIMED_OUT) {
		TRACE("the sync takes long (a network volume?); not waiting for it\n");
	}
}


/*	Starting the NPU loads firmware into it and waits for its answers. Should
	that ever stop the machine, it must not do so again whenever some
	program opens the node at the next boot: a marker file exists from
	before the first start of a boot until the NPU has answered, and while
	one exists the NPU is not started. (A start that fails is no reason: the
	marker goes then.) */
static const char* kStartMarker
	= "/boot/home/config/settings/kernel/drivers/amdxdna.starting";


static bool
previous_start_unfinished()
{
	struct stat st;
	return stat(kStartMarker, &st) == 0;
}


static void
mark_start()
{
	int fd = _kern_open(-1, kStartMarker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		// a read-only boot volume: no protection, but no harm either
		TRACE("cannot create %s: %s\n", kStartMarker, strerror(fd));
		return;
	}
	static const char kText[] = "amdxdna was starting the NPU; delete this "
		"file to let it try again\n";
	_kern_write(fd, 0, kText, sizeof(kText) - 1);
	_kern_fsync(fd, false);
	_kern_close(fd);
	sync_volumes();
}


static void
unmark_start()
{
	_kern_unlink(-1, kStartMarker);
}


// #pragma mark - power


/*	While the system sleeps (suspend to idle), calls wait at the door; the
	ones under way get a while to finish first. (Linux freezes every task
	before it suspends devices; Haiku's keep running.) */
static mutex sSleepLock = MUTEX_INITIALIZER("amdxdna sleep");
static ConditionVariable sSleepCondition;
static bool sSleeping;
static int32 sCallsUnderway;
static bool sPowerHookRegistered;
static bool sSuspendedForSleep;


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


/*!	The system suspends to idle (state 0; where the kernel tells sleeps
	apart, only a deep one) or resumes. An NPU nobody uses is powered down
	already and stays as it is. One in use is powered down as for Linux's
	system sleep (amdxdna_pm_suspend(): commands under way are
	waited for, the contexts taken from the firmware) and powered up again on
	the way back, with the contexts restored.
*/
static status_t
power_hook(void* cookie, bool resume, int32 state)
{
	if (!resume) {
		if (state != 0)
			return B_OK;
		// opens and closes change the NPU's power under sStateLock; the
		// sleep does not wait for one that is starting the NPU
		if (mutex_trylock(&sStateLock) != B_OK) {
			TRACE("busy starting or stopping: the NPU stays as it is for "
				"this sleep\n");
			return B_OK;
		}
		if (sDeviceUp && sPowered && sSelfTest != 2) {
			close_door();
			if (lx_glue_suspend() == 0) {
				sPowered = false;
				sIdleDeadline = 0;
				sSuspendedForSleep = true;
			} else
				open_door();
		}
		mutex_unlock(&sStateLock);
		return B_OK;
	}

	if (sSuspendedForSleep) {
		MutexLocker locker(sStateLock);
		sSuspendedForSleep = false;
		// without a client it stays down until the next open
		if (sOpenCount > 0 || lx_glue_keep_powered()) {
			if (lx_glue_resume() == 0)
				sPowered = true;
			else
				TRACE("the NPU did not come back: its clients' calls fail\n");
		}
		open_door();
	}
	return B_OK;
}


// #pragma mark - starting and stopping


static void start_idle_thread();


/*!	Starts what is not running yet: the Linux side, the NPU (or the device
	without hardware), its power. sStateLock must be held.
*/
static status_t
start_device()
{
	if (!sLinuxUp) {
		int error = lx_glue_init();
		if (error != 0) {
			TRACE("starting the Linux side failed: %d\n", error);
			return haiku_status(error);
		}
		sLinuxUp = true;
		sSleepCondition.Init(&sSleepCondition, "amdxdna sleep");
		if (device_manager_add_power_hook_etc != NULL) {
			sPowerHookRegistered = device_manager_add_power_hook_etc(
				&power_hook, NULL, DRIVER_NAME, kPowerHookDeepSleep) == B_OK;
		} else if (device_manager_add_power_hook != NULL) {
			sPowerHookRegistered = device_manager_add_power_hook(&power_hook,
				NULL, DRIVER_NAME) == B_OK;
		}
		if (!sPowerHookRegistered) {
			TRACE("no power hooks in this kernel: an NPU in use stays "
				"powered while the system sleeps\n");
		}
	}

	if (!sDeviceUp) {
		int error;
		if (sSelfTest == 2) {
			error = lx_glue_fake_start();
		} else if (sSelfTest == 3) {
			error = lx_glue_emulated_start();
		} else {
			if (previous_start_unfinished()) {
				dprintf(DRIVER_NAME ": the last start of the NPU did not "
					"finish (%s exists); not starting it\n", kStartMarker);
				return B_NOT_ALLOWED;
			}
			mark_start();
			lx_haiku_log(DRIVER_NAME ": starting the NPU: marker made\n");
			error = lx_glue_start(sInfo.bus, sInfo.device, sInfo.function);
			// it answered, or failed without stopping the machine
			unmark_start();
		}
		if (error != 0)
			return haiku_status(error);
		sDeviceUp = true;
		sPowered = true;
		start_idle_thread();
		return B_OK;
	}

	if (!sPowered && sSelfTest != 2) {
		int error = lx_glue_resume();
		if (error != 0)
			return haiku_status(error);
		sPowered = true;
	}
	return B_OK;
}


/*!	Powers the NPU down, if it is up. sStateLock must be held. */
static void
power_down()
{
	sIdleDeadline = 0;
	if (sDeviceUp && sPowered && sSelfTest != 2) {
		if (lx_glue_suspend() == 0)
			sPowered = false;
	}
}


/*!	No client is left: the NPU is powered down, in a while (the settings
	say when, if at all). sStateLock must be held. */
static void
idle_device()
{
	if (!sDeviceUp || !sPowered || sSelfTest == 2 || lx_glue_keep_powered())
		return;
	bigtime_t delay = (bigtime_t)lx_glue_autosuspend_ms() * 1000;
	if (delay == 0 || sIdleThread < 0) {
		power_down();
		return;
	}
	sIdleDeadline = system_time() + delay;
	release_sem_etc(sIdleSem, 1, B_DO_NOT_RESCHEDULE);
}


/*!	Waits for the time the NPU is to be powered down at; woken whenever
	that time changes. */
static status_t
idle_thread(void*)
{
	while (!sIdleQuit) {
		bigtime_t deadline = atomic_get64(&sIdleDeadline);
		status_t status = deadline != 0
			? acquire_sem_etc(sIdleSem, 1, B_ABSOLUTE_TIMEOUT, deadline)
			: acquire_sem(sIdleSem);
		if (sIdleQuit || (status != B_TIMED_OUT && status != B_OK))
			break;
		if (status != B_TIMED_OUT)
			continue;

		enter_call();
		{
			MutexLocker locker(sStateLock);
			// still nobody, and the time was not put off meanwhile
			if (sOpenCount == 0 && sIdleDeadline != 0
				&& system_time() >= sIdleDeadline) {
				power_down();
			}
		}
		leave_call();
	}
	return B_OK;
}


static void
start_idle_thread()
{
	if (sIdleThread >= 0)
		return;
	sIdleQuit = false;
	sIdleSem = create_sem(0, DRIVER_NAME " idle");
	if (sIdleSem < 0)
		return;
	sIdleThread = spawn_kernel_thread(&idle_thread, DRIVER_NAME " idle",
		B_NORMAL_PRIORITY, NULL);
	if (sIdleThread < 0) {
		delete_sem(sIdleSem);
		sIdleSem = -1;
		return;
	}
	resume_thread(sIdleThread);
}


static void
stop_idle_thread()
{
	if (sIdleThread < 0)
		return;
	sIdleQuit = true;
	delete_sem(sIdleSem);
	status_t result;
	wait_for_thread(sIdleThread, &result);
	sIdleThread = -1;
	sIdleSem = -1;
}


// #pragma mark - device hooks


static status_t
device_open(const char* name, uint32 flags, void** _cookie)
{
	if (strcmp(name, DEVICE_NAME) != 0)
		return B_ENTRY_NOT_FOUND;

	// Only root: a client can make the NPU read and write any physical
	// memory. And only for reading and writing, as its clients open it: a
	// program that merely looks at the files in /dev must not start the NPU.
	if (geteuid() != 0)
		return B_PERMISSION_DENIED;
	if ((flags & O_ACCMODE) != O_RDWR)
		return B_NOT_ALLOWED;

	DeviceCookie* cookie = new(std::nothrow) DeviceCookie;
	if (cookie == NULL)
		return B_NO_MEMORY;
	mutex_init(&cookie->lock, "amdxdna file");

	enter_call();
	MutexLocker locker(sStateLock);
	// a client: the NPU stays up
	sIdleDeadline = 0;
	status_t status = start_device();
	if (status == B_OK) {
		int error;
		cookie->file = lx_glue_open(flags & O_ACCMODE, &error);
		if (cookie->file == NULL)
			status = haiku_status(error);
	}
	if (status != B_OK) {
		if (sOpenCount == 0)
			idle_device();
		locker.Unlock();
		leave_call();
		mutex_destroy(&cookie->lock);
		delete cookie;
		return status;
	}
	sOpenCount++;
	locker.Unlock();
	leave_call();

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
	enter_call();
	unmap_all(cookie);
	lx_glue_close(cookie->file);
	{
		MutexLocker locker(sStateLock);
		if (--sOpenCount == 0) {
			lx_glue_note_idle();
			idle_device();
		}
	}
	leave_call();
	mutex_destroy(&cookie->lock);
	delete cookie;
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

	if (op == kDrmHaikuSetParam) {
		// what writing /sys/module/<module>/parameters/<name> does: takes
		// effect where the value is looked at again (drm's "debug" is)
		drm_haiku_set_param request;
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		request.name[sizeof(request.name) - 1] = '\0';
		request.value[sizeof(request.value) - 1] = '\0';
		return haiku_status(lx_glue_set_param(request.name, request.value));
	}

	if (op == kDrmHaikuDeviceInfo) {
		drm_haiku_device_info info = {};
		// one cookie per open file: dup()ed descriptors share it
		info.file_id[0] = (uint32)(addr_t)cookie;
		info.file_id[1] = (uint32)((uint64)(addr_t)cookie >> 32);
		if (sFound) {
			info.bus = sInfo.bus;
			info.dev = sInfo.device;
			info.func = sInfo.function;
			info.vendor_id = sInfo.vendor_id;
			info.device_id = sInfo.device_id;
			info.revision_id = sInfo.revision;
			info.class_code = (sInfo.class_base << 16)
				| (sInfo.class_sub << 8) | sInfo.class_api;
			info.subvendor_id = sInfo.u.h0.subsystem_vendor_id;
			info.subdevice_id = sInfo.u.h0.subsystem_id;
		}
		if (!IS_USER_ADDRESS(buffer)
			|| user_memcpy(buffer, &info, sizeof(info)) != B_OK)
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
device_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sDeviceHooks = {
	device_open,
	device_close,
	device_free,
	device_control,
	device_read,
	device_write,
	NULL,
	NULL,
	NULL,
	NULL
};


// #pragma mark - driver


/*	The NPUs amdxdna drives through its "AIE2" code, by PCI ID: Phoenix and
	Hawk Point (1502), Strix Point, Strix Halo and Krackan Point (17f0). The
	driver itself decides by the revision which one it is (and turns down
	what it does not know). Its newer "AIE4" devices are left alone: they
	have not been looked at for running without an IOMMU. */
static bool
is_npu(const pci_info& info)
{
	return info.vendor_id == 0x1022
		&& (info.device_id == 0x1502 || info.device_id == 0x17f0);
}


status_t
init_hardware(void)
{
	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return B_ERROR;
	pci_info info;
	status_t status = self_test_requested() != 0 ? B_OK : B_ENTRY_NOT_FOUND;
	for (int32 i = 0; pci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (is_npu(info)) {
			status = B_OK;
			break;
		}
	}
	put_module(B_PCI_MODULE_NAME);
	return status;
}


status_t
init_driver(void)
{
	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return B_ERROR;
	sFound = false;
	for (int32 i = 0; pci->get_nth_pci_info(i, &sInfo) == B_OK; i++) {
		if (is_npu(sInfo)) {
			sFound = true;
			break;
		}
	}
	put_module(B_PCI_MODULE_NAME);

	sSelfTest = self_test_requested();
	if (sSelfTest == 1) {
		// the compat layer's test, and nothing else
		TRACE("self test\n");
		apply_settings();
		int error = lx_glue_init();
		if (error == 0) {
			int failures = lx_glue_selftest();
			TRACE("self test: %d failure(s); stopping again\n", failures);
			lx_glue_uninit();
		} else
			TRACE("self test: starting the Linux side failed: %d\n", error);
		lx_glue_unreserve();
		TRACE("self test: stopped\n");
		return B_ERROR;
	}
	if (!sFound && sSelfTest == 0)
		return B_ENTRY_NOT_FOUND;

	sDeviceNames[0] = strdup(DEVICE_NAME);
	sDeviceNames[1] = NULL;
	if (sDeviceNames[0] == NULL)
		return B_NO_MEMORY;
	if (sSelfTest == 2) {
		TRACE("self test: /dev/" DEVICE_NAME " will be a device without "
			"hardware\n");
	} else if (sSelfTest == 3) {
		TRACE("self test: /dev/" DEVICE_NAME " will be a stand-in for the "
			"NPU's hardware\n");
	} else {
		TRACE("NPU %04x:%04x rev %02x at %d:%d.%d: /dev/" DEVICE_NAME
			" starts it when opened\n", sInfo.vendor_id, sInfo.device_id,
			sInfo.revision, sInfo.bus, sInfo.device, sInfo.function);
	}

	// The settings, and with them the memory for the NPU's buffers, now:
	// later there may be no such run of pages. Without it the driver is
	// still there, and looks again when the NPU is started.
	apply_settings();
	lx_glue_reserve();
	return B_OK;
}


void
uninit_driver(void)
{
	// devfs unloads the driver in its driver lock, when its device is not
	// open: no file system work here (see haiku/amdgpu/driver.cpp)
	TRACE("uninit_driver\n");
	stop_idle_thread();
	if (sPowerHookRegistered) {
		device_manager_remove_power_hook(&power_hook, NULL);
		sPowerHookRegistered = false;
	}
	MutexLocker locker(sStateLock);
	if (sDeviceUp) {
		// powers the NPU down if it is up
		lx_glue_stop();
		sDeviceUp = false;
		sPowered = false;
	}
	if (sLinuxUp) {
		lx_glue_uninit();
		sLinuxUp = false;
	}
	lx_glue_unreserve();
	locker.Unlock();
	free(sDeviceNames[0]);
	sDeviceNames[0] = NULL;
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
	return strcmp(name, DEVICE_NAME) == 0 ? &sDeviceHooks : NULL;
}
