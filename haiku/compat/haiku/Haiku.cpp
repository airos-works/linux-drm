/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Haiku kernel services of lx/haiku.h. Nothing here knows Linux
	structures; errors are returned as Linux error numbers. */

#include <new>
#include <stdio.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <Drivers.h>
#include <fs_info.h>
#include <fs_interface.h>

#include <condition_variable.h>
#include <cpu.h>
#include <debug.h>
#include <frame_buffer_console.h>
#include <kernel.h>
#include <lock.h>
#include <Notifications.h>
#include <smp.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/Random.h>
#include <sys/uio.h>
#include <util/iovec_support.h>
#include <vfs.h>
#include <syscalls.h>
#include <vm/vm.h>
#include <vm/vm_page.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMArea.h>
#include <vm/VMCache.h>
#include <arch/x86/arch_cpu.h>
#include <arch/debug.h>

#include "../include/lx/haiku.h"

// the add-on's name (the Makefile's): "amdgpu_drm" or "amdxdna"
#ifndef LX_DRIVER_NAME
#	define LX_DRIVER_NAME "amdgpu_drm"
#endif


static_assert(sizeof(mutex) <= LX_HAIKU_MUTEX_SIZE, "mutex storage");
static_assert(sizeof(rw_lock) <= LX_HAIKU_RWLOCK_SIZE, "rw_lock storage");

// Linux error numbers used here
static const int kLinuxENOMEM = 12;
static const int kLinuxEFAULT = 14;
static const int kLinuxENOENT = 2;
static const int kLinuxEIO = 5;
static const int kLinuxETIMEDOUT = 110;
static const int kLinuxEINTR = 4;
static const int kLinuxEPERM = 1;
static const int kLinuxESRCH = 3;


// #pragma mark - output, time, processors


/*	The log file must exist already, preallocated (zeros): it is written in
	place and never grows, so that a synced write changes no file system
	structure that a crash right after it could lose. Each opening continues
	after the text already in it (the first zero byte), so the boot after a
	crash keeps the crashed boot's lines; a full file starts over. */
static int sLogFile = -1;
static off_t sLogPosition;
static off_t sLogSize;
static mutex sLogFileLock = MUTEX_INITIALIZER(LX_DRIVER_NAME " log file");


extern "C" int
lx_haiku_log_file(const char* path)
{
	MutexLocker locker(sLogFileLock);
	if (sLogFile >= 0) {
		_kern_close(sLogFile);
		sLogFile = -1;
	}
	if (path == NULL || path[0] == '\0')
		return 0;
	// read too: the end of the text already in it
	int fd = _kern_open(-1, path, O_RDWR, 0);
	if (fd < 0)
		return -kLinuxEIO;
	struct stat st;
	if (_kern_read_stat(fd, NULL, false, &st, sizeof(st)) != B_OK
		|| st.st_size <= 0) {
		_kern_close(fd);
		return -kLinuxEIO;
	}
	sLogFile = fd;
	sLogPosition = 0;
	sLogSize = st.st_size;
	// a 64 KiB stack buffer is too much for a kernel thread
	const size_t kChunk = 65536;
	char* chunk = (char*)malloc(kChunk);
	off_t end = -1;
	while (chunk != NULL && end < 0 && sLogPosition < sLogSize) {
		ssize_t bytes = _kern_read(fd, sLogPosition, chunk, kChunk);
		if (bytes <= 0)
			break;
		char* zero = (char*)memchr(chunk, 0, bytes);
		if (zero != NULL)
			end = sLogPosition + (zero - chunk);
		else
			sLogPosition += bytes;
	}
	free(chunk);
	sLogPosition = end >= 0 ? end : 0;
	char line[96];
	snprintf(line, sizeof(line), "==== log opened, %lu s real time, %"
		B_PRId64 " ms after boot\n", (unsigned long)real_time_clock(),
		system_time() / 1000);
	locker.Unlock();
	lx_haiku_log(line);
	return 0;
}


/*	A second way to the disk for the log, around the file system and its
	caches: the log file's last MiB, written on the raw partition (and the
	drive's cache flushed). For a watch thread's heartbeat when file I/O
	itself may be what waits: the messages of the thread
	lx_haiku_raw_log_start() names go there only (not to the syslog), with
	the time since boot, as a ring - the newest line dates a freeze. Only
	the disk's own I/O path has to work then. Prepared before a stall:
	opening the partition then might wait as well. */
static int sRawDevice = -1;
static off_t sRawOffset;
static off_t sRawSize;
static off_t sRawBlockStart;
static size_t sRawFill;
static char* sRawBlock;
static const size_t kRawBlockSize = 4096;
static thread_id sRawThread = -1;
static mutex sRawLock = MUTEX_INITIALIZER(LX_DRIVER_NAME " raw log");


extern "C" int
lx_haiku_raw_log_prepare(void)
{
	MutexLocker locker(sLogFileLock);
	if (sLogFile < 0 || sRawDevice >= 0 || sLogSize < 2 * 1048576)
		return -1;
	struct vnode* vnode;
	if (vfs_get_vnode_from_fd(sLogFile, true, &vnode) != B_OK)
		return -1;
	file_io_vec vecs[4];
	size_t count = 4;
	status_t status = vfs_get_file_map(vnode, sLogSize - 1048576, 1048576,
		vecs, &count);
	vfs_put_vnode(vnode);
	struct stat st;
	fs_info info;
	if (status != B_OK || count < 1 || vecs[0].offset < 0
		|| vecs[0].offset % 512 != 0
		|| _kern_read_stat(sLogFile, NULL, false, &st, sizeof(st)) != B_OK
		|| fs_stat_dev(st.st_dev, &info) != 0)
		return -1;
	sRawBlock = (char*)malloc(kRawBlockSize);
	if (sRawBlock == NULL)
		return -1;
	int fd = _kern_open(-1, info.device_name, O_RDWR, 0);
	if (fd < 0) {
		free(sRawBlock);
		sRawBlock = NULL;
		return -1;
	}
	sRawDevice = fd;
	// the file's own log stops short of it
	sLogSize -= 1048576;
	sRawOffset = vecs[0].offset;
	sRawSize = min_c(vecs[0].length, (off_t)1048576) / kRawBlockSize
		* kRawBlockSize;
	sRawBlockStart = 0;
	sRawFill = 0;
	memset(sRawBlock, 0, kRawBlockSize);
	return 0;
}


