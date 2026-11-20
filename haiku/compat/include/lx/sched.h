/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_SCHED_H
#define _LX_SCHED_H

/*	Tasks, wait queues, completions, kernel threads and workqueues.

	A task_struct exists for every Haiku thread that runs Linux code; it is
	made on first use of "current" and freed when the thread dies
	(haiku/compat/haiku/Task.cpp). Sleeping follows Linux exactly: a task
	sets its state, then schedule() blocks unless a wake_up_process() has
	set it back to TASK_RUNNING. Wait queues, completions and the wait_event
	family are Linux's algorithms on top of that (compat/src/wait.c). */

#define TASK_RUNNING			0x00000000
#define TASK_INTERRUPTIBLE		0x00000001
#define TASK_UNINTERRUPTIBLE	0x00000002
#define __TASK_STOPPED			0x00000004
#define __TASK_TRACED			0x00000008
#define TASK_PARKED				0x00000040
#define TASK_DEAD				0x00000080
#define TASK_WAKEKILL			0x00000100
#define TASK_WAKING				0x00000200
#define TASK_NOLOAD				0x00000400
#define TASK_NEW				0x00000800
#define TASK_FREEZABLE			0x00002000
#define TASK_KILLABLE			(TASK_WAKEKILL | TASK_UNINTERRUPTIBLE)
#define TASK_IDLE				(TASK_UNINTERRUPTIBLE | TASK_NOLOAD)
#define TASK_NORMAL				(TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE)
#define TASK_COMM_LEN			16

#define PF_KTHREAD			0x00200000
#define PF_MEMALLOC			0x00000800
#define PF_MEMALLOC_NOFS	0x00040000
#define PF_MEMALLOC_NOIO	0x00080000
#define PF_EXITING			0x00000004
#define PF_WQ_WORKER		0x00000020

struct mm_struct;
struct pid;
struct signal_struct;
struct kthread;

struct task_struct {
	unsigned int __state;
	unsigned int flags;
	int pid;
	int tgid;
	char comm[TASK_COMM_LEN];
	struct mm_struct *mm;
	struct task_struct *group_leader;
	struct pid *thread_pid;
	int exit_code;
	int prio;
	void *lx_private;			/* Haiku side */
	struct kthread *lx_kthread;
	void *lx_work;				/* worker: the running work item */
	int lx_idle;				/* waits for work: not a hung wait */
	int lx_rcu_nesting;
};

struct task_struct *lx_get_current(void);
#define current				lx_get_current()
#define get_current()		lx_get_current()

#define set_current_state(state_value) \
	smp_store_mb(current->__state, (state_value))
#define __set_current_state(state_value) \
	WRITE_ONCE(current->__state, (state_value))
#define get_current_state()	READ_ONCE(current->__state)
#define task_is_running(task)	(READ_ONCE((task)->__state) == TASK_RUNNING)

void schedule(void);
long schedule_timeout(long timeout);
long schedule_timeout_interruptible(long timeout);
long schedule_timeout_killable(long timeout);
long schedule_timeout_uninterruptible(long timeout);
long schedule_timeout_idle(long timeout);
#define io_schedule()			schedule()
#define io_schedule_timeout(t)	schedule_timeout(t)
#define schedule_preempt_disabled()	schedule()
int schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode);
int schedule_hrtimeout_range(ktime_t *expires, u64 delta,
	const enum hrtimer_mode mode);
