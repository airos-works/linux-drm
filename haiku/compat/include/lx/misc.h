/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_MISC_H
#define _LX_MISC_H

/*	Everything else: reference counts, RCU lists, trees, ID allocators,
	interrupts, I2C, tracepoints, notifiers, random numbers, and the
	subsystems the drivers can do without (ACPI, DMI, backlight, hwmon,
	audio components, CEC, cgroups, crash dumps). */

#include <linux/llist.h>

/* #pragma mark - RCU lists (lx/lock.h makes RCU readers plain) */

#define list_add_rcu(new, head)			list_add(new, head)
#define list_add_tail_rcu(new, head)	list_add_tail(new, head)
#define list_del_rcu(entry)				__list_del_entry(entry)
#define list_replace_rcu(old, new)		list_replace(old, new)
#define list_entry_rcu(ptr, type, member)	container_of(READ_ONCE(ptr), type, member)
#define list_first_or_null_rcu(ptr, type, member)	list_first_entry_or_null(ptr, type, member)
#define list_next_or_null_rcu(head, ptr, type, member) ({ \
	struct list_head *__head = (head); \
	struct list_head *__ptr = (ptr); \
	struct list_head *__next = READ_ONCE(__ptr->next); \
	likely(__next != __head) ? list_entry_rcu(__next, type, member) : NULL; \
})
#define list_for_each_entry_rcu(pos, head, member, cond...) \
	list_for_each_entry(pos, head, member)
#define list_for_each_entry_srcu(pos, head, member, cond) \
	list_for_each_entry(pos, head, member)
#define list_for_each_entry_continue_rcu(pos, head, member) \
	list_for_each_entry_continue(pos, head, member)
#define list_for_each_entry_from_rcu(pos, head, member) \
	list_for_each_entry_from(pos, head, member)
#define list_for_each_entry_lockless(pos, head, member) \
	list_for_each_entry(pos, head, member)
#define list_tail_rcu(head)		(*((struct list_head __rcu **)(&(head)->prev)))
#define list_splice_init_rcu(list, head, sync) \
	do { sync(); list_splice_init(list, head); } while (0)
#define hlist_add_head_rcu(n, h)		hlist_add_head(n, h)
#define hlist_add_behind_rcu(n, prev)	hlist_add_behind(n, prev)
#define hlist_add_before_rcu(n, next)	hlist_add_before(n, next)
#define hlist_del_rcu(n)				__hlist_del(n)
#define hlist_del_init_rcu(n)			hlist_del_init(n)
#define hlist_replace_rcu(old, new)		hlist_replace_rcu_lx(old, new)
#define hlist_for_each_entry_rcu(pos, head, member, cond...) \
	hlist_for_each_entry(pos, head, member)
#define hlist_for_each_entry_rcu_notrace(pos, head, member) \
	hlist_for_each_entry(pos, head, member)
#define hlist_for_each_entry_continue_rcu(pos, member) \
	hlist_for_each_entry_continue(pos, member)
#define hlist_first_rcu(head)	(*((struct hlist_node __rcu **)(&(head)->first)))
#define hlist_next_rcu(node)	(*((struct hlist_node __rcu **)(&(node)->next)))

#include <linux/hashtable.h>
#include <linux/rbtree.h>
#include <linux/rbtree_augmented.h>
#include <linux/interval_tree.h>
#include <linux/interval_tree_generic.h>
#include <linux/uuid.h>
#include <linux/nospec.h>
#include <linux/iosys-map.h>
#include <linux/kfifo.h>
#include <linux/average.h>

/* #pragma mark - XArray and IDR (compat/src/xarray.c) */

/*	An XArray here is a sorted array of index/entry pairs: the drivers keep
	a few hundred objects per array at most. Same API, same semantics for
	the parts in use (allocation with limits and cyclic allocation, marks,
	iteration in index order). */

#define XA_FLAGS_LOCK_IRQ		((__force gfp_t)1U)
#define XA_FLAGS_LOCK_BH		((__force gfp_t)2U)
#define XA_FLAGS_TRACK_FREE		((__force gfp_t)4U)
#define XA_FLAGS_ZERO_BUSY		((__force gfp_t)8U)
#define XA_FLAGS_ALLOC_WRAPPED	((__force gfp_t)16U)
#define XA_FLAGS_ACCOUNT		((__force gfp_t)32U)
#define XA_FLAGS_MARK(mark)		((__force gfp_t)((1U << 7) << (__force unsigned)(mark)))
#define XA_FLAGS_ALLOC	(XA_FLAGS_TRACK_FREE | XA_FLAGS_MARK(XA_FREE_MARK))
#define XA_FLAGS_ALLOC1	(XA_FLAGS_TRACK_FREE | XA_FLAGS_ZERO_BUSY)

typedef unsigned __bitwise xa_mark_t;
#define XA_MARK_0		((__force xa_mark_t)0U)
#define XA_MARK_1		((__force xa_mark_t)1U)
#define XA_MARK_2		((__force xa_mark_t)2U)
#define XA_PRESENT		((__force xa_mark_t)8U)
#define XA_MARK_MAX		XA_MARK_2
#define XA_FREE_MARK	XA_MARK_0

struct lx_xa_slot {
	unsigned long index;
	void *entry;
	unsigned int marks;
};

/*	xa_lock is the lock callers take around the __xa_*() functions, as in
	Linux; the slots are guarded by an internal lock of their own, so the
	reads Linux does without locks (xa_load(), xa_find() ...) also work
	while the caller holds xa_lock. */
struct xarray {
	spinlock_t xa_lock;
	spinlock_t lx_slots_lock;
	gfp_t xa_flags;
	struct lx_xa_slot *slots;
	unsigned int count;
	unsigned int capacity;
	unsigned long next_cyclic;
};

