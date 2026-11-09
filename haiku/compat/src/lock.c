/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Locks: Linux's spinlocks and mutexes on Haiku mutexes, read/write
	semaphores on rw_locks, counting semaphores, wound/wait mutexes, RCU. */

#include <lx/haiku.h>


/* #pragma mark - spinlocks and mutexes */


/*	Waits for a lock that another thread is initializing right now: a few
	instructions, unless that thread lost the CPU in the middle of them.
	Then it has to get it back: a thread of a higher priority that only
	spun here - an interrupt thread that was woken on the same CPU - kept it
	from running for ever (Haiku moves a ready thread to no other CPU), and
	with it everything else in that CPU's queue. So after a few looks the
	wait sleeps. */
static void
lx_lock_wait_ready(int *state)
{
	int spins = 0;
	while (__atomic_load_n(state, __ATOMIC_ACQUIRE) != 2) {
		if (++spins < 64)
			cpu_relax();
		else
			lx_haiku_snooze(20);
	}
}


static void
lx_lock_init_slow(struct lx_lock *lock)
{
	int state = 0;
	if (__atomic_compare_exchange_n(&lock->state, &state, 1, false,
			__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
		lx_haiku_mutex_init(lock->haiku, lock->name);
		__atomic_store_n(&lock->state, 2, __ATOMIC_RELEASE);
		return;
	}
	/* another thread is initializing it */
	lx_lock_wait_ready(&lock->state);
}


static inline void
lx_lock_ensure(struct lx_lock *lock)
{
	if (unlikely(__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) != 2))
		lx_lock_init_slow(lock);
}


extern int hung_wait_s;

/*	For the watchdog: which lock a thread is blocked on, by thread id
	(hashed; a collision only loses that information). Written only when a
	lock is contended. */
#define LX_LOCK_WAITERS 1024
static struct {
	long thread;
	struct lx_lock *lock;
} lx_lock_waiters[LX_LOCK_WAITERS];


/*	The lock \a thread is blocked on, or NULL. The lock stays valid while
	the thread waits for it (its holder has to release it first). */
struct lx_lock *
lx_lock_waited_for(long thread)
{
	unsigned long slot = (unsigned long)thread % LX_LOCK_WAITERS;
	struct lx_lock *lock = READ_ONCE(lx_lock_waiters[slot].lock);
	if (lock == NULL || READ_ONCE(lx_lock_waiters[slot].thread) != thread)
		return NULL;
	return lock;
}


void
lx_lock_lock(struct lx_lock *lock)
{
	unsigned long slot;
	long self;

	lx_lock_ensure(lock);
	if (lx_haiku_mutex_trylock(lock->haiku)) {
		WRITE_ONCE(lock->owner, lx_haiku_current_thread());
		WRITE_ONCE(lock->owner_site, __builtin_return_address(0));
		return;
	}
	self = lx_haiku_current_thread();
	slot = (unsigned long)self % LX_LOCK_WAITERS;
	WRITE_ONCE(lx_lock_waiters[slot].lock, NULL);
	WRITE_ONCE(lx_lock_waiters[slot].thread, self);
	WRITE_ONCE(lx_lock_waiters[slot].lock, lock);
	if (hung_wait_s > 0) {
		if (lx_haiku_mutex_lock_timeout(lock->haiku,
				(long long)hung_wait_s * 1000000) != 0) {
			char what[96];
			snprintf(what, sizeof(what), "waiting for %d s for lock %s, owner "
				"thread %d", hung_wait_s, lock->name ? lock->name : "?",
				(int)READ_ONCE(lock->owner));
			lx_haiku_log_stack(what);
			lx_haiku_mutex_lock(lock->haiku);
			pr_info("lx: lock %s: got it\n", lock->name ? lock->name : "?");
		}
	} else
		lx_haiku_mutex_lock(lock->haiku);
	if (READ_ONCE(lx_lock_waiters[slot].thread) == self)
		WRITE_ONCE(lx_lock_waiters[slot].lock, NULL);
	WRITE_ONCE(lock->owner, self);
	WRITE_ONCE(lock->owner_site, __builtin_return_address(0));
}


