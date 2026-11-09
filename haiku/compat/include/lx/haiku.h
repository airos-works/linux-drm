/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_HAIKU_H
#define _LX_HAIKU_H

/*	The Haiku kernel services the Linux compat layer is built on, as plain C
	functions (haiku/compat/haiku/Haiku.cpp). Included by both sides: only
	basic C types, no Linux and no Haiku headers. Status results are 0 for
	success, Linux error numbers (negative) otherwise. */

#ifdef __cplusplus
extern "C" {
#endif

typedef __INT32_TYPE__ lx_thread_id;

/* output */
void lx_haiku_log(const char *text);
/* bring-up: also append every message to a file, synced per message */
int lx_haiku_log_file(const char *path);
/* the kernel debugger's on-screen console (frame_buffer_update()): a frame
 * buffer of 32-bit pixels in kernel memory, NULL for none */
void lx_haiku_debug_console(void *base, int width, int height,
	int bytes_per_row);
void lx_haiku_panic(const char *text) __attribute__((noreturn));
void lx_haiku_dump_stack(void);

/* time, in microseconds */
long long lx_haiku_system_time(void);		/* since boot */
long long lx_haiku_real_time(void);			/* since 1970 */
void lx_haiku_snooze(long long us);
void lx_haiku_spin(long long us);

/* processors */
int lx_haiku_cpu_count(void);
int lx_haiku_current_cpu(void);
int lx_haiku_cpu_has(int feature);
void lx_haiku_cpu_info(unsigned *family, unsigned *model, unsigned *stepping,
	unsigned *vendor_is_amd, unsigned *clflush_size);
void lx_haiku_wbinvd_all(void);
void lx_haiku_clflush_range(void *address, unsigned long size);
void lx_haiku_random(void *buffer, unsigned long size);

/* mutexes and read/write locks, in caller storage */
#define LX_HAIKU_MUTEX_SIZE		48
#define LX_HAIKU_RWLOCK_SIZE	80
void lx_haiku_mutex_init(void *mutex, const char *name);
void lx_haiku_mutex_destroy(void *mutex);
void lx_haiku_mutex_lock(void *mutex);
int lx_haiku_mutex_trylock(void *mutex);		/* 1 when locked */
void lx_haiku_mutex_unlock(void *mutex);
void lx_haiku_rwlock_init(void *lock, const char *name);
void lx_haiku_rwlock_destroy(void *lock);
void lx_haiku_rwlock_read_lock(void *lock);
int lx_haiku_rwlock_read_trylock(void *lock);
void lx_haiku_rwlock_read_unlock(void *lock);
void lx_haiku_rwlock_write_lock(void *lock);
int lx_haiku_rwlock_write_trylock(void *lock);
void lx_haiku_rwlock_write_unlock(void *lock);

/* a global interrupt-safe spinlock for very short sections */
unsigned long lx_haiku_global_lock(void);
void lx_haiku_global_unlock(unsigned long state);

/* binary events: signal() sets the event, wait() takes it or blocks */
void *lx_haiku_event_create(void);
void lx_haiku_event_delete(void *event);
void lx_haiku_event_signal(void *event);
/* timeout < 0: forever. 0 when signalled, -ETIMEDOUT otherwise */
int lx_haiku_event_wait(void *event, long long timeout_us);
#define LX_HAIKU_WAIT_UNINTERRUPTIBLE	0
#define LX_HAIKU_WAIT_KILLABLE			1
#define LX_HAIKU_WAIT_INTERRUPTIBLE		2
int lx_haiku_event_wait_etc(void *event, long long timeout_us, int interrupt);
int lx_haiku_signal_pending(int fatal_only);
void lx_haiku_log_stack(const char *what);
void lx_haiku_log_thread(lx_thread_id id, const char *what);
int lx_haiku_resource_stats(char *buffer, int size);
int lx_haiku_selftest_task_exit(void *(*get_current)(void));
int lx_haiku_mutex_lock_timeout(void *mutex, long long timeout_us);

/* threads */
#define LX_HAIKU_KERNEL_TEAM	1	/* B_SYSTEM_TEAM */
lx_thread_id lx_haiku_current_thread(void);
lx_thread_id lx_haiku_current_team(void);
void lx_haiku_team_name(lx_thread_id team, char *buffer, unsigned long size);
void lx_haiku_thread_name(lx_thread_id thread, char *buffer, unsigned long size);
/* made suspended; resume() starts it */
lx_thread_id lx_haiku_spawn(int (*entry)(void *), const char *name,
	int priority, void *argument);
void lx_haiku_resume(lx_thread_id thread);
void lx_haiku_wait_for_thread(lx_thread_id thread);
void lx_haiku_set_priority(lx_thread_id thread, int priority);
void lx_haiku_yield(void);
#define LX_HAIKU_PRIORITY_NORMAL	10
#define LX_HAIKU_PRIORITY_DISPLAY	15
#define LX_HAIKU_PRIORITY_URGENT	20
#define LX_HAIKU_PRIORITY_REALTIME	100

/* the Linux task of each thread; the exit hook runs when a thread with a
   task dies */
void *lx_haiku_task_lookup(lx_thread_id thread);
int lx_haiku_task_register(lx_thread_id thread, void *task);
void lx_haiku_task_unregister(lx_thread_id thread);
void lx_haiku_set_task_exit_hook(void (*hook)(void *task));

/* kernel heap */
void *lx_haiku_malloc(unsigned long size);
void *lx_haiku_realloc(void *pointer, unsigned long size);
void lx_haiku_free(void *pointer);

/* locked, virtually contiguous kernel memory */
void *lx_haiku_area_alloc(unsigned long size, int clear);
int lx_haiku_area_free(void *address);			/* 0 if it was one */
int lx_haiku_is_area_alloc(const void *address);

/* physical pages: a run of "count" contiguous pages below max_address
   (0: anywhere). Returns Haiku's first vm_page, NULL on failure. */
void *lx_haiku_pages_alloc(unsigned long count, unsigned long long max_address,
	int clear, unsigned long long *physical);
void lx_haiku_pages_free(void *first_page, unsigned long count);
unsigned long long lx_haiku_total_pages(void);
unsigned long long lx_haiku_free_pages(void);
unsigned long long lx_haiku_ram_frames(void);	/* frame numbers below end of RAM */

/* physical memory in the kernel address space */
#define LX_HAIKU_MAP_UC	0
#define LX_HAIKU_MAP_WC	1
#define LX_HAIKU_MAP_WB	2
void *lx_haiku_map_physical(unsigned long long physical, unsigned long size,
	int type);
void lx_haiku_unmap_physical(void *address);
/* pages at physical[i] mapped contiguously */
void *lx_haiku_vmap(const unsigned long long *physical, unsigned long count,
	int type);
void lx_haiku_vunmap(void *address);
unsigned long long lx_haiku_virt_to_phys(const void *address);
#define LX_HAIKU_PMAP_BASE	0xffffff0000000000UL

/* team memory */
int lx_haiku_user_memcpy(void *to, const void *from, unsigned long size);
long lx_haiku_user_strlcpy(char *to, const char *from, unsigned long size);
int lx_haiku_is_user_address(const void *address, unsigned long size);
/* "count" pages of the calling team wired for a device (only RAM; no
   mapped files if anon_only), their addresses in physical[]; a Linux error */
int lx_haiku_user_pages_lock(unsigned long address, unsigned long count,
	int writable, int anon_only, lx_thread_id *team,
	unsigned long long *physical);
void lx_haiku_user_pages_unlock(lx_thread_id team, unsigned long address,
	unsigned long count, int writable);
/* the hook runs as the team is deleted, before its address space goes */
int lx_haiku_team_watch(lx_thread_id team, void (*hook)(lx_thread_id, void *),
	void *data);
int lx_haiku_team_unwatch(lx_thread_id team,	/* 0: removed before it ran */
	void (*hook)(lx_thread_id, void *), void *data);

/* A run of wired pages, contiguous in physical memory and starting at a
   multiple of "alignment" bytes (a power of two, or 0), for a device that is
   given physical addresses. Not mapped anywhere. Returns a handle for
   lx_haiku_page_run_free(), or NULL. */
void *lx_haiku_page_run_alloc(unsigned long count, unsigned long long alignment,
	unsigned long long *physical);
void lx_haiku_page_run_free(void *run, unsigned long count);

/* the effective user ID of the calling team */
unsigned int lx_haiku_current_euid(void);

/* files: the whole file in a new malloc() block */
int lx_haiku_read_file(const char *path, void **data, unsigned long *size);

/* PCI devices (Haiku's PCI bus manager) */
struct lx_haiku_pci_info {
	int bus;
	int device;
	int function;
	unsigned short vendor_id;
	unsigned short device_id;
	unsigned short subsystem_vendor_id;
	unsigned short subsystem_id;
	unsigned char class_base;
	unsigned char class_sub;
	unsigned char class_api;
	unsigned char revision;
	unsigned char header_type;
	unsigned char interrupt_line;
	unsigned long long bar_address[6];	/* 64-bit BARs: in the lower one */
	unsigned long long bar_size[6];
	unsigned int bar_flags[6];			/* 1 I/O, 2 64-bit, 4 prefetchable */
};
int lx_haiku_pci_get_nth(int index, struct lx_haiku_pci_info *info);
unsigned int lx_haiku_pci_read(int bus, int device, int function, int offset,
	int size);
void lx_haiku_pci_write(int bus, int device, int function, int offset,
	int size, unsigned int value);
/* one MSI vector; returns the interrupt vector or a negative error */
int lx_haiku_pci_enable_msi(int bus, int device, int function);
void lx_haiku_pci_disable_msi(int bus, int device, int function);
/* MSI-X: how many vectors the device has; "count" of them enabled, with
   the first one's interrupt vector returned (the others follow it), or a
   negative error. lx_haiku_pci_disable_msi() ends MSI-X too. */
int lx_haiku_pci_msix_count(int bus, int device, int function);
int lx_haiku_pci_enable_msix(int bus, int device, int function, int count);

/* ACPI firmware tables (signature, 1 for the first instance); the table
   stays mapped */
int lx_haiku_acpi_get_table(const char *signature, unsigned int instance,
	void **table);

/* interrupts: fn runs in interrupt context and must only wake a thread */
void *lx_haiku_install_interrupt(unsigned int vector, void (*fn)(void *),
	void *data);
void lx_haiku_remove_interrupt(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* _LX_HAIKU_H */