int wake_up_process(struct task_struct *tsk);
int wake_up_state(struct task_struct *tsk, unsigned int state);
void yield(void);
#define cond_resched()			({ 0; })
#define cond_resched_lock(lock)	({ 0; })
#define need_resched()			0
#define cpu_is_offline(cpu)		0
/* Haiku's pending signals, for the current thread only (others: none) */
int lx_haiku_signal_pending(int fatal_only);
static inline int signal_pending(struct task_struct *p)
{ return p == current && lx_haiku_signal_pending(0); }
static inline int fatal_signal_pending(struct task_struct *p)
{ return p == current && lx_haiku_signal_pending(1); }
static inline int signal_pending_state(unsigned int state, struct task_struct *p)
{
	if (!(state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)))
		return 0;
	if (state & TASK_INTERRUPTIBLE)
		return signal_pending(p);
	return fatal_signal_pending(p);
}
#define send_sig(sig, p, priv)	0
#define flush_signals(t)	do { } while (0)
#define allow_signal(sig)	do { } while (0)
#define disallow_signal(sig) do { } while (0)
#define task_pid_nr(tsk)	((tsk)->pid)
#define task_tgid_nr(tsk)	((tsk)->tgid)
#define task_pid_vnr(tsk)	((tsk)->pid)
#define task_tgid_vnr(tsk)	((tsk)->tgid)
#define task_pid(tsk)		((tsk)->thread_pid)
#define task_tgid(tsk)		((tsk)->thread_pid)
#define task_session(tsk)	((tsk)->thread_pid)
#define get_task_comm(buf, tsk) ({ strscpy(buf, (tsk)->comm, sizeof(buf)); buf; })
#define __get_task_comm(buf, len, tsk) ({ strscpy(buf, (tsk)->comm, len); buf; })
#define set_task_comm(tsk, name)	strscpy((tsk)->comm, name, TASK_COMM_LEN)
#define get_task_struct(tsk)	(tsk)
#define put_task_struct(tsk)	do { } while (0)
#define task_nice(tsk)		0
#define set_user_nice(tsk, nice)	do { } while (0)
#define rt_task(tsk)		0
#define memalloc_noreclaim_save()		0U
#define memalloc_noreclaim_restore(f)	do { (void)(f); } while (0)
#define memalloc_nofs_save()			0U
#define memalloc_nofs_restore(f)		do { (void)(f); } while (0)
#define memalloc_noio_save()			0U
#define memalloc_noio_restore(f)		do { (void)(f); } while (0)
#define in_kthread()	((current->flags & PF_KTHREAD) != 0)
struct sched_param { int sched_priority; };
void sched_set_fifo(struct task_struct *p);
void sched_set_fifo_low(struct task_struct *p);
void sched_set_normal(struct task_struct *p, int nice);
#define sched_setscheduler(p, policy, param)	({ sched_set_fifo(p); 0; })
#define sched_setscheduler_nocheck(p, policy, param)	({ sched_set_fifo(p); 0; })
#define SCHED_NORMAL	0
#define SCHED_FIFO		1
#define MAX_RT_PRIO		100

/* pids: a reference counted number */
struct pid {
	refcount_t count;
	int nr;
};
enum pid_type { PIDTYPE_PID, PIDTYPE_TGID, PIDTYPE_PGID, PIDTYPE_SID, PIDTYPE_MAX };
struct pid *get_pid(struct pid *pid);
void put_pid(struct pid *pid);
#define pid_nr(pid)		((pid) ? (pid)->nr : 0)
#define pid_vnr(pid)	pid_nr(pid)
struct task_struct *get_pid_task(struct pid *pid, enum pid_type type);
#define pid_task(pid, type)	get_pid_task(pid, type)
#define find_get_pid(nr)	NULL
#define task_active_pid_ns(t)	NULL
#define find_vpid(nr)		NULL

/* #pragma mark - wait queues */

typedef struct wait_queue_entry wait_queue_entry_t;
typedef int (*wait_queue_func_t)(struct wait_queue_entry *wq_entry,
	unsigned mode, int flags, void *key);
int default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
	int flags, void *key);
int autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
	int sync, void *key);
int woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
	int sync, void *key);

#define WQ_FLAG_EXCLUSIVE	0x01
#define WQ_FLAG_WOKEN		0x02
#define WQ_FLAG_CUSTOM		0x04
#define WQ_FLAG_DONE		0x08
#define WQ_FLAG_PRIORITY	0x10

struct wait_queue_entry {
	unsigned int flags;
	void *private;
	wait_queue_func_t func;
	struct list_head entry;
};

struct wait_queue_head {
	spinlock_t lock;
	struct list_head head;
};
typedef struct wait_queue_head wait_queue_head_t;

#define __WAITQUEUE_INITIALIZER(name, tsk) { \
	.private = tsk, .func = default_wake_function, .entry = { NULL, NULL } }
#define DECLARE_WAITQUEUE(name, tsk) \
	struct wait_queue_entry name = __WAITQUEUE_INITIALIZER(name, tsk)
#define __WAIT_QUEUE_HEAD_INITIALIZER(name) { \
	.lock = __SPIN_LOCK_UNLOCKED(name.lock), \
	.head = LIST_HEAD_INIT(name.head) }
#define DECLARE_WAIT_QUEUE_HEAD(name) \
	struct wait_queue_head name = __WAIT_QUEUE_HEAD_INITIALIZER(name)