int
lx_lock_trylock(struct lx_lock *lock)
{
	lx_lock_ensure(lock);
	if (!lx_haiku_mutex_trylock(lock->haiku))
		return 0;
	WRITE_ONCE(lock->owner, lx_haiku_current_thread());
	WRITE_ONCE(lock->owner_site, __builtin_return_address(0));
	return 1;
}


void
lx_lock_unlock(struct lx_lock *lock)
{
	WRITE_ONCE(lock->owner, 0);
	lx_haiku_mutex_unlock(lock->haiku);
}


void
lx_lock_destroy(struct lx_lock *lock)
{
	if (__atomic_load_n(&lock->state, __ATOMIC_ACQUIRE) == 2) {
		lx_haiku_mutex_destroy(lock->haiku);
		lock->state = 0;
	}
}


bool
lx_lock_held_by_current(const struct lx_lock *lock)
{
	return READ_ONCE(lock->owner) == lx_haiku_current_thread();
}


int
atomic_dec_and_mutex_lock(atomic_t *cnt, struct mutex *lock)
{
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	mutex_lock(lock);
	if (!atomic_dec_and_test(cnt)) {
		mutex_unlock(lock);
		return 0;
	}
	return 1;
}


/* #pragma mark - read/write semaphores */


static void
rwsem_ensure(struct rw_semaphore *sem)
{
	int state = __atomic_load_n(&sem->state, __ATOMIC_ACQUIRE);
	if (likely(state == 2))
		return;
	state = 0;
	if (__atomic_compare_exchange_n(&sem->state, &state, 1, false,
			__ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
		lx_haiku_rwlock_init(sem->haiku, sem->name);
		__atomic_store_n(&sem->state, 2, __ATOMIC_RELEASE);
		return;
	}
	lx_lock_wait_ready(&sem->state);
}


void
down_read(struct rw_semaphore *sem)
{
	rwsem_ensure(sem);
	lx_haiku_rwlock_read_lock(sem->haiku);
}


int
down_read_trylock(struct rw_semaphore *sem)
{
	rwsem_ensure(sem);
	return lx_haiku_rwlock_read_trylock(sem->haiku);
}


void
up_read(struct rw_semaphore *sem)
{
	lx_haiku_rwlock_read_unlock(sem->haiku);
}


void
down_write(struct rw_semaphore *sem)
{
	rwsem_ensure(sem);
	lx_haiku_rwlock_write_lock(sem->haiku);
	WRITE_ONCE(sem->writer, lx_haiku_current_thread());
}


int
down_write_trylock(struct rw_semaphore *sem)
{
	rwsem_ensure(sem);
	if (!lx_haiku_rwlock_write_trylock(sem->haiku))
		return 0;
	WRITE_ONCE(sem->writer, lx_haiku_current_thread());
	return 1;
}


void
up_write(struct rw_semaphore *sem)
{
	WRITE_ONCE(sem->writer, 0);
	lx_haiku_rwlock_write_unlock(sem->haiku);
}


void
downgrade_write(struct rw_semaphore *sem)
{
	/* not atomic on Haiku: another writer may get in between */
	up_write(sem);
	down_read(sem);
}


bool
rwsem_is_locked(struct rw_semaphore *sem)
{
	if (READ_ONCE(sem->writer) != 0)
		return true;
	if (down_write_trylock(sem)) {
		up_write(sem);
		return false;
	}
	return true;
}


/* #pragma mark - counting semaphores */


static void
sema_ensure(struct semaphore *sem)
{
	if (READ_ONCE(sem->lx_waiters) != NULL)
		return;
	void *event = lx_haiku_event_create();
	void *expected = NULL;
	if (!__atomic_compare_exchange_n(&sem->lx_waiters, &expected, event,
			false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		lx_haiku_event_delete(event);
}


static int
__down_timeout_us(struct semaphore *sem, long long timeout_us)
{
	long long deadline = timeout_us < 0 ? -1
		: lx_haiku_system_time() + timeout_us;
	sema_ensure(sem);
	for (;;) {
		lx_lock_lock(&sem->l);
		if (sem->count > 0) {
			sem->count--;
			lx_lock_unlock(&sem->l);
			return 0;
		}
		lx_lock_unlock(&sem->l);
		long long wait = -1;
		if (deadline >= 0) {
			wait = deadline - lx_haiku_system_time();
			if (wait <= 0)
				return -ETIME;
		}
		lx_haiku_event_wait(sem->lx_waiters, wait);
	}
}


void
down(struct semaphore *sem)
{
	__down_timeout_us(sem, -1);
}


int
down_interruptible(struct semaphore *sem)
{
	return __down_timeout_us(sem, -1);
}


int
down_trylock(struct semaphore *sem)
{
	int result = 1;
	lx_lock_lock(&sem->l);
	if (sem->count > 0) {
		sem->count--;
		result = 0;
	}
	lx_lock_unlock(&sem->l);
	return result;
}


int
down_timeout(struct semaphore *sem, long timeout)
{
	return __down_timeout_us(sem, (long long)jiffies_to_usecs(timeout));
}


void
up(struct semaphore *sem)
{
	sema_ensure(sem);
	lx_lock_lock(&sem->l);
	sem->count++;
	lx_lock_unlock(&sem->l);
	/* the event is binary: waiters loop and re-check the count */
	lx_haiku_event_signal(sem->lx_waiters);
}


/* #pragma mark - wound/wait mutexes */

/*	Wait-die for both classes: a context that already holds locks and finds
	a lock held by an older context backs off with -EDEADLK; otherwise it
	waits. Waiting re-checks the holder after every unlock of the lock, so a
	context never waits for an older one while holding locks. */

static atomic_long_t ww_stamp = ATOMIC_LONG_INIT(0);

void
ww_acquire_init(struct ww_acquire_ctx *ctx, struct ww_class *ww_class)
{
	ctx->task = current;
	ctx->stamp = atomic_long_inc_return(&ww_stamp);
	ctx->acquired = 0;
	ctx->wounded = 0;
	ctx->is_wait_die = ww_class->is_wait_die;
	ctx->ww_class = ww_class;
}


static int
__ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct wait_queue_head *wq = lx_var_waitqueue(lock);

	for (;;) {
		if (mutex_trylock(&lock->base)) {
			WRITE_ONCE(lock->ctx, ctx);
			if (ctx != NULL)
				ctx->acquired++;
			return 0;
		}
		if (ctx != NULL) {
			struct ww_acquire_ctx *holder = READ_ONCE(lock->ctx);
			if (holder == ctx)
				return -EALREADY;
			if (holder != NULL && ctx->acquired > 0
				&& (long)(holder->stamp - ctx->stamp) < 0)
				return -EDEADLK;
		}
		/* wait for an unlock, then look again */
		wait_event_timeout(*wq, !mutex_is_locked(&lock->base)
			|| (ctx != NULL && ctx->acquired > 0
				&& READ_ONCE(lock->ctx) != NULL
				&& (long)(READ_ONCE(lock->ctx)->stamp - ctx->stamp) < 0),
			msecs_to_jiffies(50));
	}
}


int
ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(lock, ctx);
}


int
ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	return __ww_mutex_lock(lock, ctx);
}