struct xa_limit {
	u32 max;
	u32 min;
};
#define XA_LIMIT(_min, _max)	(struct xa_limit) { .min = _min, .max = _max }
#define xa_limit_32b	XA_LIMIT(0, UINT_MAX)
#define xa_limit_31b	XA_LIMIT(0, INT_MAX)
#define xa_limit_16b	XA_LIMIT(0, USHRT_MAX)

#define XA_ZERO_ENTRY	((void *)(uintptr_t)0x406)
#define XA_ERROR(errno)	((void *)(((unsigned long)(errno) << 2) | 2UL))
static inline bool xa_is_err(const void *entry)
{
	return unlikely(((unsigned long)entry & 3) == 2 &&
		(long)entry >= -((4095L << 2) | 2) - 1 && (long)entry < 0);
}
static inline int xa_err(void *entry)
{
	if (xa_is_err(entry))
		return (long)entry >> 2;
	return 0;
}
#define xa_is_zero(entry)	((entry) == XA_ZERO_ENTRY)
#define xa_is_value(entry)	((unsigned long)(entry) & 1)
#define xa_mk_value(v)		((void *)(((v) << 1) | 1))
#define xa_to_value(entry)	((unsigned long)(entry) >> 1)

#define XARRAY_INIT(name, flags) { \
	.xa_lock = __SPIN_LOCK_UNLOCKED(name.xa_lock), .xa_flags = flags }
#define DEFINE_XARRAY_FLAGS(name, flags) struct xarray name = XARRAY_INIT(name, flags)
#define DEFINE_XARRAY(name)			DEFINE_XARRAY_FLAGS(name, 0)
#define DEFINE_XARRAY_ALLOC(name)	DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC)
#define DEFINE_XARRAY_ALLOC1(name)	DEFINE_XARRAY_FLAGS(name, XA_FLAGS_ALLOC1)

void xa_init_flags(struct xarray *xa, gfp_t flags);
#define xa_init(xa)	xa_init_flags(xa, 0)
void xa_destroy(struct xarray *xa);
void *xa_load(struct xarray *xa, unsigned long index);
void *__xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
void *xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
#define xa_store_irq	xa_store
#define xa_store_bh		xa_store
void *__xa_erase(struct xarray *xa, unsigned long index);
void *xa_erase(struct xarray *xa, unsigned long index);
#define xa_erase_irq	xa_erase
#define xa_erase_bh		xa_erase
void *__xa_cmpxchg(struct xarray *xa, unsigned long index, void *old,
	void *entry, gfp_t gfp);
void *xa_cmpxchg(struct xarray *xa, unsigned long index, void *old, void *entry,
	gfp_t gfp);
int __xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
int xa_insert(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
#define xa_insert_irq	xa_insert
#define xa_insert_bh	xa_insert
int __xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
	gfp_t gfp);
int xa_alloc(struct xarray *xa, u32 *id, void *entry, struct xa_limit limit,
	gfp_t gfp);
#define xa_alloc_irq	xa_alloc
#define xa_alloc_bh		xa_alloc
int __xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
	struct xa_limit limit, u32 *next, gfp_t gfp);
int xa_alloc_cyclic(struct xarray *xa, u32 *id, void *entry,
	struct xa_limit limit, u32 *next, gfp_t gfp);
#define xa_alloc_cyclic_irq	xa_alloc_cyclic
int xa_reserve(struct xarray *xa, unsigned long index, gfp_t gfp);
#define xa_reserve_irq	xa_reserve
#define xa_release(xa, index)	xa_cmpxchg(xa, index, XA_ZERO_ENTRY, NULL, 0)
void *xa_find(struct xarray *xa, unsigned long *index, unsigned long max,
	xa_mark_t mark);
void *xa_find_after(struct xarray *xa, unsigned long *index, unsigned long max,
	xa_mark_t mark);
bool xa_get_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
void xa_set_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
void xa_clear_mark(struct xarray *xa, unsigned long index, xa_mark_t mark);
#define __xa_set_mark	xa_set_mark
#define __xa_clear_mark	xa_clear_mark
static inline bool xa_empty(const struct xarray *xa)
{
	return READ_ONCE(xa->count) == 0;
}
static inline bool xa_marked(const struct xarray *xa, xa_mark_t mark)
{
	unsigned int i;
	for (i = 0; i < xa->count; i++)
		if (xa->slots[i].marks & (1U << (__force unsigned)mark))
			return true;
	return false;
}
#define xa_lock(xa)				spin_lock(&(xa)->xa_lock)
#define xa_unlock(xa)			spin_unlock(&(xa)->xa_lock)
#define xa_lock_irq(xa)			spin_lock(&(xa)->xa_lock)
#define xa_unlock_irq(xa)		spin_unlock(&(xa)->xa_lock)
#define xa_lock_bh(xa)			spin_lock(&(xa)->xa_lock)
#define xa_unlock_bh(xa)		spin_unlock(&(xa)->xa_lock)
#define xa_lock_irqsave(xa, f)	spin_lock_irqsave(&(xa)->xa_lock, f)
#define xa_unlock_irqrestore(xa, f)	spin_unlock_irqrestore(&(xa)->xa_lock, f)
#define xa_for_each_range(xa, index, entry, start, last) \
	for (index = start, entry = xa_find(xa, &index, last, XA_PRESENT); \
	     entry; \
	     entry = xa_find_after(xa, &index, last, XA_PRESENT))
#define xa_for_each_start(xa, index, entry, start) \
	xa_for_each_range(xa, index, entry, start, ULONG_MAX)
#define xa_for_each(xa, index, entry)	xa_for_each_start(xa, index, entry, 0)
#define xa_for_each_marked(xa, index, entry, filter) \
	for (index = 0, entry = xa_find(xa, &index, ULONG_MAX, filter); \
	     entry; entry = xa_find_after(xa, &index, ULONG_MAX, filter))