#define DECLARE_WAIT_QUEUE_HEAD_ONSTACK(name)	DECLARE_WAIT_QUEUE_HEAD(name)
#define init_waitqueue_head(wq_head) \
	do { spin_lock_init(&(wq_head)->lock); INIT_LIST_HEAD(&(wq_head)->head); } \
	while (0)
static inline void init_waitqueue_entry(struct wait_queue_entry *wq_entry,
	struct task_struct *p)
{
	wq_entry->flags = 0;
	wq_entry->private = p;
	wq_entry->func = default_wake_function;
}
static inline void init_waitqueue_func_entry(struct wait_queue_entry *wq_entry,
	wait_queue_func_t func)
{
	wq_entry->flags = 0;
	wq_entry->private = NULL;
	wq_entry->func = func;
}
static inline int waitqueue_active(struct wait_queue_head *wq_head)
{
	return !list_empty(&wq_head->head);
}
static inline bool wq_has_sleeper(struct wait_queue_head *wq_head)
{
	smp_mb();
	return waitqueue_active(wq_head);
}
void add_wait_queue(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry);
void add_wait_queue_exclusive(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry);
void remove_wait_queue(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry);
static inline void __add_wait_queue(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry)
{
	list_add(&wq_entry->entry, &wq_head->head);
}
static inline void __add_wait_queue_entry_tail(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry)
{
	list_add_tail(&wq_entry->entry, &wq_head->head);
}
static inline void __remove_wait_queue(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry)
{
	list_del(&wq_entry->entry);
}
void __wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr,
	void *key);
void __wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr);
void __wake_up_locked_key(struct wait_queue_head *wq_head, unsigned int mode,
	void *key);
#define wake_up(x)					__wake_up(x, TASK_NORMAL, 1, NULL)
#define wake_up_nr(x, nr)			__wake_up(x, TASK_NORMAL, nr, NULL)
#define wake_up_all(x)				__wake_up(x, TASK_NORMAL, 0, NULL)
#define wake_up_locked(x)			__wake_up_locked((x), TASK_NORMAL, 1)
#define wake_up_all_locked(x)		__wake_up_locked((x), TASK_NORMAL, 0)
#define wake_up_interruptible(x)	__wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_interruptible_nr(x, nr)	__wake_up(x, TASK_INTERRUPTIBLE, nr, NULL)
#define wake_up_interruptible_all(x)	__wake_up(x, TASK_INTERRUPTIBLE, 0, NULL)
#define wake_up_interruptible_sync(x)	__wake_up(x, TASK_INTERRUPTIBLE, 1, NULL)
#define wake_up_locked_poll(x, m) \
	__wake_up_locked_key((x), TASK_NORMAL, (void *)(uintptr_t)(m))
#define wait_event_interruptible_locked(wq, condition) ({ \
	spin_unlock(&(wq).lock); \
	int __r = wait_event_interruptible(wq, condition); \
	spin_lock(&(wq).lock); \
	__r; \
})
#define wake_up_poll(x, m) \
	__wake_up(x, TASK_NORMAL, 1, (void *)(uintptr_t)(m))
#define wake_up_interruptible_poll(x, m) \
	__wake_up(x, TASK_INTERRUPTIBLE, 1, (void *)(uintptr_t)(m))
#define wake_up_all_poll(x, m) \
	__wake_up(x, TASK_NORMAL, 0, (void *)(uintptr_t)(m))
#define wake_up_var(var)	wake_up_all(lx_var_waitqueue(var))
struct wait_queue_head *lx_var_waitqueue(void *var);

void init_wait_entry(struct wait_queue_entry *wq_entry, int flags);
long prepare_to_wait_event(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry, int state);
void prepare_to_wait(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry, int state);
bool prepare_to_wait_exclusive(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry, int state);
void finish_wait(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry);
long wait_woken(struct wait_queue_entry *wq_entry, unsigned mode, long timeout);

#define DEFINE_WAIT_FUNC(name, function) \
	struct wait_queue_entry name = { \
		.private = current, \
		.func = function, \
		.entry = LIST_HEAD_INIT((name).entry), \
	}
#define DEFINE_WAIT(name)	DEFINE_WAIT_FUNC(name, autoremove_wake_function)
#define init_wait(wait) \
	do { \
		(wait)->private = current; \
		(wait)->func = autoremove_wake_function; \
		INIT_LIST_HEAD(&(wait)->entry); \
		(wait)->flags = 0; \
	} while (0)

