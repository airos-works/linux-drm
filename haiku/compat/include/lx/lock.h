/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_LOCK_H
#define _LX_LOCK_H

/*	Locks.

	Nothing Linux runs here is in interrupt context: the device interrupt only
	wakes a thread, and timers fire in a thread too (lx/time.h). So every
	Linux lock - spinlock_t, raw_spinlock_t, rwlock_t, struct mutex - is a
	Haiku mutex, and holding a "spinlock" may block. Interrupts are never
	disabled for Linux code (local_irq_save() and preempt_disable() do
	nothing).

	The Haiku mutex is opaque storage here (haiku/compat/haiku/Lock.cpp
	checks its size) and is initialized on first use, so zeroed memory and
	static initializers are valid unlocked locks, as they are in Linux. */

struct task_struct;

struct lx_lock {
	int state;			/* 0 not initialized, 1 initializing, 2 ready */
	int depth;			/* lockdep subclass, unused */
	long owner;			/* Haiku thread holding it, 0 if none */
	const char *name;
	void *owner_site;	/* where the owner locked it (for the watchdog) */
	u64 haiku[6];		/* Haiku's mutex */
};

void lx_lock_lock(struct lx_lock *lock);
struct lx_lock *lx_lock_waited_for(long thread);
int lx_lock_trylock(struct lx_lock *lock);
void lx_lock_unlock(struct lx_lock *lock);
void lx_lock_destroy(struct lx_lock *lock);
bool lx_lock_held_by_current(const struct lx_lock *lock);

#define __LX_LOCK_INITIALIZER(lockname)	{ .name = #lockname }


/* #pragma mark - spinlocks */

typedef struct spinlock {
	struct lx_lock l;
} spinlock_t;
typedef struct raw_spinlock {
	struct lx_lock l;
} raw_spinlock_t;

#define __SPIN_LOCK_UNLOCKED(lockname)	{ .l = __LX_LOCK_INITIALIZER(lockname) }
#define __RAW_SPIN_LOCK_UNLOCKED(lockname) __SPIN_LOCK_UNLOCKED(lockname)
#define DEFINE_SPINLOCK(x)		spinlock_t x = __SPIN_LOCK_UNLOCKED(x)
#define DEFINE_RAW_SPINLOCK(x)	raw_spinlock_t x = __RAW_SPIN_LOCK_UNLOCKED(x)

#define spin_lock_init(lock) \
	do { *(lock) = (spinlock_t)__SPIN_LOCK_UNLOCKED(lock); } while (0)
#define raw_spin_lock_init(lock) \
	do { *(lock) = (raw_spinlock_t)__RAW_SPIN_LOCK_UNLOCKED(lock); } while (0)
#define spin_lock(lock)				lx_lock_lock(&(lock)->l)
#define spin_lock_nested(lock, sub)	lx_lock_lock(&(lock)->l)
#define spin_lock_bh(lock)			lx_lock_lock(&(lock)->l)
#define spin_lock_irq(lock)			lx_lock_lock(&(lock)->l)
#define spin_lock_irqsave(lock, flags) \
	do { (flags) = 0; lx_lock_lock(&(lock)->l); } while (0)
#define spin_lock_irqsave_nested(lock, flags, sub) spin_lock_irqsave(lock, flags)
#define spin_trylock(lock)			lx_lock_trylock(&(lock)->l)
#define spin_trylock_irq(lock)		lx_lock_trylock(&(lock)->l)
#define spin_trylock_irqsave(lock, flags) \
	({ (flags) = 0; lx_lock_trylock(&(lock)->l); })
#define spin_unlock(lock)			lx_lock_unlock(&(lock)->l)
#define spin_unlock_bh(lock)		lx_lock_unlock(&(lock)->l)
#define spin_unlock_irq(lock)		lx_lock_unlock(&(lock)->l)
#define spin_unlock_irqrestore(lock, flags) \
	do { (void)(flags); lx_lock_unlock(&(lock)->l); } while (0)
#define spin_is_locked(lock)		(READ_ONCE((lock)->l.owner) != 0)
#define assert_spin_locked(lock)	do { } while (0)
#define raw_spin_lock				spin_lock
#define raw_spin_lock_irq			spin_lock_irq
#define raw_spin_lock_irqsave		spin_lock_irqsave
#define raw_spin_unlock				spin_unlock
#define raw_spin_unlock_irq			spin_unlock_irq
#define raw_spin_unlock_irqrestore	spin_unlock_irqrestore
#define raw_spin_trylock			spin_trylock