extern "C" void
lx_haiku_raw_log_start(int32 thread)
{
	if (sRawDevice >= 0)
		sRawThread = thread;
}


extern "C" void
lx_haiku_raw_log_end(void)
{
	MutexLocker locker(sRawLock);
	sRawThread = -1;
	if (sRawDevice >= 0)
		_kern_close(sRawDevice);
	sRawDevice = -1;
	free(sRawBlock);
	sRawBlock = NULL;
}


static void
raw_log(const char* text)
{
	MutexLocker locker(sRawLock);
	if (sRawDevice < 0)
		return;
	char line[192];
	bigtime_t now = system_time();
	snprintf(line, sizeof(line), "[%" B_PRId64 ".%03d] %s", now / 1000000,
		(int)(now / 1000 % 1000), text);
	text = line;
	for (size_t length = strlen(text); length > 0;) {
		if (sRawFill == kRawBlockSize) {
			// a ring: the oldest block goes
			sRawBlockStart += kRawBlockSize;
			if (sRawBlockStart >= sRawSize)
				sRawBlockStart = 0;
			sRawFill = 0;
			memset(sRawBlock, 0, kRawBlockSize);
		}
		size_t part = min_c(length, kRawBlockSize - sRawFill);
		memcpy(sRawBlock + sRawFill, text, part);
		sRawFill += part;
		text += part;
		length -= part;
		_kern_write(sRawDevice, sRawOffset + sRawBlockStart, sRawBlock,
			kRawBlockSize);
	}
	_kern_ioctl(sRawDevice, B_FLUSH_DRIVE_CACHE, NULL, 0);
}


// The kernel's suspend trace (kdevice_manager.h; weak, as only kernels with
// suspend to idle have it): while the GPU resumes, every line goes there
// too, so that a hang there leaves its last line behind.
extern "C" void device_manager_suspend_trace(const char* format, ...)
	__attribute__((weak));
__asm__(".type device_manager_suspend_trace, @function");
static bool sSuspendTrace;


extern "C" void
lx_haiku_suspend_trace(bool enabled)
{
	sSuspendTrace = enabled;
}


extern "C" void
lx_haiku_log(const char* text)
{
	if (sSuspendTrace && device_manager_suspend_trace != NULL) {
		char line[200];
		strlcpy(line, text, sizeof(line));
		size_t length = strlen(line);
		while (length > 0 && line[length - 1] == '\n')
			line[--length] = '\0';
		if (length > 0)
			device_manager_suspend_trace("%s", line);
	}
	if (sRawThread >= 0 && are_interrupts_enabled()
		&& thread_get_current_thread_id() == sRawThread) {
		raw_log(text);
		return;
	}
	dprintf("%s", text);
	if (sLogFile >= 0 && are_interrupts_enabled()) {
		// in the file, lines begin with the time since boot ("[s.ms] ")
		static bool sLineStart = true;
		char stamp[32];
		size_t stampLength = 0;
		bigtime_t now = system_time();
		MutexLocker locker(sLogFileLock);
		size_t length = strlen(text);
		if (sLineStart) {
			stampLength = snprintf(stamp, sizeof(stamp), "[%" B_PRId64
				".%03d] ", now / 1000000, (int)(now / 1000 % 1000));
		}
		if (sLogFile >= 0
			&& sLogPosition + (off_t)(stampLength + length) <= sLogSize) {
			if (stampLength > 0) {
				_kern_write(sLogFile, sLogPosition, stamp, stampLength);
				sLogPosition += stampLength;
			}
			_kern_write(sLogFile, sLogPosition, text, length);
			sLogPosition += length;
			_kern_fsync(sLogFile, true);
			sLineStart = length > 0 && text[length - 1] == '\n';
		}
	}
}


extern "C" void
lx_haiku_panic(const char* text)
{
	// the log file is the only record that survives on the lab machine
	lx_haiku_log(text);
	panic("%s", text);
	for (;;)
		;
}


extern "C" void
lx_haiku_dump_stack(void)
{
	// The kernel debugger's "bt" needs a KDL session; leave a marker.
	dprintf(LX_DRIVER_NAME ": (stack trace not available)\n");
}


extern "C" long long
lx_haiku_system_time(void)
{
	return system_time();
}


extern "C" long long
lx_haiku_real_time(void)
{
	return real_time_clock_usecs();
}


extern "C" void
lx_haiku_snooze(long long us)
{
	snooze(us);
}


extern "C" void
lx_haiku_spin(long long us)
{
	spin(us);
}


extern "C" int
lx_haiku_cpu_count(void)
{
	return smp_get_num_cpus();
}


extern "C" int
lx_haiku_current_cpu(void)
{
	return smp_get_current_cpu();
}


extern "C" int
lx_haiku_cpu_has(int feature)
{
	// feature numbers of lx/mm.h
	switch (feature) {
		case 1: return x86_check_feature(IA32_FEATURE_CLFSH, FEATURE_COMMON);
		case 2: return x86_check_feature(IA32_FEATURE_CLFLUSHOPT, FEATURE_7_EBX);
		case 3: return x86_check_feature(IA32_FEATURE_PAT, FEATURE_COMMON);
		case 4: return x86_check_feature(IA32_FEATURE_EXT_HYPERVISOR, FEATURE_EXT);
		case 5: return x86_check_feature(IA32_FEATURE_SSE, FEATURE_COMMON);
		case 6: return x86_check_feature(IA32_FEATURE_SSE2, FEATURE_COMMON);
		case 7: return x86_check_feature(IA32_FEATURE_EXT_SSE4_1, FEATURE_EXT);
		case 8: return x86_check_feature(IA32_FEATURE_EXT_AVX, FEATURE_EXT);
	}
	return 0;
}