/* Linux's wait_event() family (include/linux/wait.h) */
#define ___wait_is_interruptible(state) \
	(!__builtin_constant_p(state) || \
	 (state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)))
#define ___wait_cond_timeout(condition) ({ \
	bool __cond = (condition); \
	if (__cond && !__ret) \
		__ret = 1; \
	__cond || !__ret; \
})
#define ___wait_event(wq_head, condition, state, exclusive, ret, cmd) ({ \
	__label__ __out; \
	struct wait_queue_entry __wq_entry; \
	long __ret = ret; \
	init_wait_entry(&__wq_entry, exclusive ? WQ_FLAG_EXCLUSIVE : 0); \
	for (;;) { \
		long __int = prepare_to_wait_event(&wq_head, &__wq_entry, state); \
		if (condition) \
			break; \
		if (___wait_is_interruptible(state) && __int) { \
			__ret = __int; \
			goto __out; \
		} \
		cmd; \
	} \
	finish_wait(&wq_head, &__wq_entry); \
__out: \
	__ret; \
})
#define __wait_event(wq_head, condition) \
	(void)___wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, 0, schedule())
#define wait_event(wq_head, condition) \
do { \
	if (condition) \
		break; \
	__wait_event(wq_head, condition); \
} while (0)
#define __wait_event_timeout(wq_head, condition, timeout) \
	___wait_event(wq_head, ___wait_cond_timeout(condition), \
		TASK_UNINTERRUPTIBLE, 0, timeout, __ret = schedule_timeout(__ret))
#define wait_event_timeout(wq_head, condition, timeout) ({ \
	long __ret = timeout; \
	if (!___wait_cond_timeout(condition)) \
		__ret = __wait_event_timeout(wq_head, condition, timeout); \
	__ret; \
})
#define __wait_event_interruptible(wq_head, condition) \
	___wait_event(wq_head, condition, TASK_INTERRUPTIBLE, 0, 0, schedule())
#define wait_event_interruptible(wq_head, condition) ({ \
	int __ret = 0; \
	if (!(condition)) \
		__ret = __wait_event_interruptible(wq_head, condition); \
	__ret; \
})
#define __wait_event_interruptible_timeout(wq_head, condition, timeout) \
	___wait_event(wq_head, ___wait_cond_timeout(condition), \
		TASK_INTERRUPTIBLE, 0, timeout, __ret = schedule_timeout(__ret))
#define wait_event_interruptible_timeout(wq_head, condition, timeout) ({ \
	long __ret = timeout; \
	if (!___wait_cond_timeout(condition)) \
		__ret = __wait_event_interruptible_timeout(wq_head, condition, timeout); \
	__ret; \
})
/* killable: only a fatal signal ends them (prepare_to_wait_event()) */
#define wait_event_killable(wq_head, condition) ({ \
	int __ret = 0; \
	if (!(condition)) \
		__ret = ___wait_event(wq_head, condition, TASK_KILLABLE, 0, 0, \
			schedule()); \
	__ret; \
})
#define wait_event_killable_timeout(wq_head, condition, timeout) ({ \
	long __ret = timeout; \
	if (!___wait_cond_timeout(condition)) \
		__ret = ___wait_event(wq_head, ___wait_cond_timeout(condition), \
			TASK_KILLABLE, 0, timeout, __ret = schedule_timeout(__ret)); \
	__ret; \
})
#define wait_event_idle(wq_head, condition)	wait_event(wq_head, condition)
#define wait_event_idle_timeout(wq_head, condition, timeout) \
	wait_event_timeout(wq_head, condition, timeout)
#define wait_event_freezable(wq_head, condition) \
	wait_event_interruptible(wq_head, condition)
#define wait_event_interruptible_exclusive(wq_head, condition) \
	wait_event_interruptible(wq_head, condition)
#define __wait_event_lock_irq(wq_head, condition, lock, cmd) \
	(void)___wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, 0, \
		spin_unlock_irq(&lock); cmd; schedule(); spin_lock_irq(&lock))
#define wait_event_lock_irq(wq_head, condition, lock) \
do { \
	if (condition) \
		break; \
	__wait_event_lock_irq(wq_head, condition, lock, ); \
} while (0)
#define wait_event_interruptible_lock_irq(wq_head, condition, lock) ({ \
	wait_event_lock_irq(wq_head, condition, lock); 0; })