/* rwlock_t: exclusive for readers too; only short sections use it */
typedef spinlock_t rwlock_t;
#define DEFINE_RWLOCK(x)	DEFINE_SPINLOCK(x)
#define __RW_LOCK_UNLOCKED(x)	__SPIN_LOCK_UNLOCKED(x)
#define rwlock_init(lock)	spin_lock_init(lock)
#define read_lock(lock)		spin_lock(lock)
#define read_unlock(lock)	spin_unlock(lock)
#define write_lock(lock)	spin_lock(lock)
#define write_unlock(lock)	spin_unlock(lock)
#define read_lock_irqsave(lock, flags)		spin_lock_irqsave(lock, flags)
#define read_unlock_irqrestore(lock, flags)	spin_unlock_irqrestore(lock, flags)
#define write_lock_irqsave(lock, flags)		spin_lock_irqsave(lock, flags)
#define write_unlock_irqrestore(lock, flags) spin_unlock_irqrestore(lock, flags)
#define read_lock_irq(lock)		spin_lock(lock)
#define read_unlock_irq(lock)	spin_unlock(lock)
#define write_lock_irq(lock)	spin_lock(lock)
#define write_unlock_irq(lock)	spin_unlock(lock)

/* interrupt and preemption control: nothing runs in interrupt context */
#define local_irq_save(flags)		do { (flags) = 0; } while (0)
#define local_irq_restore(flags)	do { (void)(flags); } while (0)
#define local_irq_disable()			do { } while (0)
#define local_irq_enable()			do { } while (0)
#define local_bh_disable()			do { } while (0)
#define local_bh_enable()			do { } while (0)
#define irqs_disabled()				0
#define preempt_disable()			barrier()
#define preempt_enable()			barrier()
#define preempt_disable_notrace()	barrier()
#define preempt_enable_notrace()	barrier()
#define preempt_enable_no_resched()	barrier()
#define migrate_disable()			barrier()
#define migrate_enable()			barrier()
#define preemptible()				1
#define in_interrupt()				0
#define in_irq()					0
#define in_hardirq()				0
#define in_softirq()				0
#define in_serving_softirq()		0
#define in_nmi()					0
#define in_task()					1
#define in_atomic()					0
#define irq_count()					0
#define hardirq_count()				0
#define softirq_count()				0
#define preempt_count()				0


/* #pragma mark - mutexes */

struct mutex {
	struct lx_lock l;
};

#define __MUTEX_INITIALIZER(lockname)	{ .l = __LX_LOCK_INITIALIZER(lockname) }
#define DEFINE_MUTEX(mutexname) \
	struct mutex mutexname = __MUTEX_INITIALIZER(mutexname)
#define mutex_init(m) \
	do { *(m) = (struct mutex)__MUTEX_INITIALIZER(m); } while (0)
#define __mutex_init(m, name, key)	mutex_init(m)
#define mutex_init_with_key(m, key)	mutex_init(m)
#define mutex_destroy(mutex)		lx_lock_destroy(&(mutex)->l)
#define mutex_lock(mutex)			lx_lock_lock(&(mutex)->l)
#define mutex_lock_nested(mutex, sub) lx_lock_lock(&(mutex)->l)
#define mutex_lock_nest_lock(mutex, nest) lx_lock_lock(&(mutex)->l)
#define mutex_lock_io(mutex)		lx_lock_lock(&(mutex)->l)
#define mutex_lock_interruptible(mutex)	({ lx_lock_lock(&(mutex)->l); 0; })
#define mutex_lock_interruptible_nested(mutex, sub) mutex_lock_interruptible(mutex)
#define mutex_lock_killable(mutex)	({ lx_lock_lock(&(mutex)->l); 0; })
#define mutex_lock_killable_nested(mutex, sub) mutex_lock_killable(mutex)
#define mutex_trylock(mutex)		lx_lock_trylock(&(mutex)->l)
#define mutex_unlock(mutex)			lx_lock_unlock(&(mutex)->l)
#define mutex_is_locked(m)			(READ_ONCE((m)->l.owner) != 0)
#define devm_mutex_init(dev, mutex)	({ mutex_init(mutex); 0; })
int atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock);