int
ww_mutex_trylock(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
	if (!mutex_trylock(&lock->base))
		return 0;
	WRITE_ONCE(lock->ctx, ctx);
	if (ctx != NULL)
		ctx->acquired++;
	return 1;
}


void
ww_mutex_unlock(struct ww_mutex *lock)
{
	struct ww_acquire_ctx *ctx = lock->ctx;
	if (ctx != NULL && ctx->acquired > 0)
		ctx->acquired--;
	WRITE_ONCE(lock->ctx, NULL);
	mutex_unlock(&lock->base);
	wake_up_all(lx_var_waitqueue(lock));
}


/* #pragma mark - RCU */

/*	One global reader count. synchronize_rcu() returns once the count has
	been zero after it was called: every reader that started before has
	finished then. call_rcu() queues callbacks for a thread that runs them in
	batches after a grace period. */

static long rcu_readers;

void
lx_rcu_read_lock(void)
{
	__atomic_add_fetch(&rcu_readers, 1, __ATOMIC_SEQ_CST);
}


void
lx_rcu_read_unlock(void)
{
	__atomic_sub_fetch(&rcu_readers, 1, __ATOMIC_SEQ_CST);
}


void
synchronize_rcu(void)
{
	int spins = 0;
	while (__atomic_load_n(&rcu_readers, __ATOMIC_SEQ_CST) != 0) {
		if (++spins < 100)
			cpu_relax();
		else
			lx_haiku_snooze(20);
	}
}