#define wait_event_cmd(wq_head, condition, cmd1, cmd2) \
do { \
	if (condition) \
		break; \
	(void)___wait_event(wq_head, condition, TASK_UNINTERRUPTIBLE, 0, 0, \
		cmd1; schedule(); cmd2); \
} while (0)
#define wait_var_event(var, condition) \
	wait_event(*lx_var_waitqueue(var), condition)
#define wait_var_event_timeout(var, condition, timeout) \
	wait_event_timeout(*lx_var_waitqueue(var), condition, timeout)
#define wait_var_event_killable(var, condition) \
	wait_event_killable(*lx_var_waitqueue(var), condition)

/* bit waits */
void wake_up_bit(void *word, int bit);
int wait_on_bit(unsigned long *word, int bit, unsigned mode);
int wait_on_bit_timeout(unsigned long *word, int bit, unsigned mode,
	unsigned long timeout);
#define clear_and_wake_up_bit(bit, word) \
	do { clear_bit_unlock(bit, word); smp_mb__after_atomic(); \
		wake_up_bit(word, bit); } while (0)

/* #pragma mark - completions */

struct completion {
	unsigned int done;
	struct wait_queue_head wait;
};

#define COMPLETION_INITIALIZER(work) \
	{ 0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait) }
#define DECLARE_COMPLETION(work) \
	struct completion work = COMPLETION_INITIALIZER(work)
#define DECLARE_COMPLETION_ONSTACK(work)	DECLARE_COMPLETION(work)
static inline void init_completion(struct completion *x)
{
	x->done = 0;
	init_waitqueue_head(&x->wait);
}
static inline void reinit_completion(struct completion *x)
{
	x->done = 0;
}
void complete(struct completion *x);
void complete_all(struct completion *x);
void wait_for_completion(struct completion *x);
unsigned long wait_for_completion_timeout(struct completion *x,
	unsigned long timeout);
int wait_for_completion_interruptible(struct completion *x);
long wait_for_completion_interruptible_timeout(struct completion *x,
	unsigned long timeout);
int wait_for_completion_killable(struct completion *x);
long wait_for_completion_killable_timeout(struct completion *x,
	unsigned long timeout);
#define wait_for_completion_io(x)	wait_for_completion(x)
#define wait_for_completion_io_timeout(x, t)	wait_for_completion_timeout(x, t)
bool try_wait_for_completion(struct completion *x);
bool completion_done(struct completion *x);
DEFINE_FREE(lx_unused_completion, void *, )

/* #pragma mark - kernel threads */

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
	void *data, int node, const char namefmt[], ...) __printf(4, 5);
#define kthread_create(threadfn, data, namefmt, arg...) \
	kthread_create_on_node(threadfn, data, -1, namefmt, ##arg)
#define kthread_run(threadfn, data, namefmt, ...) ({ \
	struct task_struct *__k = kthread_create(threadfn, data, namefmt, \
		## __VA_ARGS__); \
	if (!IS_ERR(__k)) \
		wake_up_process(__k); \
	__k; \
})
int kthread_stop(struct task_struct *k);
int kthread_stop_put(struct task_struct *k);
bool kthread_should_stop(void);
bool kthread_should_park(void);
int kthread_park(struct task_struct *k);
void kthread_unpark(struct task_struct *k);
void kthread_parkme(void);
#define kthread_bind(k, cpu)	do { } while (0)
#define kthread_use_mm(mm)		do { } while (0)
#define kthread_unuse_mm(mm)	do { } while (0)
#define kthread_freezable_should_stop(was_frozen)	kthread_should_stop()
#define set_freezable()			do { } while (0)
#define try_to_freeze()			0
#define freezing(task)			0

/* kthread workers: a thread running queued kthread_work items in order */
struct kthread_worker;
struct kthread_work;
typedef void (*kthread_work_func_t)(struct kthread_work *work);
struct kthread_work {
	struct list_head node;
	kthread_work_func_t func;
	struct kthread_worker *worker;
	int canceling;
};
struct kthread_delayed_work {
	struct kthread_work work;
	struct timer_list timer;
};
struct kthread_worker {
	unsigned int flags;
	spinlock_t lock;
	struct list_head work_list;
	struct list_head delayed_work_list;
	struct task_struct *task;
	struct kthread_work *current_work;
	struct wait_queue_head idle;
};
#define kthread_init_work(work, fn) \
	do { \
		memset((work), 0, sizeof(struct kthread_work)); \
		INIT_LIST_HEAD(&(work)->node); \
		(work)->func = (fn); \
	} while (0)