/* the scope-based guards of linux/cleanup.h */
DEFINE_GUARD(mutex, struct mutex *, mutex_lock(_T), mutex_unlock(_T))
DEFINE_GUARD_COND(mutex, _try, mutex_trylock(_T))
DEFINE_GUARD_COND(mutex, _intr, mutex_lock_interruptible(_T), _RET == 0)
DEFINE_LOCK_GUARD_1(spinlock, spinlock_t, spin_lock(_T->lock), spin_unlock(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irq, spinlock_t, spin_lock_irq(_T->lock),
	spin_unlock_irq(_T->lock))
DEFINE_LOCK_GUARD_1(spinlock_irqsave, spinlock_t,
	spin_lock_irqsave(_T->lock, _T->flags),
	spin_unlock_irqrestore(_T->lock, _T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_1(raw_spinlock, raw_spinlock_t, lx_lock_lock(&_T->lock->l),
	lx_lock_unlock(&_T->lock->l))
DEFINE_LOCK_GUARD_1(raw_spinlock_irqsave, raw_spinlock_t,
	lx_lock_lock(&_T->lock->l), lx_lock_unlock(&_T->lock->l), unsigned long flags)
DEFINE_LOCK_GUARD_0(irq, local_irq_disable(), local_irq_enable())
DEFINE_LOCK_GUARD_0(irqsave, local_irq_save(_T->flags),
	local_irq_restore(_T->flags), unsigned long flags)
DEFINE_LOCK_GUARD_0(preempt, preempt_disable(), preempt_enable())
DEFINE_LOCK_GUARD_0(migrate, migrate_disable(), migrate_enable())


/* #pragma mark - read/write semaphores */

struct rw_semaphore {
	int state;
	int pad;
	long writer;
	const char *name;
	u64 haiku[10];		/* Haiku's rw_lock */
};

#define __RWSEM_INITIALIZER(lockname)	{ .name = #lockname }
#define DECLARE_RWSEM(lockname) \
	struct rw_semaphore lockname = __RWSEM_INITIALIZER(lockname)
#define init_rwsem(sem) \
	do { *(sem) = (struct rw_semaphore)__RWSEM_INITIALIZER(sem); } while (0)
void down_read(struct rw_semaphore *sem);
int down_read_trylock(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
int down_write_trylock(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);
void downgrade_write(struct rw_semaphore *sem);
#define down_read_nested(sem, sub)		down_read(sem)
#define down_write_nested(sem, sub)		down_write(sem)
#define down_read_interruptible(sem)	({ down_read(sem); 0; })
#define down_read_killable(sem)			({ down_read(sem); 0; })
#define down_write_killable(sem)		({ down_write(sem); 0; })
bool rwsem_is_locked(struct rw_semaphore *sem);
#define rwsem_is_contended(sem)	0
#define rwsem_assert_held(sem)			do { } while (0)
#define rwsem_assert_held_write(sem)	do { } while (0)
DEFINE_GUARD(rwsem_read, struct rw_semaphore *, down_read(_T), up_read(_T))
DEFINE_GUARD(rwsem_write, struct rw_semaphore *, down_write(_T), up_write(_T))


/* #pragma mark - counting semaphores */

struct semaphore {
	struct lx_lock l;		/* protects count */
	unsigned int count;
	void *lx_waiters;		/* Haiku condition variable */
	u64 haiku[4];
};
#define __SEMAPHORE_INITIALIZER(name, n)	{ .l = __LX_LOCK_INITIALIZER(name), .count = (n) }
#define DEFINE_SEMAPHORE(name, n)	struct semaphore name = __SEMAPHORE_INITIALIZER(name, n)
static inline void sema_init(struct semaphore *sem, int val)
{
	*sem = (struct semaphore)__SEMAPHORE_INITIALIZER(*sem, val);
}
void down(struct semaphore *sem);
int down_interruptible(struct semaphore *sem);
#define down_killable(sem)	down_interruptible(sem)
int down_trylock(struct semaphore *sem);
int down_timeout(struct semaphore *sem, long jiffies);
void up(struct semaphore *sem);


/* #pragma mark - lockdep */

struct lock_class_key { char dummy; };
struct lockdep_map { const char *name; };
#define lockdep_assert_held(lock)			do { (void)(lock); } while (0)
#define lockdep_assert_held_write(lock)		do { (void)(lock); } while (0)
#define lockdep_assert_held_read(lock)		do { (void)(lock); } while (0)
#define lockdep_assert_held_once(lock)		do { (void)(lock); } while (0)
#define lockdep_assert_not_held(lock)		do { (void)(lock); } while (0)
#define lockdep_assert_none_held_once()		do { } while (0)
#define lockdep_assert_irqs_disabled()		do { } while (0)
#define lockdep_assert_irqs_enabled()		do { } while (0)
#define lockdep_assert_in_irq()				do { } while (0)
#define lockdep_assert(cond)				do { (void)(cond); } while (0)
#define lockdep_assert_once(cond)			do { (void)(cond); } while (0)
#define lockdep_is_held(lock)				1
#define lockdep_is_held_type(lock, r)		1
#define lockdep_set_class(lock, key)		do { (void)(key); } while (0)
#define lockdep_set_class_and_name(lock, key, name) do { (void)(key); } while (0)
#define lockdep_set_subclass(lock, sub)		do { } while (0)
#define lockdep_set_novalidate_class(lock)	do { } while (0)
#define lockdep_register_key(key)			do { } while (0)
#define lockdep_unregister_key(key)			do { } while (0)
#define lockdep_init_map(map, name, key, sub) do { } while (0)
#define lockdep_off()						do { } while (0)
#define lockdep_on()						do { } while (0)
#define lock_map_acquire(map)				do { } while (0)
#define lock_map_acquire_read(map)			do { } while (0)
#define lock_map_acquire_try(map)			do { } while (0)
#define lock_map_release(map)				do { } while (0)
#define lock_acquire(...)					do { } while (0)
#define lock_release(...)					do { } while (0)
#define lock_acquire_shared_recursive(...)	do { } while (0)
#define lock_acquire_exclusive(...)			do { } while (0)
#define rwsem_acquire(...)					do { } while (0)
#define rwsem_acquire_read(...)				do { } while (0)
#define rwsem_release(...)					do { } while (0)
#define mutex_acquire(...)					do { } while (0)
#define mutex_release(...)					do { } while (0)
#define lockdep_pin_lock(l)					((struct pin_cookie){ })
#define lockdep_unpin_lock(l, c)			do { } while (0)
#define STATIC_LOCKDEP_MAP_INIT(_name, _key)	{ .name = (_name) }
#define debug_locks				0
#define debug_locks_off()		0
#define debug_check_no_locks_held()	do { } while (0)
struct pin_cookie { };
#define SINGLE_DEPTH_NESTING	1
#define MAX_LOCKDEP_SUBCLASSES	8UL
#define lockdep_set_notrack_class(lock)	do { } while (0)
#define might_lock_read(lock)	do { } while (0)
#define fs_reclaim_acquire(gfp)	do { } while (0)
#define fs_reclaim_release(gfp)	do { } while (0)
#define lock_is_held_type(map, r)	1
#define lock_is_held(map)	1


/* #pragma mark - wound/wait mutexes */

struct ww_class {
	atomic_long_t stamp;
	const char *name;
	bool is_wait_die;
};

struct ww_acquire_ctx {
	struct task_struct *task;
	unsigned long stamp;
	unsigned int acquired;
	unsigned short wounded;
	unsigned short is_wait_die;
	struct ww_class *ww_class;
};

struct ww_mutex {
	struct mutex base;
	struct ww_acquire_ctx *ctx;
	struct ww_class *ww_class;
};

#define __WW_CLASS_INITIALIZER(ww_class, _is_wait_die) \
	{ .stamp = ATOMIC_LONG_INIT(0), .name = #ww_class, \
	  .is_wait_die = _is_wait_die }
#define DEFINE_WD_CLASS(classname) \
	struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 1)
#define DEFINE_WW_CLASS(classname) \
	struct ww_class classname = __WW_CLASS_INITIALIZER(classname, 0)

static inline void ww_mutex_init(struct ww_mutex *lock, struct ww_class *ww_class)
{
	mutex_init(&lock->base);
	lock->ctx = NULL;
	lock->ww_class = ww_class;
}
void ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class);
static inline void ww_acquire_done(struct ww_acquire_ctx *ctx) { }
static inline void ww_acquire_fini(struct ww_acquire_ctx *ctx) { }
int ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
int ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
#define ww_mutex_lock_slow(lock, ctx)	((void)ww_mutex_lock(lock, ctx))
#define ww_mutex_lock_slow_interruptible(lock, ctx) ww_mutex_lock(lock, ctx)
int ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx);
void ww_mutex_unlock(struct ww_mutex *lock);
#define ww_mutex_destroy(lock)		mutex_destroy(&(lock)->base)
#define ww_mutex_is_locked(lock)	mutex_is_locked(&(lock)->base)


/* #pragma mark - sequence counters */

typedef struct seqcount {
	unsigned sequence;
} seqcount_t;
typedef seqcount_t seqcount_spinlock_t;
typedef seqcount_t seqcount_raw_spinlock_t;
typedef seqcount_t seqcount_mutex_t;
typedef seqcount_t seqcount_ww_mutex_t;
typedef seqcount_t seqcount_rwlock_t;

#define SEQCNT_ZERO(name)	{ .sequence = 0 }
#define seqcount_init(s)	do { (s)->sequence = 0; } while (0)
#define seqcount_spinlock_init(s, lock)		seqcount_init(s)
#define seqcount_raw_spinlock_init(s, lock)	seqcount_init(s)
#define seqcount_mutex_init(s, lock)		seqcount_init(s)
#define seqcount_ww_mutex_init(s, lock)		seqcount_init(s)

static inline unsigned read_seqcount_begin(const seqcount_t *s)
{
	unsigned seq;
	while ((seq = READ_ONCE(s->sequence)) & 1)
		cpu_relax();
	smp_rmb();
	return seq;
}
#define raw_read_seqcount_begin(s)	read_seqcount_begin(s)
#define __read_seqcount_begin(s)	read_seqcount_begin(s)
static inline unsigned raw_read_seqcount(const seqcount_t *s)
{
	unsigned seq = READ_ONCE(s->sequence);
	smp_rmb();
	return seq;
}
static inline int read_seqcount_retry(const seqcount_t *s, unsigned start)
{
	smp_rmb();
	return unlikely(READ_ONCE(s->sequence) != start);
}
#define __read_seqcount_retry(s, start)	read_seqcount_retry(s, start)
static inline void write_seqcount_begin(seqcount_t *s)
{
	s->sequence++;
	smp_wmb();
}
static inline void write_seqcount_end(seqcount_t *s)
{
	smp_wmb();
	s->sequence++;
}
#define raw_write_seqcount_begin(s)	write_seqcount_begin(s)
#define raw_write_seqcount_end(s)	write_seqcount_end(s)
#define write_seqcount_invalidate(s) do { smp_wmb(); (s)->sequence += 2; } while (0)
#define raw_seqcount_begin(s)		(READ_ONCE((s)->sequence) & ~1)

typedef struct {
	seqcount_t seqcount;
	spinlock_t lock;
} seqlock_t;
#define __SEQLOCK_UNLOCKED(name) \
	{ .seqcount = SEQCNT_ZERO(name), .lock = __SPIN_LOCK_UNLOCKED(name) }
#define DEFINE_SEQLOCK(sl)	seqlock_t sl = __SEQLOCK_UNLOCKED(sl)
#define seqlock_init(sl) \
	do { seqcount_init(&(sl)->seqcount); spin_lock_init(&(sl)->lock); } while (0)
#define read_seqbegin(sl)			read_seqcount_begin(&(sl)->seqcount)
#define read_seqretry(sl, start)	read_seqcount_retry(&(sl)->seqcount, start)
static inline void write_seqlock(seqlock_t *sl)
{
	spin_lock(&sl->lock);
	write_seqcount_begin(&sl->seqcount);
}
static inline void write_sequnlock(seqlock_t *sl)
{
	write_seqcount_end(&sl->seqcount);
	spin_unlock(&sl->lock);
}
#define write_seqlock_irqsave(sl, flags) \
	do { (flags) = 0; write_seqlock(sl); } while (0)
#define write_sequnlock_irqrestore(sl, flags) \
	do { (void)(flags); write_sequnlock(sl); } while (0)
#define write_seqlock_irq(sl)		write_seqlock(sl)
#define write_sequnlock_irq(sl)		write_sequnlock(sl)


/* #pragma mark - RCU */

/*	Readers increment one global count; a grace period has passed once the
	count has been seen at zero after it began (haiku/compat/haiku/Rcu.cpp).
	call_rcu() callbacks run in batches on a thread. */

void lx_rcu_read_lock(void);
void lx_rcu_read_unlock(void);
#define rcu_read_lock()			lx_rcu_read_lock()
#define rcu_read_unlock()		lx_rcu_read_unlock()
#define rcu_read_lock_bh()		lx_rcu_read_lock()
#define rcu_read_unlock_bh()	lx_rcu_read_unlock()
#define rcu_read_lock_sched()	lx_rcu_read_lock()
#define rcu_read_unlock_sched()	lx_rcu_read_unlock()
#define rcu_read_lock_held()	1
#define rcu_read_lock_any_held()	1
#define rcu_read_lock_bh_held()	1
#define rcu_read_lock_sched_held()	1
void synchronize_rcu(void);
#define synchronize_rcu_expedited()	synchronize_rcu()
#define synchronize_srcu(s)			synchronize_rcu()
#define synchronize_rcu_tasks()		synchronize_rcu()
#define synchronize_net()			synchronize_rcu()
typedef void (*rcu_callback_t)(struct rcu_head *head);
void call_rcu(struct rcu_head *head, rcu_callback_t func);
void rcu_barrier(void);
unsigned long lx_rcu_backlog(void);
void lx_kfree_rcu(void *object, struct rcu_head *head);
#define kfree_rcu(ptr, rhf)		lx_kfree_rcu((ptr), &(ptr)->rhf)
#define kvfree_rcu(ptr, rhf)	lx_kfree_rcu((ptr), &(ptr)->rhf)
#define kfree_rcu_mightsleep(ptr) do { synchronize_rcu(); kfree(ptr); } while (0)
#define kvfree_rcu_mightsleep(ptr)	kfree_rcu_mightsleep(ptr)
#define cond_synchronize_rcu(x)	synchronize_rcu()
#define get_state_synchronize_rcu()	0UL
#define rcu_head_init(head)		do { } while (0)

#define RCU_INITIALIZER(v)		(typeof(*(v)) __force __rcu *)(v)
#define rcu_dereference(p)		READ_ONCE(p)
#define rcu_dereference_raw(p)	READ_ONCE(p)
#define rcu_dereference_check(p, c)		READ_ONCE(p)
#define rcu_dereference_protected(p, c)	(p)
#define rcu_dereference_bh(p)	READ_ONCE(p)
#define rcu_dereference_sched(p)	READ_ONCE(p)
#define rcu_dereference_raw_check(p)	READ_ONCE(p)
#define srcu_dereference(p, s)	READ_ONCE(p)
#define srcu_dereference_check(p, s, c)	READ_ONCE(p)
#define rcu_access_pointer(p)	READ_ONCE(p)
#define rcu_assign_pointer(p, v)	smp_store_release(&(p), (v))
#define RCU_INIT_POINTER(p, v)	do { WRITE_ONCE(p, v); } while (0)
#define rcu_pointer_handoff(p)	(p)
#define rcu_replace_pointer(rcu_ptr, ptr, c) ({ \
	typeof(ptr) __tmp = rcu_dereference_protected((rcu_ptr), (c)); \
	rcu_assign_pointer((rcu_ptr), (ptr)); \
	__tmp; \
})
#define unrcu_pointer(p)		(p)
#define RCU_LOCKDEP_WARN(c, s)	do { } while (0)
#define rcu_sleep_check()		do { } while (0)
#define rcu_lockdep_assert(c, s) do { } while (0)
DEFINE_LOCK_GUARD_0(rcu, rcu_read_lock(), rcu_read_unlock())

/* SRCU: the same as RCU */
struct srcu_struct { int readers; };
#define DEFINE_SRCU(name)			struct srcu_struct name
#define DEFINE_STATIC_SRCU(name)	static struct srcu_struct name
#define init_srcu_struct(s)			0
#define cleanup_srcu_struct(s)		do { } while (0)
#define srcu_read_lock(s)			({ lx_rcu_read_lock(); 0; })
#define srcu_read_unlock(s, idx)	lx_rcu_read_unlock()

/* #pragma mark - reference counts with locks */

bool refcount_dec_and_mutex_lock(refcount_t *r, struct mutex *lock);
bool refcount_dec_and_lock(refcount_t *r, spinlock_t *lock);
bool refcount_dec_and_lock_irqsave(refcount_t *r, spinlock_t *lock,
	unsigned long *flags);

#include <linux/kref.h>

#endif /* _LX_LOCK_H */