extern "C" void
lx_haiku_cpu_info(unsigned* family, unsigned* model, unsigned* stepping,
	unsigned* vendorIsAMD, unsigned* clflushSize)
{
	const arch_cpu_info& info = gCPU[0].arch;
	*family = info.family + info.extended_family;
	*model = info.model + (info.extended_model << 4);
	*stepping = info.stepping;
	*vendorIsAMD = info.vendor == VENDOR_AMD;
	*clflushSize = 64;
}


static void
wbinvd_on_cpu(void*, int)
{
	asm volatile("wbinvd" ::: "memory");
}


extern "C" void
lx_haiku_wbinvd_all(void)
{
	call_all_cpus_sync(wbinvd_on_cpu, NULL);
}


extern "C" void
lx_haiku_clflush_range(void* address, unsigned long size)
{
	addr_t start = (addr_t)address & ~(addr_t)63;
	addr_t end = (addr_t)address + size;
	asm volatile("mfence" ::: "memory");
	for (addr_t line = start; line < end; line += 64)
		asm volatile("clflush %0" : "+m" (*(volatile char*)line));
	asm volatile("mfence" ::: "memory");
}


extern "C" void
lx_haiku_random(void* buffer, unsigned long size)
{
	uint8* bytes = (uint8*)buffer;
	while (size > 0) {
		unsigned int value = secure_random_value();
		size_t chunk = min_c(size, sizeof(value));
		memcpy(bytes, &value, chunk);
		bytes += chunk;
		size -= chunk;
	}
}


// #pragma mark - locks


extern "C" void
lx_haiku_mutex_init(void* storage, const char* name)
{
	mutex_init((mutex*)storage, name != NULL ? name : "linux mutex");
}


extern "C" void
lx_haiku_mutex_destroy(void* storage)
{
	mutex_destroy((mutex*)storage);
}


extern "C" void
lx_haiku_mutex_lock(void* storage)
{
	mutex_lock((mutex*)storage);
}


/*	0 when locked, -ETIMEDOUT when the timeout passed first. */
extern "C" int
lx_haiku_mutex_lock_timeout(void* storage, long long timeout)
{
	return mutex_lock_with_timeout((mutex*)storage, B_RELATIVE_TIMEOUT,
		timeout) == B_OK ? 0 : -kLinuxETIMEDOUT;
}


extern "C" int
lx_haiku_mutex_trylock(void* storage)
{
	return mutex_trylock((mutex*)storage) == B_OK;
}


extern "C" void
lx_haiku_mutex_unlock(void* storage)
{
	mutex_unlock((mutex*)storage);
}


extern "C" void
lx_haiku_rwlock_init(void* storage, const char* name)
{
	rw_lock_init((rw_lock*)storage, name != NULL ? name : "linux rwsem");
}


extern "C" void
lx_haiku_rwlock_destroy(void* storage)
{
	rw_lock_destroy((rw_lock*)storage);
}


extern "C" void
lx_haiku_rwlock_read_lock(void* storage)
{
	rw_lock_read_lock((rw_lock*)storage);
}


extern "C" int
lx_haiku_rwlock_read_trylock(void* storage)
{
	return rw_lock_read_lock_with_timeout((rw_lock*)storage, B_RELATIVE_TIMEOUT,
		0) == B_OK;
}


extern "C" void
lx_haiku_rwlock_read_unlock(void* storage)
{
	rw_lock_read_unlock((rw_lock*)storage);
}


extern "C" void
lx_haiku_rwlock_write_lock(void* storage)
{
	rw_lock_write_lock((rw_lock*)storage);
}


extern "C" int
lx_haiku_rwlock_write_trylock(void* storage)
{
	rw_lock* lock = (rw_lock*)storage;
	// Haiku has no timed write lock; a write lock is free when nobody holds
	// or waits for it, which only the lock's own spinlock can tell.
	InterruptsSpinLocker locker(lock->lock);
	if (lock->holder >= 0 || lock->count != 0 || lock->waiters != NULL)
		return 0;
	locker.Unlock();
	// A racing reader or writer can still get in first; then this waits
	// briefly, which Linux's callers tolerate.
	rw_lock_write_lock(lock);
	return 1;
}


extern "C" void
lx_haiku_rwlock_write_unlock(void* storage)
{
	rw_lock_write_unlock((rw_lock*)storage);
}


extern "C" void
lx_haiku_debug_console(void* base, int width, int height, int bytesPerRow)
{
	if (base == NULL)
		frame_buffer_update(0, 0, 0, 32, 0);
	else
		frame_buffer_update((addr_t)base, width, height, 32, bytesPerRow);
}


static spinlock sGlobalLock = B_SPINLOCK_INITIALIZER;


extern "C" unsigned long
lx_haiku_global_lock(void)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sGlobalLock);
	return state;
}


extern "C" void
lx_haiku_global_unlock(unsigned long state)
{
	release_spinlock(&sGlobalLock);
	restore_interrupts((cpu_status)state);
}


// #pragma mark - events


struct LinuxEvent {
	spinlock			lock;
	bool				signalled;
	ConditionVariable	condition;

	LinuxEvent()
		:
		signalled(false)
	{
		B_INITIALIZE_SPINLOCK(&lock);
		condition.Init(this, "linux event");
	}
};


extern "C" void*
lx_haiku_event_create(void)
{
	return new(std::nothrow) LinuxEvent;
}


extern "C" void
lx_haiku_event_delete(void* event)
{
	delete (LinuxEvent*)event;
}