static DEFINE_SPINLOCK(rcu_lock);
static struct rcu_head *rcu_queue;
static struct rcu_head **rcu_queue_tail = &rcu_queue;
static unsigned long rcu_queued, rcu_done;
static DECLARE_WAIT_QUEUE_HEAD(rcu_done_wait);
static struct task_struct *rcu_thread;
static DECLARE_WAIT_QUEUE_HEAD(rcu_thread_wait);

static int
rcu_thread_main(void *unused)
{
	while (!kthread_should_stop()) {
		struct rcu_head *batch;
		unsigned long count = 0;

		wait_event_timeout(rcu_thread_wait, READ_ONCE(rcu_queue) != NULL
			|| kthread_should_stop(), msecs_to_jiffies(1000));

		spin_lock(&rcu_lock);
		batch = rcu_queue;
		rcu_queue = NULL;
		rcu_queue_tail = &rcu_queue;
		spin_unlock(&rcu_lock);
		if (batch == NULL)
			continue;

		synchronize_rcu();
		while (batch != NULL) {
			struct rcu_head *next = batch->next;
			batch->func(batch);
			batch = next;
			count++;
		}

		spin_lock(&rcu_lock);
		rcu_done += count;
		spin_unlock(&rcu_lock);
		wake_up_all(&rcu_done_wait);
	}
	return 0;
}


static void
rcu_start_thread(void)
{
	static DEFINE_MUTEX(start_lock);
	mutex_lock(&start_lock);
	if (rcu_thread == NULL) {
		struct task_struct *thread = kthread_run(rcu_thread_main, NULL,
			"linux rcu");
		if (!IS_ERR(thread))
			rcu_thread = thread;
	}
	mutex_unlock(&start_lock);
}


void
call_rcu(struct rcu_head *head, rcu_callback_t func)
{
	if (unlikely(READ_ONCE(rcu_thread) == NULL))
		rcu_start_thread();

	head->func = func;
	head->next = NULL;
	spin_lock(&rcu_lock);
	*rcu_queue_tail = head;
	rcu_queue_tail = &head->next;
	rcu_queued++;
	spin_unlock(&rcu_lock);
	wake_up(&rcu_thread_wait);
}


/* for a watchdog: call_rcu() callbacks queued but not run yet */
unsigned long
lx_rcu_backlog(void)
{
	unsigned long backlog;
	spin_lock(&rcu_lock);
	backlog = rcu_queued - rcu_done;
	spin_unlock(&rcu_lock);
	return backlog;
}


void
rcu_barrier(void)
{
	unsigned long target;
	spin_lock(&rcu_lock);
	target = rcu_queued;
	spin_unlock(&rcu_lock);
	if (READ_ONCE(rcu_thread) == NULL)
		return;
	wake_up(&rcu_thread_wait);
	wait_event(rcu_done_wait, ({
		unsigned long done;
		spin_lock(&rcu_lock);
		done = rcu_done;
		spin_unlock(&rcu_lock);
		(long)(done - target) >= 0;
	}));
}


struct lx_kfree_rcu_entry {
	struct rcu_head head;
	void *object;
};

void lx_rcu_exit(void);

void
lx_rcu_exit(void)
{
	struct task_struct *thread = READ_ONCE(rcu_thread);
	if (thread == NULL)
		return;
	rcu_barrier();
	kthread_stop(thread);
	rcu_thread = NULL;
}


static void
lx_kfree_rcu_callback(struct rcu_head *head)
{
	struct lx_kfree_rcu_entry *entry
		= container_of(head, struct lx_kfree_rcu_entry, head);
	kfree(entry->object);
	kfree(entry);
}


void
lx_kfree_rcu(void *object, struct rcu_head *head)
{
	/*	The object's own rcu_head may not be the first member, and Linux
		frees the object, not the head; keep both in a small record. If that
		allocation fails, wait for a grace period here instead. */
	struct lx_kfree_rcu_entry *entry;
	if (object == NULL)
		return;
	entry = kmalloc(sizeof(*entry), GFP_NOWAIT);
	if (entry == NULL) {
		synchronize_rcu();
		kfree(object);
		return;
	}
	entry->object = object;
	call_rcu(&entry->head, lx_kfree_rcu_callback);
}