/* IDR and IDA on the XArray */
struct idr {
	struct xarray idr_rt;
	unsigned int idr_base;
	unsigned int idr_next;
};
#define IDR_INIT_BASE(name, base) { \
	.idr_rt = XARRAY_INIT(name.idr_rt, XA_FLAGS_ALLOC), .idr_base = (base), \
	.idr_next = 0 }
#define IDR_INIT(name)	IDR_INIT_BASE(name, 0)
#define DEFINE_IDR(name)	struct idr name = IDR_INIT(name)
static inline void idr_init_base(struct idr *idr, int base)
{
	xa_init_flags(&idr->idr_rt, XA_FLAGS_ALLOC);
	idr->idr_base = base;
	idr->idr_next = 0;
}
#define idr_init(idr)	idr_init_base(idr, 0)
int idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
int idr_alloc_cyclic(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
int idr_alloc_u32(struct idr *idr, void *ptr, u32 *id, unsigned long max,
	gfp_t gfp);
void *idr_find(const struct idr *idr, unsigned long id);
void *idr_remove(struct idr *idr, unsigned long id);
void *idr_replace(struct idr *idr, void *ptr, unsigned long id);
void *idr_get_next(struct idr *idr, int *nextid);
void *idr_get_next_ul(struct idr *idr, unsigned long *nextid);
int idr_for_each(const struct idr *idr,
	int (*fn)(int id, void *p, void *data), void *data);
void idr_destroy(struct idr *idr);
#define idr_is_empty(idr)	xa_empty(&(idr)->idr_rt)
#define idr_preload(gfp)	do { } while (0)
#define idr_preload_end()	do { } while (0)
#define idr_lock(idr)		xa_lock(&(idr)->idr_rt)
#define idr_unlock(idr)		xa_unlock(&(idr)->idr_rt)
#define idr_for_each_entry(idr, entry, id) \
	for (id = 0; ((entry) = idr_get_next(idr, &(id))) != NULL; id += 1U)
#define idr_for_each_entry_ul(idr, entry, tmp, id) \
	for (tmp = 0, id = 0; \
	     ((entry) = tmp <= id ? idr_get_next_ul(idr, &(id)) : NULL) != NULL; \
	     tmp = id, ++id)
#define idr_for_each_entry_continue(idr, entry, id) \
	for ((entry) = idr_get_next((idr), &(id)); entry; \
	     ++id, (entry) = idr_get_next((idr), &(id)))

struct ida {
	struct xarray xa;
};
#define IDA_INIT(name)	{ .xa = XARRAY_INIT(name.xa, XA_FLAGS_ALLOC) }
#define DEFINE_IDA(name)	struct ida name = IDA_INIT(name)
#define ida_init(ida)	xa_init_flags(&(ida)->xa, XA_FLAGS_ALLOC)
int ida_alloc_range(struct ida *ida, unsigned int min, unsigned int max,
	gfp_t gfp);
#define ida_alloc(ida, gfp)	ida_alloc_range(ida, 0, ~0, gfp)
#define ida_alloc_min(ida, min, gfp)	ida_alloc_range(ida, min, ~0, gfp)
#define ida_alloc_max(ida, max, gfp)	ida_alloc_range(ida, 0, max, gfp)
void ida_free(struct ida *ida, unsigned int id);
void ida_destroy(struct ida *ida);
#define ida_is_empty(ida)	xa_empty(&(ida)->xa)
#define ida_simple_get(ida, start, end, gfp) \
	ida_alloc_range(ida, start, (end) - 1, gfp)
#define ida_simple_remove(ida, id)	ida_free(ida, id)

/* #pragma mark - interrupts */

typedef enum irqreturn {
	IRQ_NONE = (0 << 0),
	IRQ_HANDLED = (1 << 0),
	IRQ_WAKE_THREAD = (1 << 1),
} irqreturn_t;
#define IRQ_RETVAL(x)	((x) ? IRQ_HANDLED : IRQ_NONE)
typedef irqreturn_t (*irq_handler_t)(int, void *);
#define IRQF_SHARED		0x00000080
#define IRQF_ONESHOT	0x00002000
#define IRQF_NO_SUSPEND	0x00004000
/*	The handler runs on a thread the Haiku interrupt handler wakes
	(haiku/compat/haiku/Irq.cpp). */
int request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
	const char *name, void *dev);
int request_threaded_irq(unsigned int irq, irq_handler_t handler,
	irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev);
const void *free_irq(unsigned int irq, void *dev_id);
void lx_irq_stats(void *dev_id, unsigned long *arrived, unsigned long *handled,
	u64 *running_ns, long *thread);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);
void enable_irq(unsigned int irq);
void synchronize_irq(unsigned int irq);
#define devm_request_irq(dev, irq, h, f, n, d)	request_irq(irq, h, f, n, d)
#define irq_set_irq_wake(irq, on)	0
#define enable_irq_wake(irq)	0
#define disable_irq_wake(irq)	0
#define local_irq_enable_in_hardirq()	do { } while (0)
#define tasklet_init(t, f, d)	do { } while (0)
#define tasklet_schedule(t)		do { } while (0)
#define tasklet_kill(t)			do { } while (0)
struct tasklet_struct { int dummy; };
struct irq_domain;
struct irq_chip;
struct irq_data;
#define irq_find_mapping(domain, hwirq)	0
#define irq_create_mapping(domain, hwirq)	0
#define irq_dispose_mapping(virq)	do { } while (0)
#define irq_domain_remove(domain)	kfree(domain)
#define generic_handle_irq(irq)	0
#define generic_handle_domain_irq(domain, hwirq)	0
#define mmu_notifier_synchronize()	do { } while (0)
#define topology_num_cores_per_package()	lx_num_online_cpus()