extern "C" void
lx_haiku_event_signal(void* _event)
{
	LinuxEvent* event = (LinuxEvent*)_event;
	InterruptsSpinLocker locker(event->lock);
	event->signalled = true;
	event->condition.NotifyAll();
}


extern "C" int
lx_haiku_event_wait(void* _event, long long timeout)
{
	return lx_haiku_event_wait_etc(_event, timeout, LX_HAIKU_WAIT_UNINTERRUPTIBLE);
}


/*	Like lx_haiku_event_wait(); a wait that signals may interrupt returns
	-EINTR when one does. */
extern "C" int
lx_haiku_event_wait_etc(void* _event, long long timeout, int interrupt)
{
	LinuxEvent* event = (LinuxEvent*)_event;
	uint32 flags = interrupt == LX_HAIKU_WAIT_INTERRUPTIBLE ? B_CAN_INTERRUPT
		: interrupt == LX_HAIKU_WAIT_KILLABLE ? B_KILL_CAN_INTERRUPT : 0;

	InterruptsSpinLocker locker(event->lock);
	if (event->signalled) {
		event->signalled = false;
		return 0;
	}
	if (timeout == 0)
		return -kLinuxETIMEDOUT;

	ConditionVariableEntry entry;
	event->condition.Add(&entry);
	locker.Unlock();

	status_t status;
	if (timeout < 0)
		status = entry.Wait(flags);
	else
		status = entry.Wait(flags | B_RELATIVE_TIMEOUT, timeout);

	locker.Lock();
	if (event->signalled) {
		event->signalled = false;
		return 0;
	}
	return status == B_INTERRUPTED ? -kLinuxEINTR : -kLinuxETIMEDOUT;
}


/*	Whether a signal is pending for the current thread that interrupts a
	wait (any one, or only a kill when fatalOnly). Kernel threads have none. */
extern "C" int
lx_haiku_signal_pending(int fatalOnly)
{
	Thread* thread = thread_get_current_thread();
	if (thread->team == team_get_kernel_team())
		return 0;
	return thread_is_interrupted(thread,
		fatalOnly ? B_KILL_CAN_INTERRUPT : B_CAN_INTERRUPT) ? 1 : 0;
}


extern "C" void __cyg_profile_func_enter(void* function, void* site);


/*	Not exported by every kernel (Haiku R1/beta6's lacks it): weak, so that
	the driver loads without it and logs no stack there. */
extern "C" int32 arch_get_stack_trace(addr_t* returnAddresses, int32 maxCount,
	int32 skipIframes, int32 skipMore, uint32 flags) __attribute__((weak));


/*	Logs the current thread's kernel stack, return addresses after a header
	with the address of the trace anchor (see compat/src/trace.c), which
	tools/symbolize-trace.sh turns into names. */
extern "C" void
lx_haiku_log_stack(const char* what)
{
	addr_t addresses[24];
	int32 count = arch_get_stack_trace != NULL
		? arch_get_stack_trace(addresses, 24, 0, 1, STACK_TRACE_KERNEL) : 0;
	char line[192];
	snprintf(line, sizeof(line), "stack of thread %" B_PRId32 " (%s), anchor "
		"%#lx: %s\n", thread_get_current_thread_id(),
		thread_get_current_thread()->name,
		(addr_t)&__cyg_profile_func_enter, what);
	lx_haiku_log(line);
	for (int32 i = 0; i < count; i++) {
		snprintf(line, sizeof(line), "  %#lx\n", addresses[i]);
		lx_haiku_log(line);
	}
}


/*	For the watchdog: the kernel's free pages and the threads, teams, ports
	and semaphores in use. */
extern "C" int
lx_haiku_resource_stats(char* buffer, int size)
{
	system_info info;
	if (get_system_info(&info) != B_OK)
		return 0;
	return snprintf(buffer, size, "free %" B_PRIu64 " MiB threads %" B_PRIu32
		" teams %" B_PRIu32 " ports %" B_PRIu32 " sems %" B_PRIu32,
		(uint64)(info.max_pages - info.used_pages) * B_PAGE_SIZE / 1048576,
		info.used_threads, info.used_teams, info.used_ports, info.used_sems);
}


/*	For the watchdog: one line about thread \a id (name, state, CPU, what
	it is blocked on) and, while it is blocked, its kernel stack, which a
	blocked thread leaves alone (frame pointers, bounded by the stack). */
extern "C" void
lx_haiku_log_thread(lx_thread_id id, const char* what)
{
	char line[192];
	Thread* thread = Thread::Get(id);
	if (thread == NULL) {
		snprintf(line, sizeof(line), "  %s: thread %" B_PRId32 " is gone\n",
			what, (thread_id)id);
		lx_haiku_log(line);
		return;
	}
	int32 state = thread->state;
	uint32 type = thread->wait.type;
	const void* object = thread->wait.object;
	cpu_ent* cpu = thread->cpu;
	// a semaphore by name: looked up by its ID, which is safe also when the
	// thread wakes meanwhile (unlike reading a lock or condition variable)
	char objectName[B_OS_NAME_LENGTH] = "";
	sem_info info;
	if (state == B_THREAD_WAITING && type == THREAD_BLOCK_TYPE_SEMAPHORE
		&& get_sem_info((sem_id)(addr_t)object, &info) == B_OK) {
		strlcpy(objectName, info.name, sizeof(objectName));
	}
	snprintf(line, sizeof(line), "  %s: thread %" B_PRId32 " (%s) state %"
		B_PRId32 " cpu %" B_PRId32 " waits (type %" B_PRIu32 ") on %p %s, "
		"anchor %#lx\n", what, thread->id, thread->name, state,
		cpu != NULL ? cpu->cpu_num : -1, type, object, objectName,
		(addr_t)&__cyg_profile_func_enter);
	lx_haiku_log(line);
	if (state == B_THREAD_WAITING && thread != thread_get_current_thread()) {
		addr_t base = thread->kernel_stack_base;
		addr_t top = thread->kernel_stack_top;
		addr_t frame = thread->arch_info.GetFramePointer();
		int length = 0;
		line[0] = '\0';
		for (int i = 0; i < 20; i++) {
			if (frame < base || frame + 16 > top || (frame & 7) != 0)
				break;
			addr_t next = ((addr_t*)frame)[0];
			addr_t ip = ((addr_t*)frame)[1];
			length += snprintf(line + length, sizeof(line) - length, " %#lx",
				ip);
			if (length > (int)sizeof(line) - 24) {
				strlcat(line, "\n", sizeof(line));
				lx_haiku_log(line);
				length = 0;
				line[0] = '\0';
			}
			if (next <= frame)
				break;
			frame = next;
		}
		if (length > 0) {
			strlcat(line, "\n", sizeof(line));
			lx_haiku_log(line);
		}
	}
	thread->ReleaseReference();
}