struct kthread_worker *kthread_create_worker(unsigned int flags,
	const char namefmt[], ...) __printf(2, 3);
#define kthread_run_worker(flags, namefmt, ...) \
	kthread_create_worker(flags, namefmt, ## __VA_ARGS__)
void kthread_destroy_worker(struct kthread_worker *worker);
bool kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work);
void kthread_flush_work(struct kthread_work *work);
void kthread_flush_worker(struct kthread_worker *worker);
bool kthread_cancel_work_sync(struct kthread_work *work);

/* #pragma mark - workqueues */

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
struct workqueue_struct;

struct work_struct {
	atomic_long_t data;			/* bit 0: pending */
	struct list_head entry;
	work_func_t func;
	struct workqueue_struct *wq;
};

struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
	struct workqueue_struct *wq;
	int cpu;
};

struct rcu_work {
	struct work_struct work;
	struct rcu_head rcu;
	struct workqueue_struct *wq;
};

#define WORK_STRUCT_PENDING_BIT	0
#define WORK_DATA_INIT()	ATOMIC_LONG_INIT(0)
#define __WORK_INITIALIZER(n, f) { \
	.data = WORK_DATA_INIT(), \
	.entry = { &(n).entry, &(n).entry }, \
	.func = (f), \
}
#define __DELAYED_WORK_INITIALIZER(n, f, tflags) { \
	.work = __WORK_INITIALIZER((n).work, (f)), \
	.timer = { .entry = LIST_HEAD_INIT((n).timer.entry), \
		.function = lx_delayed_work_timer_fn }, \
}
#define DECLARE_WORK(n, f)	struct work_struct n = __WORK_INITIALIZER(n, f)
#define DECLARE_DELAYED_WORK(n, f) \
	struct delayed_work n = __DELAYED_WORK_INITIALIZER(n, f, 0)
void lx_delayed_work_timer_fn(struct timer_list *t);

#define INIT_WORK(_work, _func) \
	do { \
		atomic_long_set(&(_work)->data, 0); \
		INIT_LIST_HEAD(&(_work)->entry); \
		(_work)->func = (_func); \
		(_work)->wq = NULL; \
	} while (0)
#define INIT_WORK_ONSTACK(_work, _func)	INIT_WORK(_work, _func)
#define INIT_DELAYED_WORK(_work, _func) \
	do { \
		INIT_WORK(&(_work)->work, (_func)); \
		timer_setup(&(_work)->timer, lx_delayed_work_timer_fn, 0); \
		(_work)->wq = NULL; \
	} while (0)
#define INIT_DELAYED_WORK_ONSTACK(_work, _func)	INIT_DELAYED_WORK(_work, _func)
#define INIT_DEFERRABLE_WORK(_work, _func)	INIT_DELAYED_WORK(_work, _func)
#define INIT_RCU_WORK(_work, _func)	INIT_WORK(&(_work)->work, (_func))
#define destroy_work_on_stack(work)	do { } while (0)
#define destroy_delayed_work_on_stack(work)	do { } while (0)
static inline struct delayed_work *to_delayed_work(struct work_struct *work)
{
	return container_of(work, struct delayed_work, work);
}
static inline struct rcu_work *to_rcu_work(struct work_struct *work)
{
	return container_of(work, struct rcu_work, work);
}
#define work_pending(work) \
	test_bit(WORK_STRUCT_PENDING_BIT, (unsigned long *)&(work)->data.counter)
#define delayed_work_pending(w)	work_pending(&(w)->work)
bool work_busy(struct work_struct *work);

enum {
	WQ_UNBOUND = 1 << 1,
	WQ_FREEZABLE = 1 << 2,
	WQ_MEM_RECLAIM = 1 << 3,
	WQ_HIGHPRI = 1 << 4,
	WQ_CPU_INTENSIVE = 1 << 5,
	WQ_SYSFS = 1 << 6,
	WQ_POWER_EFFICIENT = 1 << 7,
	WQ_PERCPU = 1 << 8,
	__WQ_DESTROYING = 1 << 15,
	__WQ_DRAINING = 1 << 16,
	__WQ_ORDERED = 1 << 17,
	__WQ_LEGACY = 1 << 18,
	WQ_MAX_ACTIVE = 512,
	WQ_UNBOUND_MAX_ACTIVE = WQ_MAX_ACTIVE,
	WQ_DFL_ACTIVE = WQ_MAX_ACTIVE / 2,
};