/* #pragma mark - notifiers */

struct notifier_block;
typedef int (*notifier_fn_t)(struct notifier_block *nb, unsigned long action,
	void *data);
struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block __rcu *next;
	int priority;
};
struct blocking_notifier_head {
	struct rw_semaphore rwsem;
	struct notifier_block __rcu *head;
};
struct atomic_notifier_head {
	spinlock_t lock;
	struct notifier_block __rcu *head;
};
#define NOTIFY_DONE		0x0000
#define NOTIFY_OK		0x0001
#define NOTIFY_STOP_MASK	0x8000
#define NOTIFY_BAD		(NOTIFY_STOP_MASK | 0x0002)
#define NOTIFY_STOP		(NOTIFY_OK | NOTIFY_STOP_MASK)
#define BLOCKING_NOTIFIER_HEAD(name) \
	struct blocking_notifier_head name = { .rwsem = __RWSEM_INITIALIZER(name.rwsem) }
#define BLOCKING_INIT_NOTIFIER_HEAD(name) \
	do { init_rwsem(&(name)->rwsem); (name)->head = NULL; } while (0)
#define ATOMIC_INIT_NOTIFIER_HEAD(name) \
	do { spin_lock_init(&(name)->lock); (name)->head = NULL; } while (0)
int blocking_notifier_chain_register(struct blocking_notifier_head *nh,
	struct notifier_block *nb);
int blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
	struct notifier_block *nb);
int blocking_notifier_call_chain(struct blocking_notifier_head *nh,
	unsigned long val, void *v);
#define register_reboot_notifier(nb)	0
#define unregister_reboot_notifier(nb)	0
#define register_pm_notifier(nb)	0
#define unregister_pm_notifier(nb)	0
#define register_acpi_notifier(nb)	0
#define unregister_acpi_notifier(nb)	0
#define register_oom_notifier(nb)	0
#define unregister_oom_notifier(nb)	0
#define PM_HIBERNATION_PREPARE	0x0001
#define PM_POST_HIBERNATION		0x0002
#define PM_SUSPEND_PREPARE		0x0003
#define PM_POST_SUSPEND			0x0004
#define PM_RESTORE_PREPARE		0x0005
#define PM_POST_RESTORE			0x0006
#define SYS_RESTART	0x0001
#define SYS_HALT	0x0002
#define SYS_POWER_OFF	0x0003

/* #pragma mark - random numbers */

u32 get_random_u32(void);
u64 get_random_u64(void);
void get_random_bytes(void *buf, size_t len);
#define get_random_long()	((unsigned long)get_random_u64())
#define get_random_int()	get_random_u32()
static inline u32 get_random_u32_below(u32 ceil)
{
	return (u32)(((u64)get_random_u32() * ceil) >> 32);
}
#define get_random_u32_inclusive(floor, ceil) \
	((floor) + get_random_u32_below((ceil) - (floor) + 1))
#define prandom_u32_max(ceil)	get_random_u32_below(ceil)

/* #pragma mark - I2C */

struct i2c_adapter;
#include <uapi/linux/i2c.h>
#define I2C_CLASS_HWMON		(1 << 0)
#define I2C_CLASS_DDC		(1 << 3)
#define I2C_CLASS_SPD		(1 << 7)
#define I2C_NAME_SIZE		20
struct i2c_algorithm {
	int (*xfer)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	int (*xfer_atomic)(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
	int (*smbus_xfer)(struct i2c_adapter *adap, u16 addr, unsigned short flags,
		char read_write, u8 command, int size, union i2c_smbus_data *data);
	u32 (*functionality)(struct i2c_adapter *adap);
};
#define master_xfer		xfer
#define master_xfer_atomic	xfer_atomic
struct i2c_lock_operations {
	void (*lock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	int (*trylock_bus)(struct i2c_adapter *adapter, unsigned int flags);
	void (*unlock_bus)(struct i2c_adapter *adapter, unsigned int flags);
};
struct i2c_adapter_quirks {
	u64 flags;
	int max_num_msgs;
	u16 max_write_len;
	u16 max_read_len;
	u16 max_comb_1st_msg_len;
	u16 max_comb_2nd_msg_len;
};
#define I2C_AQ_COMB				BIT(0)
#define I2C_AQ_COMB_WRITE_FIRST	BIT(1)
#define I2C_AQ_COMB_READ_SECOND	BIT(2)
#define I2C_AQ_COMB_SAME_ADDR	BIT(3)
#define I2C_AQ_COMB_WRITE_THEN_READ	(I2C_AQ_COMB | I2C_AQ_COMB_WRITE_FIRST | \
	I2C_AQ_COMB_READ_SECOND | I2C_AQ_COMB_SAME_ADDR)
#define I2C_AQ_NO_CLK_STRETCH	BIT(4)
#define I2C_AQ_NO_ZERO_LEN_READ	BIT(5)
#define I2C_AQ_NO_ZERO_LEN_WRITE	BIT(6)
#define I2C_AQ_NO_ZERO_LEN		(I2C_AQ_NO_ZERO_LEN_READ | I2C_AQ_NO_ZERO_LEN_WRITE)
#define I2C_AQ_NO_REP_START		BIT(7)
struct i2c_adapter {
	struct module *owner;
	unsigned int class;
	const struct i2c_algorithm *algo;
	void *algo_data;
	const struct i2c_lock_operations *lock_ops;
	struct rt_mutex_lx { struct mutex m; } bus_lock;
	int timeout;
	int retries;
	struct device dev;
	int nr;
	char name[48];
	const struct i2c_adapter_quirks *quirks;
};
struct i2c_client {
	unsigned short flags;
	unsigned short addr;
	char name[I2C_NAME_SIZE];
	struct i2c_adapter *adapter;
	struct device dev;
};
struct i2c_board_info {
	char type[I2C_NAME_SIZE];
	unsigned short flags;
	unsigned short addr;
	void *platform_data;
};
#define I2C_BOARD_INFO(dev_type, dev_addr)	.type = dev_type, .addr = (dev_addr)
#define to_i2c_adapter(d)	container_of(d, struct i2c_adapter, dev)
static inline void *i2c_get_adapdata(const struct i2c_adapter *adap)
{
	return dev_get_drvdata(&adap->dev);
}
static inline void i2c_set_adapdata(struct i2c_adapter *adap, void *data)
{
	dev_set_drvdata(&adap->dev, data);
}
int i2c_add_adapter(struct i2c_adapter *adap);
void i2c_del_adapter(struct i2c_adapter *adap);
int i2c_add_numbered_adapter(struct i2c_adapter *adap);
static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg)
{
	return (msg->addr << 1) | (msg->flags & I2C_M_RD);
}
int i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
int __i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num);
#define I2C_LOCK_ROOT_ADAPTER	BIT(0)
#define I2C_LOCK_SEGMENT		BIT(1)
#define i2c_lock_bus(adap, flags)	mutex_lock(&(adap)->bus_lock.m)
#define i2c_unlock_bus(adap, flags)	mutex_unlock(&(adap)->bus_lock.m)
#define i2c_trylock_bus(adap, flags)	mutex_trylock(&(adap)->bus_lock.m)
#define i2c_new_client_device(adap, info)	ERR_PTR(-ENODEV)
#define i2c_unregister_device(client)	do { } while (0)
#define i2c_check_functionality(adap, func)	true
#define devm_i2c_add_adapter(dev, adap)	i2c_add_adapter(adap)
s32 i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command);
s32 i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command,
	u8 value);