/*	How much has gone to the log file: grows while anything is logged. */
extern "C" int64
lx_haiku_log_position(void)
{
	return sLogPosition;
}


// #pragma mark - threads


extern "C" lx_thread_id
lx_haiku_current_thread(void)
{
	return thread_get_current_thread_id();
}


extern "C" lx_thread_id
lx_haiku_current_team(void)
{
	return team_get_current_team_id();
}


extern "C" void
lx_haiku_team_name(lx_thread_id team, char* buffer, unsigned long size)
{
	team_info info;
	if (get_team_info(team, &info) == B_OK) {
		// the first word of the command line, without its directory
		char* args = info.args;
		char* end = strchr(args, ' ');
		if (end != NULL)
			*end = '\0';
		const char* name = strrchr(args, '/');
		strlcpy(buffer, name != NULL ? name + 1 : args, size);
	} else
		strlcpy(buffer, "kernel", size);
}


extern "C" void
lx_haiku_thread_name(lx_thread_id thread, char* buffer, unsigned long size)
{
	thread_info info;
	if (get_thread_info(thread, &info) == B_OK)
		strlcpy(buffer, info.name, size);
	else
		strlcpy(buffer, "?", size);
}


extern "C" lx_thread_id
lx_haiku_spawn(int (*entry)(void*), const char* name, int priority,
	void* argument)
{
	return spawn_kernel_thread((thread_func)entry, name, priority, argument);
}


extern "C" void
lx_haiku_resume(lx_thread_id thread)
{
	resume_thread(thread);
}


extern "C" void
lx_haiku_wait_for_thread(lx_thread_id thread)
{
	status_t result;
	wait_for_thread(thread, &result);
}


extern "C" void
lx_haiku_set_priority(lx_thread_id thread, int priority)
{
	set_thread_priority(thread, priority);
}


extern "C" void
lx_haiku_yield(void)
{
	thread_yield();
}


// #pragma mark - tasks of threads


/*	thread_id -> Linux task: a hash table with chains, under a spinlock.
	Lookups happen on every "current", so the locked part is a short chain
	walk; nodes are allocated and freed outside the lock. (It was an open
	addressing table whose deleted slots never became free again: once 8192
	threads had come and gone, every lookup of a new thread walked the whole
	table with interrupts disabled, and a full table left new threads
	unregistered, getting a new task at every "current".) */

struct TaskNode {
	TaskNode*	next;
	thread_id	thread;
	void*		task;
	bool		owned;
		// someone else frees the task (a kthread's: kthread_stop()); the
		// thread's end leaves it alone
};

static const uint32 kTaskBuckets = 1024;
static TaskNode* sTaskBuckets[kTaskBuckets];
static spinlock sTaskLock = B_SPINLOCK_INITIALIZER;
static void (*sTaskExitHook)(void*) = NULL;


static inline uint32
task_hash(thread_id thread)
{
	return ((uint32)thread * 2654435761u) % kTaskBuckets;
}


extern "C" void*
lx_haiku_task_lookup(lx_thread_id thread)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTaskLock);
	void* task = NULL;
	for (TaskNode* node = sTaskBuckets[task_hash(thread)]; node != NULL;
			node = node->next) {
		if (node->thread == thread) {
			task = node->task;
			break;
		}
	}
	release_spinlock(&sTaskLock);
	restore_interrupts(state);
	return task;
}


static int
task_register(lx_thread_id thread, void* task, bool owned)
{
	TaskNode* node = (TaskNode*)malloc(sizeof(TaskNode));
	if (node == NULL)
		return -kLinuxENOMEM;
	node->thread = thread;
	node->task = task;
	node->owned = owned;

	TaskNode* replaced = NULL;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTaskLock);
	TaskNode** link = &sTaskBuckets[task_hash(thread)];
	for (; *link != NULL; link = &(*link)->next) {
		if ((*link)->thread == thread) {
			replaced = *link;
			*link = replaced->next;
			break;
		}
	}
	node->next = sTaskBuckets[task_hash(thread)];
	sTaskBuckets[task_hash(thread)] = node;
	release_spinlock(&sTaskLock);
	restore_interrupts(state);
	free(replaced);
	return 0;
}


extern "C" int
lx_haiku_task_register(lx_thread_id thread, void* task)
{
	return task_register(thread, task, false);
}


/*!	The task of a thread whose end is someone else's to deal with: when the
	thread ends, the task exit hook is not called for it, and the task stays
	registered until lx_haiku_task_unregister().

	A kthread's task is freed by kthread_stop(), which waits for the thread
	first - but wait_for_thread() returns at once, with an error, for a
	thread of another team that is already out of the thread table and
	still on its way out. Its end was then noticed after kthread_stop() had
	freed the task: the hook looked into freed memory to see whose task it
	was, took it for its own and freed it and its event a second time.
*/
extern "C" int
lx_haiku_task_register_owned(lx_thread_id thread, void* task)
{
	return task_register(thread, task, true);
}