struct workqueue_struct *alloc_workqueue(const char *fmt, unsigned int flags,
	int max_active, ...) __printf(1, 4);
#define alloc_ordered_workqueue(fmt, flags, args...) \
	alloc_workqueue(fmt, __WQ_ORDERED | (flags), 1, ##args)
#define alloc_ordered_workqueue_lockdep_map(fmt, flags, map, args...) \
	alloc_ordered_workqueue(fmt, flags, ##args)
#define create_workqueue(name)	alloc_workqueue("%s", WQ_MEM_RECLAIM, 1, (name))
#define create_freezable_workqueue(name) \
	alloc_workqueue("%s", WQ_FREEZABLE | WQ_UNBOUND | WQ_MEM_RECLAIM, 1, (name))
#define create_singlethread_workqueue(name) \
	alloc_ordered_workqueue("%s", WQ_MEM_RECLAIM, name)
void destroy_workqueue(struct workqueue_struct *wq);
bool queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work);
#define queue_work(wq, work)	queue_work_on(-1, wq, work)
#define queue_work_node(node, wq, work)	queue_work_on(-1, wq, work)
bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
	struct delayed_work *work, unsigned long delay);
#define queue_delayed_work(wq, dwork, delay) \
	queue_delayed_work_on(-1, wq, dwork, delay)
bool mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
	struct delayed_work *dwork, unsigned long delay);
#define mod_delayed_work(wq, dwork, delay) mod_delayed_work_on(-1, wq, dwork, delay)
bool queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork);
void flush_workqueue(struct workqueue_struct *wq);
void drain_workqueue(struct workqueue_struct *wq);
bool flush_work(struct work_struct *work);
bool cancel_work(struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
bool flush_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work(struct delayed_work *dwork);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
#define disable_work_sync(w)		cancel_work_sync(w)
#define disable_delayed_work_sync(w)	cancel_delayed_work_sync(w)
#define enable_work(w)			true
#define enable_delayed_work(w)	true
bool flush_rcu_work(struct rcu_work *rwork);
struct work_struct *current_work(void);
#define current_is_workqueue_rescuer()	false
#define workqueue_set_max_active(wq, max)	do { } while (0)
#define set_workqueue_cpumask(wq, mask)	0

extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_highpri_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
extern struct workqueue_struct *system_freezable_wq;
extern struct workqueue_struct *system_power_efficient_wq;
extern struct workqueue_struct *system_freezable_power_efficient_wq;
#define system_percpu_wq	system_wq
#define system_dfl_wq		system_unbound_wq
#define schedule_work(work)		queue_work(system_wq, work)
#define schedule_work_on(cpu, work)	queue_work(system_wq, work)
#define schedule_delayed_work(dwork, delay) \
	queue_delayed_work(system_wq, dwork, delay)
#define schedule_delayed_work_on(cpu, dwork, delay) \
	queue_delayed_work(system_wq, dwork, delay)
#define flush_scheduled_work()	flush_workqueue(system_wq)

/* irq_work: run "soon" from a thread */
#include <linux/llist.h>
struct irq_work {
	struct work_struct work;
	void (*func)(struct irq_work *);
	struct {
		struct llist_node llist;	/* for the owner's lists (i915) */
		unsigned int u_flags;
	} node;
};
void lx_irq_work_fn(struct work_struct *work);
#define init_irq_work(w, f) \
	do { INIT_WORK(&(w)->work, lx_irq_work_fn); (w)->func = (f); } while (0)
#define IRQ_WORK_INIT(f)	{ .work = __WORK_INITIALIZER(work, lx_irq_work_fn), .func = (f) }
static inline bool irq_work_queue(struct irq_work *w)
{
	return queue_work(system_highpri_wq, &w->work);
}
#define irq_work_queue_on(w, cpu)	irq_work_queue(w)
#define irq_work_sync(w)	flush_work(&(w)->work)
#define irq_work_is_pending(w)	work_pending(&(w)->work)
#define irq_work_is_busy(w)	work_busy(&(w)->work)

#endif /* _LX_SCHED_H */