struct i2c_algo_bit_data {
	void *data;
	void (*setsda)(void *data, int state);
	void (*setscl)(void *data, int state);
	int (*getsda)(void *data);
	int (*getscl)(void *data);
	int (*pre_xfer)(struct i2c_adapter *);
	void (*post_xfer)(struct i2c_adapter *);
	int udelay;
	int timeout;
	bool can_do_atomic;
};
int i2c_bit_add_bus(struct i2c_adapter *adap);
int i2c_bit_add_numbered_bus(struct i2c_adapter *adap);
extern const struct i2c_algorithm i2c_bit_algo;

/* #pragma mark - tracepoints: compiled out */

#define TP_PROTO(args...)	args
#define TP_ARGS(args...)	args
#define TP_CONDITION(args...)	args
#define TP_STRUCT__entry(args...)	args
#define TP_fast_assign(args...)	args
#define TP_printk(args...)	args
#define PARAMS(args...)		args
#define __DECLARE_TRACE_LX(name, proto, args) \
	static inline void trace_##name(proto) { } \
	static inline bool trace_##name##_enabled(void) { return false; } \
	static inline int register_trace_##name(void (*probe)(void *, proto), \
		void *data) { return 0; } \
	static inline int unregister_trace_##name(void (*probe)(void *, proto), \
		void *data) { return 0; }
#define DECLARE_TRACE(name, proto, args) \
	__DECLARE_TRACE_LX(name, PARAMS(proto), PARAMS(args))
#define DECLARE_TRACE_CONDITION(name, proto, args, cond) \
	__DECLARE_TRACE_LX(name, PARAMS(proto), PARAMS(args))
#define DECLARE_TRACE_EVENT(name, proto, args) \
	__DECLARE_TRACE_LX(name, PARAMS(proto), PARAMS(args))
#define DEFINE_TRACE(name, proto, args)
#define TRACE_EVENT(name, proto, args, struct, assign, print) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_EVENT_FN(name, proto, args, struct, assign, print, reg, unreg) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_EVENT_CONDITION(name, proto, args, cond, struct, assign, print) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(template, name, proto, args) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_FN(template, name, proto, args, reg, unreg) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_PRINT(template, name, proto, args, print) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define DEFINE_EVENT_CONDITION(template, name, proto, args, cond) \
	DECLARE_TRACE(name, PARAMS(proto), PARAMS(args))
#define TRACE_DEFINE_ENUM(x)
#define TRACE_DEFINE_SIZEOF(x)
#define EXPORT_TRACEPOINT_SYMBOL(x)
#define EXPORT_TRACEPOINT_SYMBOL_GPL(x)
#define tracepoint_enabled(tp)	false
#define trace_printk(fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
#define tracing_off()			do { } while (0)
#define ftrace_dump(mode)		do { } while (0)

/* #pragma mark - subsystems the drivers do without */

/* ACPI */
typedef void *acpi_handle;
typedef u32 acpi_status;
typedef u64 acpi_size;
#define AE_OK		0
#define ACPI_FAILURE(s)	((s) != AE_OK)
#define ACPI_SUCCESS(s)	((s) == AE_OK)
#define ACPI_HANDLE(dev)	NULL
#define ACPI_COMPANION(dev)	NULL
#define has_acpi_companion(dev)	false
#define acpi_disabled		1
#define acpi_video_register_backlight()	do { } while (0)
#define acpi_video_backlight_use_native()	true
#define acpi_video_get_backlight_type()	0
#define acpi_backlight_vendor	0
#define acpi_backlight_native	1
#define acpi_video_report_nolcd()	do { } while (0)
#define acpi_video_get_edid(adev, type, device, edid)	(-ENODEV)
#define ACPI_VIDEO_DISPLAY_LCD	4
struct acpi_table_header {
	char signature[4];
	u32 length;
	u8 revision;
	u8 checksum;
	char oem_id[6];
	char oem_table_id[8];
	u32 oem_revision;
	char asl_compiler_id[4];
	u32 asl_compiler_revision;
};
/* Firmware tables are real: APUs and most laptops keep their VBIOS in VFCT. */
int lx_haiku_acpi_get_table(const char *signature, unsigned int instance,
	void **table);