/*!	The thread has ended: its task, taken out of the table - unless the
	task is someone else's to free.
*/
static void*
task_take_ended(thread_id thread)
{
	TaskNode* removed = NULL;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTaskLock);
	for (TaskNode** link = &sTaskBuckets[task_hash(thread)]; *link != NULL;
			link = &(*link)->next) {
		if ((*link)->thread == thread) {
			if (!(*link)->owned) {
				removed = *link;
				*link = removed->next;
			}
			break;
		}
	}
	release_spinlock(&sTaskLock);
	restore_interrupts(state);
	if (removed == NULL)
		return NULL;
	void* task = removed->task;
	free(removed);
	return task;
}


extern "C" void
lx_haiku_task_unregister(lx_thread_id thread)
{
	TaskNode* removed = NULL;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sTaskLock);
	for (TaskNode** link = &sTaskBuckets[task_hash(thread)]; *link != NULL;
			link = &(*link)->next) {
		if ((*link)->thread == thread) {
			removed = *link;
			*link = removed->next;
			break;
		}
	}
	release_spinlock(&sTaskLock);
	restore_interrupts(state);
	free(removed);
}


static status_t
task_test_thread(void* data)
{
	void* (*getCurrent)(void) = (void* (*)(void))data;
	return getCurrent() != NULL ? B_OK : B_ERROR;
}


/*	Self test: a thread that got a task ("current") and ended has none in
	the table any more. 0 if so. */
extern "C" int
lx_haiku_selftest_task_exit(void* (*getCurrent)(void))
{
	thread_id thread = spawn_kernel_thread(task_test_thread, "lx task test",
		B_NORMAL_PRIORITY, (void*)getCurrent);
	if (thread < 0)
		return -1;
	resume_thread(thread);
	status_t result;
	if (wait_for_thread(thread, &result) != B_OK || result != B_OK)
		return -1;
	return lx_haiku_task_lookup(thread) == NULL ? 0 : -1;
}


class TaskExitListener : public NotificationListener {
public:
	virtual void EventOccurred(NotificationService& service,
		const KMessage* event)
	{
		int32 eventCode;
		thread_id thread;
		if (event->FindInt32("event", &eventCode) != B_OK
			|| eventCode != THREAD_REMOVED) {
			return;
		}
		// The thread itself, not the "thread" field: Haiku's
		// ThreadNotificationService::Notify(uint32, Thread*) passes the
		// thread and team IDs the wrong way round (R1/beta6 included), so
		// "thread" is the team's ID - the main thread's, whose task was then
		// freed while it ran, and no other thread's task ever was.
		Thread* threadStruct = NULL;
		if (event->FindPointer("threadStruct", (void**)&threadStruct) == B_OK
			&& threadStruct != NULL) {
			thread = threadStruct->id;
		} else if (event->FindInt32("thread", &thread) != B_OK)
			return;
		void* task = task_take_ended(thread);
		if (task != NULL && sTaskExitHook != NULL)
			sTaskExitHook(task);
	}
};


static TaskExitListener* sTaskExitListener = NULL;


extern "C" void
lx_haiku_set_task_exit_hook(void (*hook)(void*))
{
	sTaskExitHook = hook;
	if (hook != NULL && sTaskExitListener == NULL) {
		sTaskExitListener = new(std::nothrow) TaskExitListener;
		if (sTaskExitListener != NULL) {
			NotificationManager::Manager().AddListener("threads",
				THREAD_REMOVED, *sTaskExitListener);
		}
	} else if (hook == NULL && sTaskExitListener != NULL) {
		NotificationManager::Manager().RemoveListener("threads", NULL,
			*sTaskExitListener);
		delete sTaskExitListener;
		sTaskExitListener = NULL;
	}
}


// #pragma mark - kernel heap and areas


extern "C" void*
lx_haiku_malloc(unsigned long size)
{
	return malloc(size);
}


extern "C" void*
lx_haiku_realloc(void* pointer, unsigned long size)
{
	return realloc(pointer, size);
}


extern "C" void
lx_haiku_free(void* pointer)
{
	free(pointer);
}


static const char* kAreaName = "linux vmalloc";