static inline acpi_status
acpi_get_table(const char *signature, u32 instance,
	struct acpi_table_header **table)
{
	return lx_haiku_acpi_get_table(signature, instance, (void **)table) == 0
		? AE_OK : 1;
}
#define acpi_put_table(table)	do { } while (0)
#define acpi_target_system_state()	0
#define ACPI_STATE_S0	0
#define ACPI_STATE_S3	3

struct mmu_interval_notifier;
struct mmu_notifier;
struct fb_var_screeninfo;
struct fb_cmap;

/* DMI (struct dmi_system_id is in linux/mod_devicetable.h) */
#define dmi_check_system(list)	0
#define dmi_first_match(list)	NULL
#define dmi_match(field, str)	false
#define dmi_get_system_info(field)	NULL

/* EFI, firmware frame buffers */
#define efi_enabled(f)		false
#define EFI_BOOT			0
#define aperture_remove_conflicting_pci_devices(pdev, name)	0
#define aperture_remove_conflicting_devices(base, size, name)	0
#define video_firmware_drivers_only()	false
#define screen_info_pci_dev(si)	NULL
#define sysfb_disable(dev)	do { } while (0)

/* backlight */
struct backlight_device;
struct backlight_properties {
	int brightness;
	int max_brightness;
	int power;
	int type;
	unsigned int state;
	int scale;
};
struct backlight_ops {
	unsigned int options;
	int (*update_status)(struct backlight_device *);
	int (*get_brightness)(struct backlight_device *);
};
struct backlight_device {
	struct backlight_properties props;
	const struct backlight_ops *ops;
	struct device dev;
};
#define BACKLIGHT_RAW		1
#define BACKLIGHT_PLATFORM	2
#define BACKLIGHT_FIRMWARE	3
#define BACKLIGHT_POWER_ON	0
#define BACKLIGHT_POWER_OFF	4
#define BACKLIGHT_SCALE_UNKNOWN	0
#define BACKLIGHT_SCALE_LINEAR	1
#define BACKLIGHT_SCALE_NON_LINEAR	2
#define BL_CORE_SUSPENDED	(1 << 0)
#define BL_CORE_FBBLANK		(1 << 1)
#define BL_CORE_SUSPENDRESUME	(1 << 0)
/* registered for real: DRM_HAIKU_IOCTL_BACKLIGHT sets the brightness the
   way writing it in sysfs does (compat/src/misc.c) */
struct backlight_device *backlight_device_register(const char *name,
	struct device *parent, void *devdata, const struct backlight_ops *ops,
	const struct backlight_properties *props);
void backlight_device_unregister(struct backlight_device *bd);
int backlight_device_set_brightness(struct backlight_device *bd,
	unsigned long brightness);
#define backlight_get_brightness(bd)	((bd)->props.brightness)
#define bl_get_data(bd)		dev_get_drvdata(&(bd)->dev)
static inline int
backlight_update_status(struct backlight_device *bd)
{
	return bd->ops != NULL && bd->ops->update_status != NULL
		? bd->ops->update_status(bd) : -ENXIO;
}
#define backlight_is_blank(bd)	false

/* hwmon, power supply, thermal */
#define hwmon_device_register_with_groups(dev, name, data, groups)	ERR_PTR(-ENODEV)
#define hwmon_device_unregister(dev)	do { } while (0)
#define power_supply_is_system_supplied()	1
struct sensor_device_attribute {
	struct device_attribute dev_attr;
	int index;
};
#define SENSOR_DEVICE_ATTR(_name, _mode, _show, _store, _index) \
	struct sensor_device_attribute sensor_dev_attr_##_name = \
		{ .dev_attr = __ATTR(_name, _mode, _show, _store), .index = _index }
#define to_sensor_dev_attr(_dev_attr) \
	container_of(_dev_attr, struct sensor_device_attribute, dev_attr)

/* components (HDMI audio) */
struct component_ops {
	int (*bind)(struct device *comp, struct device *master, void *master_data);
	void (*unbind)(struct device *comp, struct device *master, void *master_data);
};
#define component_add(dev, ops)		0
#define component_del(dev, ops)		do { } while (0)

/* CEC */
struct cec_adapter;
struct cec_notifier;
struct cec_connector_info { int dummy; };
#define cec_notifier_conn_register(dev, name, info)	NULL
#define cec_notifier_conn_unregister(n)	do { } while (0)
#define cec_notifier_set_phys_addr(n, pa)	do { } while (0)
#define cec_notifier_set_phys_addr_from_edid(n, edid)	do { } while (0)
#define cec_notifier_phys_addr_invalidate(n)	do { } while (0)
#define cec_fill_conn_info_from_drm(info, conn)	do { } while (0)
#define CEC_PHYS_ADDR_INVALID	0xffff

/* cgroups */
struct dmem_cgroup_region;
struct dmem_cgroup_pool_state;
#define dmem_cgroup_register_region(size, fmt, ...)	NULL
#define dmem_cgroup_unregister_region(region)	do { } while (0)
#define dmem_cgroup_try_charge(region, size, ret_pool, ret_limit_pool)	0
#define dmem_cgroup_uncharge(pool, size)	do { } while (0)
#define dmem_cgroup_state_evict_valuable(limit, test, ignore, hit)	true
#define dmem_cgroup_pool_state_put(pool)	do { } while (0)

/* crash dumps, fault injection, stack traces */
#define dev_coredumpm(dev, owner, data, datalen, gfp, read, free) \
	do { free(data); } while (0)
#define dev_coredumpv(dev, data, datalen, gfp)	kvfree(data)
struct fault_attr { int dummy; };
#define DECLARE_FAULT_ATTR(name)	struct fault_attr name
#define fault_create_debugfs_attr(name, parent, attr)	ERR_PTR(-ENODEV)
typedef u32 depot_stack_handle_t;
#define stack_depot_save(entries, nr, gfp)	0
#define stack_depot_snprint(h, buf, size, spaces)	0
#define stack_depot_init()	do { } while (0)
#define stack_trace_save(store, size, skipnr)	0
#define stack_trace_print(entries, nr, spaces)	do { } while (0)
#define stack_trace_snprint(buf, size, entries, nr, spaces)	0
#define dump_stack_lvl(level)	dump_stack()

/* kernel message dumps (drm_panic) */
enum kmsg_dump_reason {
	KMSG_DUMP_UNDEF,
	KMSG_DUMP_PANIC,
	KMSG_DUMP_OOPS,
	KMSG_DUMP_EMERG,
	KMSG_DUMP_SHUTDOWN,
	KMSG_DUMP_MAX
};
struct kmsg_dump_detail {
	enum kmsg_dump_reason reason;
	const char *description;
};
struct kmsg_dumper {
	struct list_head list;
	void (*dump)(struct kmsg_dumper *dumper, struct kmsg_dump_detail *detail);
	enum kmsg_dump_reason max_reason;
	bool registered;
};
#define kmsg_dump_register(dumper)		(-ENOSYS)
#define kmsg_dump_unregister(dumper)	0

/* device tree: x86 PCs have none */
struct device_node;
struct of_endpoint {
	unsigned int port;
	unsigned int id;
	const struct device_node *local_node;
};
struct of_device_id;
struct fwnode_handle { struct fwnode_handle *secondary; const void *ops; };
#define of_node_put(np)			do { } while (0)
#define of_node_get(np)			(np)
#define of_graph_parse_endpoint(node, endpoint)	(-ENODEV)
#define of_graph_get_remote_port_parent(node)	NULL
#define of_graph_get_port_by_id(node, id)	NULL
#define of_graph_get_next_endpoint(parent, prev)	NULL
#define of_graph_get_remote_port(node)	NULL
#define of_graph_get_endpoint_by_regs(parent, port, ep)	NULL
#define of_graph_get_remote_endpoint(node)	NULL
#define of_graph_get_remote_node(node, port, ep)	NULL
#define for_each_endpoint_of_node(parent, child) 	for (child = NULL; child; )
#define of_device_is_available(np)	false
#define of_device_is_compatible(np, c)	0
#define of_property_read_u32(np, name, val)	(-EINVAL)
#define of_property_read_bool(np, name)	false
#define of_property_present(np, name)	false
#define of_get_child_by_name(np, name)	NULL
#define of_find_node_by_name(from, name)	NULL
#define of_machine_is_compatible(c)	0
#define of_match_ptr(p)				NULL
#define fwnode_handle_put(f)		do { } while (0)
#define fwnode_handle_get(f)		(f)
#define of_fwnode_handle(np)		NULL
#define fwnode_property_present(f, name)	false
#define dev_fwnode(dev)				((dev)->fwnode)

/* kernel FPU use: Haiku saves the FPU state on every kernel entry */
#define kernel_fpu_begin()		do { } while (0)
#define kernel_fpu_end()		do { } while (0)
#define kernel_fpu_begin_mask(m)	do { } while (0)
#define kernel_fpu_available()	true
#define irq_fpu_usable()		true
#define fpregs_lock()			do { } while (0)
#define fpregs_unlock()			do { } while (0)

/* eventfd */
struct eventfd_ctx;
#define eventfd_ctx_fdget(fd)	((struct eventfd_ctx *)ERR_PTR(-ENOSYS))
#define eventfd_ctx_fileget(f)	((struct eventfd_ctx *)ERR_PTR(-ENOSYS))
#define eventfd_ctx_put(ctx)	do { } while (0)
#define eventfd_signal(ctx)		do { } while (0)

/* kernel command line */
#define video_get_options(name)	((const char *)NULL)
#define get_options(str, nints, ints)	((char *)(str))
#define get_cmdline(task, buffer, buflen)	0
#define saved_command_line	""

/* platform devices */
struct platform_device {
	const char *name;
	int id;
	struct device dev;
};
#define platform_device_unregister(pdev)	do { } while (0)
#define platform_device_register_simple(name, id, res, num)	ERR_PTR(-ENODEV)
#define platform_device_register_data(parent, name, id, data, size)	ERR_PTR(-ENODEV)
#define platform_get_drvdata(pdev)	dev_get_drvdata(&(pdev)->dev)
#define to_platform_device(d)	container_of(d, struct platform_device, dev)

#define KHZ2PICOS(a)	(1000000000UL / (a))
#define PICOS2KHZ(a)	(1000000000UL / (a))

/* per-CPU variables: an array indexed by CPU (only DC's FPU depth uses them) */
#define DEFINE_PER_CPU(type, name)	__typeof__(type) name[CONFIG_NR_CPUS]
#define DECLARE_PER_CPU(type, name)	extern __typeof__(type) name[CONFIG_NR_CPUS]
#define per_cpu(var, cpu)			((var)[cpu])
#define this_cpu_ptr(ptr)			(&(*(ptr))[smp_processor_id()])
#define __this_cpu_read(var)		((var)[smp_processor_id()])
#define this_cpu_read(var)			((var)[smp_processor_id()])
#define __this_cpu_write(var, v)	((var)[smp_processor_id()] = (v))
#define __this_cpu_inc_return(var)	(++(var)[smp_processor_id()])
#define __this_cpu_dec_return(var)	(--(var)[smp_processor_id()])
#define this_cpu_inc(var)			((var)[smp_processor_id()]++)
#define this_cpu_dec(var)			((var)[smp_processor_id()]--)