extern "C" void*
lx_haiku_area_alloc(unsigned long size, int clear)
{
	void* address = NULL;
	area_id area = create_area(kAreaName, &address, B_ANY_KERNEL_ADDRESS,
		ROUNDUP(size, B_PAGE_SIZE), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (area < 0)
		return NULL;
	if (clear)
		memset(address, 0, size);
	return address;
}


extern "C" int
lx_haiku_is_area_alloc(const void* address)
{
	area_id area = area_for((void*)address);
	if (area < 0)
		return 0;
	area_info info;
	if (get_area_info(area, &info) != B_OK)
		return 0;
	return strcmp(info.name, kAreaName) == 0 && info.address == address;
}


extern "C" int
lx_haiku_area_free(void* address)
{
	if (!lx_haiku_is_area_alloc(address))
		return -kLinuxENOENT;
	delete_area(area_for(address));
	return 0;
}


// #pragma mark - physical pages


extern "C" void*
lx_haiku_pages_alloc(unsigned long count, unsigned long long maxAddress,
	int clear, unsigned long long* physical)
{
	vm_page* page;
	uint32 flags = PAGE_STATE_WIRED | (clear ? VM_PAGE_ALLOC_CLEAR : 0);
	if (count == 1 && maxAddress == 0) {
		vm_page_reservation reservation;
		if (!vm_page_try_reserve_pages(&reservation, 1, VM_PRIORITY_SYSTEM))
			return NULL;
		page = vm_page_allocate_page(&reservation, flags);
		vm_page_unreserve_pages(&reservation);
	} else {
		physical_address_restrictions restrictions = {};
		restrictions.high_address = maxAddress;
		// Linux allocations of order n are aligned to their size
		restrictions.alignment = count * B_PAGE_SIZE;
		if ((count & (count - 1)) != 0)
			restrictions.alignment = 0;
		page = vm_page_allocate_page_run(flags, count, &restrictions,
			VM_PRIORITY_SYSTEM);
	}
	if (page == NULL)
		return NULL;
	// With DEBUG_PAGE_ACCESS (KDEBUG_LEVEL 2), a page belongs to the thread
	// that allocated it until that thread ends its access; Linux frees pages
	// on any thread (TTM destroys busy buffers on a worker, for instance).
	for (unsigned long i = 0; i < count; i++)
		DEBUG_PAGE_ACCESS_END(&page[i]);
	*physical = (unsigned long long)page->physical_page_number * B_PAGE_SIZE;
	return page;
}


extern "C" void
lx_haiku_pages_free(void* firstPage, unsigned long count)
{
	vm_page* page = (vm_page*)firstPage;
	page_num_t first = page->physical_page_number;
	for (unsigned long i = 0; i < count; i++) {
		vm_page* current = i == 0 ? page : vm_lookup_page(first + i);
		if (current == NULL)
			continue;
		DEBUG_PAGE_ACCESS_START(current);
		vm_page_free(NULL, current);
	}
}


extern "C" void*
lx_haiku_page_run_alloc(unsigned long count, unsigned long long alignment,
	unsigned long long* physical)
{
	physical_address_restrictions restrictions = {};
	restrictions.alignment = alignment;
	// user priority: a large run must not eat into the pages the system
	// keeps for itself
	vm_page* page = vm_page_allocate_page_run(
		PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR, count, &restrictions,
		VM_PRIORITY_USER);
	if (page == NULL)
		return NULL;
	// see lx_haiku_pages_alloc()
	for (unsigned long i = 0; i < count; i++)
		DEBUG_PAGE_ACCESS_END(&page[i]);
	*physical = (unsigned long long)page->physical_page_number * B_PAGE_SIZE;
	return page;
}


extern "C" void
lx_haiku_page_run_free(void* run, unsigned long count)
{
	lx_haiku_pages_free(run, count);
}


extern "C" unsigned int
lx_haiku_current_euid(void)
{
	return geteuid();
}


extern "C" unsigned long long
lx_haiku_total_pages(void)
{
	return vm_page_num_pages();
}


extern "C" unsigned long long
lx_haiku_free_pages(void)
{
	return vm_page_num_free_pages();
}


extern "C" unsigned long long
lx_haiku_ram_frames(void)
{
	return (vm_page_max_address() + 1) / B_PAGE_SIZE;
}


// #pragma mark - mappings


static uint32
memory_type(int type)
{
	switch (type) {
		case LX_HAIKU_MAP_WC:
			return B_WRITE_COMBINING_MEMORY;
		case LX_HAIKU_MAP_WB:
			return B_WRITE_BACK_MEMORY;
		default:
			return B_UNCACHED_MEMORY;
	}
}


extern "C" void*
lx_haiku_map_physical(unsigned long long physical, unsigned long size,
	int type)
{
	phys_addr_t base = physical & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t offset = physical - base;
	void* address = NULL;
	area_id area = map_physical_memory("linux ioremap", base,
		ROUNDUP(size + offset, B_PAGE_SIZE),
		B_ANY_KERNEL_ADDRESS | memory_type(type),
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return NULL;
	return (uint8*)address + offset;
}


extern "C" void
lx_haiku_unmap_physical(void* address)
{
	area_id area = area_for(address);
	if (area >= 0)
		delete_area(area);
}


extern "C" void*
lx_haiku_vmap(const unsigned long long* physical, unsigned long count,
	int type)
{
	generic_io_vec* vecs = (generic_io_vec*)malloc(sizeof(generic_io_vec)
		* count);
	if (vecs == NULL)
		return NULL;
	for (unsigned long i = 0; i < count; i++) {
		vecs[i].base = physical[i];
		vecs[i].length = B_PAGE_SIZE;
	}
	void* address = NULL;
	addr_t size = count * B_PAGE_SIZE;
	// Older kernels map scattered pages only with the default memory type
	area_id area = vm_map_physical_memory_vecs(VMAddressSpace::KernelID(),
		"linux vmap", &address, B_ANY_KERNEL_ADDRESS | memory_type(type),
		&size, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, vecs, count);
	if (area == B_NOT_SUPPORTED) {
		address = NULL;
		size = count * B_PAGE_SIZE;
		area = vm_map_physical_memory_vecs(VMAddressSpace::KernelID(),
			"linux vmap", &address, B_ANY_KERNEL_ADDRESS, &size,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, vecs, count);
	}
	free(vecs);
	if (area < 0)
		return NULL;
	return address;
}


extern "C" void
lx_haiku_vunmap(void* address)
{
	area_id area = area_for(address);
	if (area >= 0)
		delete_area(area);
}


extern "C" unsigned long long
lx_haiku_virt_to_phys(const void* address)
{
	addr_t virt = (addr_t)address;
	if (virt >= LX_HAIKU_PMAP_BASE
		&& virt < LX_HAIKU_PMAP_BASE + KERNEL_PMAP_SIZE) {
		return virt - LX_HAIKU_PMAP_BASE;
	}
	physical_entry entry;
	if (get_memory_map(address, 1, &entry, 1) != B_OK)
		return 0;
	return entry.address;
}


// #pragma mark - team memory


extern "C" int
lx_haiku_user_memcpy(void* to, const void* from, unsigned long size)
{
	if (size == 0)
		return 0;
	if (!IS_USER_ADDRESS(to) && !IS_USER_ADDRESS(from)) {
		// Linux calls copy_{from,to}_user() with kernel buffers too
		// (ioctls the kernel issues itself)
		memcpy(to, from, size);
		return 0;
	}
	return user_memcpy(to, from, size) == B_OK ? 0 : -kLinuxEFAULT;
}


extern "C" long
lx_haiku_user_strlcpy(char* to, const char* from, unsigned long size)
{
	if (!IS_USER_ADDRESS(from))
		return strlcpy(to, from, size);
	ssize_t length = user_strlcpy(to, from, size);
	return length < 0 ? -kLinuxEFAULT : length;
}


extern "C" int
lx_haiku_is_user_address(const void* address, unsigned long size)
{
	return is_user_address_range(address, size);
}


/*	Whether [address, address + size) of the address space (read locked) is
	all in areas of RAM: anonymous memory (also cloned or copied on write)
	and, unless anonOnly, mapped files. Never device memory, which a device
	must not be told is RAM. */
static int
check_user_ram(VMAddressSpace* addressSpace, addr_t address, size_t size,
	bool anonOnly)
{
	addr_t end = address + size;
	while (address < end) {
		VMArea* area = addressSpace->LookupArea(address);
		if (area == NULL)
			return -kLinuxEFAULT;

		int error = 0;
		VMCache* top = vm_area_get_locked_cache(area);
		VMCache* cache = top;
		while (true) {
			if (cache->type == CACHE_TYPE_VNODE ? anonOnly
					: cache->type != CACHE_TYPE_RAM) {
				error = cache->type == CACHE_TYPE_VNODE
					? -kLinuxEPERM : -kLinuxEFAULT;
				break;
			}
			VMCache* source = cache->source;
			if (source == NULL)
				break;
			source->Lock();
			source->AcquireRefLocked();
			if (cache != top)
				cache->ReleaseRefAndUnlock();
			cache = source;
		}
		if (cache != top)
			cache->ReleaseRefAndUnlock();
		vm_area_put_locked_cache(top);
		if (error != 0)
			return error;

		address = area->Base() + area->Size();
	}
	return 0;
}


/*	Wires \a count pages of the calling team from \a address for a device,
	as nvidia_rm's os_lock_user_pages() does, writable if the device writes
	into them, and fills \a physical with their addresses. Haiku moves no
	wired page, so they stay where they are until
	lx_haiku_user_pages_unlock(). That must come before the team's address
	space goes away: a team's areas cannot be deleted while wired. */
extern "C" int
lx_haiku_user_pages_lock(unsigned long address, unsigned long count,
	int writable, int anonOnly, lx_thread_id* _team,
	unsigned long long* physical)
{
	size_t size = count * B_PAGE_SIZE;
	if (count == 0 || size / B_PAGE_SIZE != count
		|| address % B_PAGE_SIZE != 0
		|| !is_user_address_range((void*)address, size))
		return -kLinuxEFAULT;

	team_id team = team_get_current_team_id();
	VMAddressSpace* addressSpace = VMAddressSpace::Get(team);
	if (addressSpace == NULL)
		return -kLinuxESRCH;
	addressSpace->ReadLock();
	int error = check_user_ram(addressSpace, address, size, anonOnly != 0);
	addressSpace->ReadUnlock();
	addressSpace->Put();
	if (error != 0)
		return error;

	uint32 flags = writable ? B_READ_DEVICE : 0;
	status_t status = lock_memory_etc(team, (void*)address, size, flags);
	if (status != B_OK)
		return status == B_NO_MEMORY ? -kLinuxENOMEM : -kLinuxEFAULT;

	unsigned long done = 0;
	while (done < count) {
		physical_entry table[64];
		uint32 entries = B_COUNT_OF(table);
		status = get_memory_map_etc(team,
			(void*)(address + done * B_PAGE_SIZE),
			(count - done) * B_PAGE_SIZE, table, &entries);
		if (status != B_OK && status != B_BUFFER_OVERFLOW)
			break;
		for (uint32 i = 0; i < entries; i++) {
			for (phys_size_t offset = 0;
					offset < table[i].size && done < count;
					offset += B_PAGE_SIZE)
				physical[done++] = table[i].address + offset;
		}
		if (status == B_OK)
			break;
	}
	if (done != count) {
		unlock_memory_etc(team, (void*)address, size, flags);
		return -kLinuxEFAULT;
	}

	*_team = team;
	return 0;
}


extern "C" void
lx_haiku_user_pages_unlock(lx_thread_id team, unsigned long address,
	unsigned long count, int writable)
{
	status_t status = unlock_memory_etc(team, (void*)address,
		count * B_PAGE_SIZE, writable ? B_READ_DEVICE : 0);
	if (status != B_OK) {
		panic("linux compat: cannot unwire %lu pages at %#lx of team %"
			B_PRId32 ": %s", count, address, team, strerror(status));
	}
}


/*	\a hook runs when the team goes away: at the start of its deletion, while
	its address space still exists (and with the team locked). */
extern "C" int
lx_haiku_team_watch(lx_thread_id team, void (*hook)(lx_thread_id, void*),
	void* data)
{
	status_t status = start_watching_team(team, hook, data);
	if (status == B_OK)
		return 0;
	return status == B_NO_MEMORY ? -kLinuxENOMEM : -kLinuxESRCH;
}


/* 0 if the hook was removed before it ran */
extern "C" int
lx_haiku_team_unwatch(lx_thread_id team, void (*hook)(lx_thread_id, void*),
	void* data)
{
	return stop_watching_team(team, hook, data) == B_OK ? 0 : -kLinuxENOENT;
}


// #pragma mark - files


extern "C" int
lx_haiku_read_file(const char* path, void** _data, unsigned long* _size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -kLinuxENOENT;
	struct stat st;
	if (fstat(fd, &st) != 0 || st.st_size <= 0) {
		close(fd);
		return -kLinuxEIO;
	}
	void* data = malloc(st.st_size);
	if (data == NULL) {
		close(fd);
		return -kLinuxENOMEM;
	}
	ssize_t bytesRead = read(fd, data, st.st_size);
	close(fd);
	if (bytesRead != st.st_size) {
		free(data);
		return -kLinuxEIO;
	}
	*_data = data;
	*_size = st.st_size;
	return 0;
}