#define HW_ERR		"[Hardware Error]: "
#ifndef TRUE
#	define TRUE		1
#	define FALSE	0
#endif
#define num_possible_nodes()	1
#define num_online_nodes()		1

/* IRQ domains (amdgpu's own interrupt sources for ACP and ISP) */
typedef unsigned long irq_hw_number_t_lx;
struct irq_data {
	unsigned int irq;
	unsigned long hwirq;
	void *chip_data;
};
struct irq_chip {
	const char *name;
	void (*irq_mask)(struct irq_data *data);
	void (*irq_unmask)(struct irq_data *data);
};
struct irq_domain_ops {
	int (*map)(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw);
	void (*unmap)(struct irq_domain *d, unsigned int virq);
};
/*	amdgpu's own domain feeds its ACP and ISP children, which Haiku does not
	have: the domain exists, its interrupts never fire. */
struct irq_domain *lx_irq_domain_create(unsigned int size,
	const struct irq_domain_ops *ops, void *host_data);
#define irq_domain_create_linear(fwnode, size, ops, host_data) \
	lx_irq_domain_create(size, ops, host_data)
#define irq_domain_add_linear(of_node, size, ops, host_data) \
	lx_irq_domain_create(size, ops, host_data)
#define irq_set_chip_and_handler(irq, chip, handle)	do { } while (0)
#define handle_simple_irq	NULL
#define irq_to_desc(irq)	NULL

/* device memory pages (SVM) */
enum memory_type {
	MEMORY_DEVICE_PRIVATE = 1,
	MEMORY_DEVICE_COHERENT,
	MEMORY_DEVICE_FS_DAX,
	MEMORY_DEVICE_GENERIC,
	MEMORY_DEVICE_PCI_P2PDMA,
};
struct range {
	u64 start;
	u64 end;
};
struct dev_pagemap_ops;
struct dev_pagemap {
	struct range range;
	enum memory_type type;
	const struct dev_pagemap_ops *ops;
	void *owner;
	int nr_range;
};
#define devm_memremap_pages(dev, pgmap)	ERR_PTR(-ENODEV)
#define devm_memunmap_pages(dev, pgmap)	do { } while (0)
/*	amdgpu's userptr pages (haiku/amdgpu/userptr.c): the range describes
	pages that stay wired for the buffer's life, and its "pfns" are their
	struct pages. */
struct hmm_range {
	struct mmu_interval_notifier *notifier;
	unsigned long notifier_seq;
	unsigned long start;
	unsigned long end;
	unsigned long *hmm_pfns;
	unsigned long default_flags;
	unsigned long pfn_flags_mask;
	void *dev_private_owner;
};
#define hmm_pfn_to_page(pfn)	((struct page *)(pfn))

/* radix trees, on the XArray */
#define radix_tree_root		xarray
struct radix_tree_iter {
	unsigned long index;
	unsigned long next_index;
	unsigned long tags;
};
#define INIT_RADIX_TREE(root, mask)	xa_init_flags(root, 0)
#define RADIX_TREE(name, mask)		struct radix_tree_root name = XARRAY_INIT(name, 0)
#define radix_tree_insert(root, index, entry)	xa_insert(root, index, entry, GFP_KERNEL)
#define radix_tree_lookup(root, index)	xa_load(root, index)
#define radix_tree_delete(root, index)	xa_erase(root, index)
#define radix_tree_tag_set(root, index, tag) \
	({ xa_set_mark(root, index, (__force xa_mark_t)(tag)); xa_load(root, index); })
#define radix_tree_tag_clear(root, index, tag) \
	({ xa_clear_mark(root, index, (__force xa_mark_t)(tag)); xa_load(root, index); })
#define radix_tree_tag_get(root, index, tag) \
	xa_get_mark(root, index, (__force xa_mark_t)(tag))
#define radix_tree_tagged(root, tag)	xa_marked(root, (__force xa_mark_t)(tag))
#define radix_tree_empty(root)		xa_empty(root)
#define radix_tree_deref_slot(slot)	(*(slot))
void **lx_radix_tree_next(struct xarray *root, struct radix_tree_iter *iter,
	int tag);
#define radix_tree_for_each_slot(slot, root, iter, start) \
	for ((iter)->next_index = (start); \
	     ((slot) = lx_radix_tree_next(root, iter, -1)) != NULL; )
#define radix_tree_for_each_tagged(slot, root, iter, start, tag) \
	for ((iter)->next_index = (start); \
	     ((slot) = lx_radix_tree_next(root, iter, tag)) != NULL; )
#define radix_tree_iter_delete(root, iter, slot) \
	((void)xa_erase(root, (iter)->index))
unsigned int radix_tree_gang_lookup(struct xarray *root, void **results,
	unsigned long first_index, unsigned int max_items);
unsigned int radix_tree_gang_lookup_tag(struct xarray *root, void **results,
	unsigned long first_index, unsigned int max_items, unsigned int tag);

/* console */
#define console_lock()		do { } while (0)
#define console_unlock()	do { } while (0)
#define console_trylock()	1
#define console_suspend_enabled	0

/* misc */
#define get_seconds()	ktime_get_real_seconds()
#define kernel_restart(cmd)	do { } while (0)
#define emergency_restart()	do { } while (0)
#define orderly_poweroff(f)	do { } while (0)
#define arch_prctl(x)	0
#define add_device_randomness(buf, len)	do { } while (0)
#define printk_get_level(s)	0

#endif /* _LX_MISC_H */
