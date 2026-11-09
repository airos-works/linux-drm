/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Tasks, sleeping and waking, kernel threads and kthread workers.

	Each task has a binary event. A sleeping task sets its state, then
	schedule() waits on the event unless a waker has already set the state
	back to TASK_RUNNING (and signalled the event). A stale signal only causes
	a spurious wakeup, which Linux's wait loops tolerate. */

#include <lx/haiku.h>


struct lx_task_private {
	void *event;
	lx_thread_id thread;
};

struct kthread {
	int (*threadfn)(void *);
	void *data;
	unsigned long flags;
	struct completion exited;
	struct completion parked;
	int result;
	lx_thread_id thread;
};

#define KTHREAD_SHOULD_STOP	0
#define KTHREAD_SHOULD_PARK	1
#define KTHREAD_IS_PARKED	2

static struct pid lx_kernel_pid = { .count = REFCOUNT_INIT(1), .nr = 0 };


/* #pragma mark - teams' mms */

static LIST_HEAD(lx_team_mms);
static DEFINE_SPINLOCK(lx_team_mms_lock);


static struct mm_struct *
lx_team_mm_find(int team)
{
	struct mm_struct *mm;
	list_for_each_entry(mm, &lx_team_mms, lx_link) {
		if (mm->lx_team == team && atomic_inc_not_zero(&mm->mm_count))
			return mm;
	}
	return NULL;
}


/* the team's mm, with a reference */
static struct mm_struct *
lx_team_mm_get(int team)
{
	struct mm_struct *mm, *other;

	spin_lock(&lx_team_mms_lock);
	mm = lx_team_mm_find(team);
	spin_unlock(&lx_team_mms_lock);
	if (mm != NULL)
		return mm;

	mm = kzalloc(sizeof(*mm), GFP_KERNEL);
	if (mm == NULL)
		return NULL;
	init_rwsem(&mm->mmap_lock);
	atomic_set(&mm->mm_users, 1);
	atomic_set(&mm->mm_count, 1);
	mm->lx_team = team;
	spin_lock(&lx_team_mms_lock);
	other = lx_team_mm_find(team);
	if (other == NULL)
		list_add(&mm->lx_link, &lx_team_mms);
	spin_unlock(&lx_team_mms_lock);
	if (other != NULL) {
		kfree(mm);
		return other;
	}
	return mm;
}


void
mmdrop(struct mm_struct *mm)
{
	bool last;

	spin_lock(&lx_team_mms_lock);
	last = atomic_dec_and_test(&mm->mm_count);
	if (last)
		list_del(&mm->lx_link);
	spin_unlock(&lx_team_mms_lock);
	if (last)
		kfree(mm);
}


/* #pragma mark - tasks */


static void
lx_task_free(void *_task)
{
	struct task_struct *task = _task;
	struct lx_task_private *priv = task->lx_private;
	if (task->lx_kthread != NULL)
		return;		/* kthread_stop() frees it */
	if (task->mm != NULL)
		mmdrop(task->mm);
	if (priv != NULL) {
		lx_haiku_event_delete(priv->event);
		kfree(priv);
	}
	if (task->thread_pid != NULL && task->thread_pid != &lx_kernel_pid)
		put_pid(task->thread_pid);
	kfree(task);
}


static struct task_struct *
lx_task_create(lx_thread_id thread, bool kernel_thread)
{
	struct task_struct *task = kzalloc(sizeof(*task), GFP_KERNEL);
	struct lx_task_private *priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (task == NULL || priv == NULL)
		goto fail;
	priv->event = lx_haiku_event_create();
	if (priv->event == NULL)
		goto fail;
	priv->thread = thread;
	task->lx_private = priv;
	task->__state = TASK_RUNNING;
	task->pid = thread;
	task->group_leader = task;
	if (kernel_thread) {
		task->flags = PF_KTHREAD;
		task->tgid = 0;
		task->thread_pid = &lx_kernel_pid;
	} else {
		struct pid *pid = kzalloc(sizeof(*pid), GFP_KERNEL);
		task->tgid = lx_haiku_current_team();
		if (pid != NULL) {
			refcount_set(&pid->count, 1);
			pid->nr = task->tgid;
		}
		task->thread_pid = pid != NULL ? pid : &lx_kernel_pid;
		/* Haiku's own kernel threads have no mm, as Linux kernel threads */
		if (task->tgid != LX_HAIKU_KERNEL_TEAM)
			task->mm = lx_team_mm_get(task->tgid);
	}
	lx_haiku_thread_name(thread, task->comm, sizeof(task->comm));
	if (!kernel_thread)
		lx_haiku_team_name(task->tgid, task->comm, sizeof(task->comm));
	return task;

fail:
	if (priv != NULL && priv->event != NULL)
		lx_haiku_event_delete(priv->event);
	kfree(priv);
	kfree(task);
	return NULL;
}


struct task_struct *
lx_get_current(void)
{
	static bool hook_installed;
	lx_thread_id thread = lx_haiku_current_thread();
	struct task_struct *task = lx_haiku_task_lookup(thread);
	if (likely(task != NULL))
		return task;

	if (!READ_ONCE(hook_installed)) {
		WRITE_ONCE(hook_installed, true);
		lx_haiku_set_task_exit_hook(lx_task_free);
	}

	task = lx_task_create(thread, false);
	if (task == NULL)
		lx_haiku_panic("linux compat: no memory for a task");
	lx_haiku_task_register(thread, task);
	return task;
}


/* #pragma mark - sleeping and waking */


/*	Bring-up aid: waits without a timeout that last longer than this many
	seconds get the waiting thread's stack logged (0: off). */
int hung_wait_s = 30;
module_param(hung_wait_s, int, 0644);


static long
lx_schedule_us(long long timeout_us)
{
	struct task_struct *task = current;
	struct lx_task_private *priv = task->lx_private;
	long long start = lx_haiku_system_time();
	long long remaining;
	unsigned int state = READ_ONCE(task->__state);
	int interrupt;

	if (state == TASK_RUNNING)
		return 1;

	/* a signal ends interruptible waits (the waiter checks signal_pending()) */
	interrupt = (state & TASK_INTERRUPTIBLE) ? LX_HAIKU_WAIT_INTERRUPTIBLE
		: (state & TASK_WAKEKILL) ? LX_HAIKU_WAIT_KILLABLE
		: LX_HAIKU_WAIT_UNINTERRUPTIBLE;
	if (timeout_us < 0 && hung_wait_s > 0 && !task->lx_idle) {
		/* Linux's hung task detector, per wait: a wait that does not end
		 * gets its stack logged once */
		bool reported = false;
		while (lx_haiku_event_wait_etc(priv->event,
				(long long)hung_wait_s * 1000000, interrupt) == -ETIMEDOUT) {
			if (!reported) {
				char what[96];
				snprintf(what, sizeof(what), "waiting for %d s (state %#x)",
					hung_wait_s, state);
				lx_haiku_log_stack(what);
				reported = true;
			}
		}
		if (reported)
			pr_info("lx: thread %d: the wait ended\n", task->pid);
	} else
		lx_haiku_event_wait_etc(priv->event, timeout_us, interrupt);
	WRITE_ONCE(task->__state, TASK_RUNNING);

	if (timeout_us < 0)
		return 1;
	remaining = timeout_us - (lx_haiku_system_time() - start);
	return remaining > 0 ? remaining : 0;
}


void
schedule(void)
{
	lx_schedule_us(-1);
}


long
schedule_timeout(long timeout)
{
	long long remaining_us;
	if (timeout == MAX_SCHEDULE_TIMEOUT || timeout < 0) {
		schedule();
		return MAX_SCHEDULE_TIMEOUT;
	}
	if (READ_ONCE(current->__state) == TASK_RUNNING)
		return timeout;
	remaining_us = lx_schedule_us((long long)jiffies_to_usecs(timeout));
	return remaining_us > 0 ? (long)usecs_to_jiffies(remaining_us) : 0;
}


long
schedule_timeout_interruptible(long timeout)
{
	__set_current_state(TASK_INTERRUPTIBLE);
	return schedule_timeout(timeout);
}


long
schedule_timeout_killable(long timeout)
{
	__set_current_state(TASK_KILLABLE);
	return schedule_timeout(timeout);
}


long
schedule_timeout_uninterruptible(long timeout)
{
	__set_current_state(TASK_UNINTERRUPTIBLE);
	return schedule_timeout(timeout);
}


long
schedule_timeout_idle(long timeout)
{
	__set_current_state(TASK_IDLE);
	return schedule_timeout(timeout);
}


int
schedule_hrtimeout_range(ktime_t *expires, u64 delta,
	const enum hrtimer_mode mode)
{
	long long timeout_us;
	if (expires == NULL) {
		schedule();
		return -EINTR;
	}
	timeout_us = (mode & HRTIMER_MODE_REL) ? ktime_to_us(*expires)
		: ktime_to_us(*expires - ktime_get());
	if (timeout_us <= 0) {
		__set_current_state(TASK_RUNNING);
		return 0;
	}
	return lx_schedule_us(timeout_us) > 0 ? -EINTR : 0;
}


int
schedule_hrtimeout(ktime_t *expires, const enum hrtimer_mode mode)
{
	return schedule_hrtimeout_range(expires, 0, mode);
}


int lx_kthread_start_if_new(struct task_struct *task);

int
wake_up_state(struct task_struct *task, unsigned int state)
{
	struct lx_task_private *priv = task->lx_private;
	unsigned int old;
	if (unlikely(task->lx_kthread != NULL) && lx_kthread_start_if_new(task))
		return 1;
	old = READ_ONCE(task->__state);
	do {
		if (!(old & state))
			return 0;
	} while (!__atomic_compare_exchange_n(&task->__state, &old, TASK_RUNNING,
		false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
	lx_haiku_event_signal(priv->event);
	return 1;
}


int
wake_up_process(struct task_struct *task)
{
	return wake_up_state(task, TASK_NORMAL);
}


void
yield(void)
{
	lx_haiku_yield();
}


void
sched_set_fifo(struct task_struct *task)
{
	struct lx_task_private *priv = task->lx_private;
	lx_haiku_set_priority(priv->thread, LX_HAIKU_PRIORITY_URGENT);
}


void
sched_set_fifo_low(struct task_struct *task)
{
	struct lx_task_private *priv = task->lx_private;
	lx_haiku_set_priority(priv->thread, LX_HAIKU_PRIORITY_DISPLAY);
}


void
sched_set_normal(struct task_struct *task, int nice)
{
	struct lx_task_private *priv = task->lx_private;
	lx_haiku_set_priority(priv->thread, LX_HAIKU_PRIORITY_NORMAL);
}


/* #pragma mark - pids */


struct pid *
get_pid(struct pid *pid)
{
	if (pid != NULL && pid != &lx_kernel_pid)
		refcount_inc(&pid->count);
	return pid;
}


void
put_pid(struct pid *pid)
{
	if (pid != NULL && pid != &lx_kernel_pid
		&& refcount_dec_and_test(&pid->count))
		kfree(pid);
}


struct task_struct *
get_pid_task(struct pid *pid, enum pid_type type)
{
	/* only used to name the task of a DRM client; ours is enough */
	return current;
}


/* #pragma mark - kernel threads */


static int
kthread_main(void *_task)
{
	struct task_struct *task = _task;
	struct kthread *kthread = task->lx_kthread;
	struct lx_task_private *priv = task->lx_private;

	/* kthread_stop() frees the task, not the end of this thread: see
	 * lx_haiku_task_register_owned() */
	lx_haiku_task_register_owned(priv->thread, task);

	/* Linux threads start stopped until wake_up_process() */
	if (!test_bit(KTHREAD_SHOULD_STOP, &kthread->flags))
		kthread->result = kthread->threadfn(kthread->data);
	else
		kthread->result = -EINTR;

	complete_all(&kthread->exited);
	return 0;
}


struct task_struct *
kthread_create_on_node(int (*threadfn)(void *data), void *data, int node,
	const char namefmt[], ...)
{
	struct task_struct *task;
	struct kthread *kthread;
	char name[32];
	va_list args;
	lx_thread_id thread;

	va_start(args, namefmt);
	vsnprintf(name, sizeof(name), namefmt, args);
	va_end(args);

	kthread = kzalloc(sizeof(*kthread), GFP_KERNEL);
	if (kthread == NULL)
		return ERR_PTR(-ENOMEM);
	kthread->threadfn = threadfn;
	kthread->data = data;
	init_completion(&kthread->exited);
	init_completion(&kthread->parked);

	/* the task exists before the thread, so that wake_up_process() works */
	task = lx_task_create(0, true);
	if (task == NULL) {
		kfree(kthread);
		return ERR_PTR(-ENOMEM);
	}
	task->lx_kthread = kthread;
	strscpy(task->comm, name, sizeof(task->comm));

	thread = lx_haiku_spawn(kthread_main, name, LX_HAIKU_PRIORITY_NORMAL, task);
	if (thread < 0) {
		task->lx_kthread = NULL;
		lx_task_free(task);
		kfree(kthread);
		return ERR_PTR(-ENOMEM);
	}
	((struct lx_task_private *)task->lx_private)->thread = thread;
	kthread->thread = thread;
	task->pid = thread;
	/* not running yet: the first wake_up_process() resumes it */
	task->__state = TASK_UNINTERRUPTIBLE | TASK_NEW;
	return task;
}


/* wake_up_process() of a new kthread starts it */
int
lx_kthread_start_if_new(struct task_struct *task)
{
	unsigned int old = READ_ONCE(task->__state);
	if (task->lx_kthread == NULL || !(old & TASK_NEW))
		return 0;
	if (!__atomic_compare_exchange_n(&task->__state, &old, TASK_RUNNING,
			false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
		return 0;
	lx_haiku_resume(task->lx_kthread->thread);
	return 1;
}


static struct kthread *
to_kthread(struct task_struct *task)
{
	return task->lx_kthread;
}


bool
kthread_should_stop(void)
{
	struct kthread *kthread = to_kthread(current);
	return kthread != NULL && test_bit(KTHREAD_SHOULD_STOP, &kthread->flags);
}


bool
kthread_should_park(void)
{
	struct kthread *kthread = to_kthread(current);
	return kthread != NULL && test_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
}


void
kthread_parkme(void)
{
	struct kthread *kthread = to_kthread(current);
	if (kthread == NULL)
		return;
	for (;;) {
		set_current_state(TASK_PARKED);
		if (!test_bit(KTHREAD_SHOULD_PARK, &kthread->flags))
			break;
		if (!test_and_set_bit(KTHREAD_IS_PARKED, &kthread->flags))
			complete(&kthread->parked);
		current->lx_idle = 1;
		schedule();
		current->lx_idle = 0;
	}
	clear_bit(KTHREAD_IS_PARKED, &kthread->flags);
	__set_current_state(TASK_RUNNING);
}


int
kthread_park(struct task_struct *task)
{
	struct kthread *kthread = to_kthread(task);
	if (kthread == NULL)
		return -ENOSYS;
	if (!test_and_set_bit(KTHREAD_SHOULD_PARK, &kthread->flags)) {
		if (task != current) {
			wake_up_process(task);
			wait_for_completion(&kthread->parked);
		}
	}
	return 0;
}


void
kthread_unpark(struct task_struct *task)
{
	struct kthread *kthread = to_kthread(task);
	if (kthread == NULL)
		return;
	clear_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
	reinit_completion(&kthread->parked);
	wake_up_state(task, TASK_PARKED);
}


int
kthread_stop(struct task_struct *task)
{
	struct kthread *kthread = to_kthread(task);
	struct lx_task_private *priv = task->lx_private;
	int result;

	if (kthread == NULL)
		return -EINVAL;

	set_bit(KTHREAD_SHOULD_STOP, &kthread->flags);
	kthread_unpark(task);
	if (!lx_kthread_start_if_new(task))
		wake_up_process(task);
	wait_for_completion(&kthread->exited);
	lx_haiku_wait_for_thread(kthread->thread);
	result = kthread->result;

	lx_haiku_task_unregister(kthread->thread);
	task->lx_kthread = NULL;
	kfree(kthread);
	lx_haiku_event_delete(priv->event);
	kfree(priv);
	kfree(task);
	return result;
}


int
kthread_stop_put(struct task_struct *task)
{
	return kthread_stop(task);
}


/* #pragma mark - kthread workers */


static int
kthread_worker_main(void *data)
{
	struct kthread_worker *worker = data;

	for (;;) {
		struct kthread_work *work = NULL;

		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			break;
		}

		spin_lock(&worker->lock);
		if (!list_empty(&worker->work_list)) {
			work = list_first_entry(&worker->work_list, struct kthread_work,
				node);
			list_del_init(&work->node);
		}
		worker->current_work = work;
		spin_unlock(&worker->lock);

		if (work != NULL) {
			__set_current_state(TASK_RUNNING);
			work->func(work);
			spin_lock(&worker->lock);
			worker->current_work = NULL;
			spin_unlock(&worker->lock);
			wake_up_all(&worker->idle);
		} else {
			current->lx_idle = 1;
			schedule();
			current->lx_idle = 0;
		}
	}
	return 0;
}


struct kthread_worker *
kthread_create_worker(unsigned int flags, const char namefmt[], ...)
{
	struct kthread_worker *worker;
	struct task_struct *task;
	char name[32];
	va_list args;

	va_start(args, namefmt);
	vsnprintf(name, sizeof(name), namefmt, args);
	va_end(args);

	worker = kzalloc(sizeof(*worker), GFP_KERNEL);
	if (worker == NULL)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&worker->lock);
	INIT_LIST_HEAD(&worker->work_list);
	INIT_LIST_HEAD(&worker->delayed_work_list);
	init_waitqueue_head(&worker->idle);
	worker->flags = flags;

	task = kthread_create(kthread_worker_main, worker, "%s", name);
	if (IS_ERR(task)) {
		kfree(worker);
		return ERR_CAST(task);
	}
	worker->task = task;
	wake_up_process(task);
	return worker;
}


bool
kthread_queue_work(struct kthread_worker *worker, struct kthread_work *work)
{
	bool queued = false;
	spin_lock(&worker->lock);
	if (list_empty(&work->node) && worker->current_work != work) {
		work->worker = worker;
		list_add_tail(&work->node, &worker->work_list);
		queued = true;
	} else if (list_empty(&work->node)) {
		/* running now: queue it again behind itself */
		work->worker = worker;
		list_add_tail(&work->node, &worker->work_list);
		queued = true;
	}
	spin_unlock(&worker->lock);
	if (queued)
		wake_up_process(worker->task);
	return queued;
}


static bool
kthread_work_busy(struct kthread_worker *worker, struct kthread_work *work)
{
	bool busy;
	spin_lock(&worker->lock);
	busy = !list_empty(&work->node) || worker->current_work == work;
	spin_unlock(&worker->lock);
	return busy;
}


void
kthread_flush_work(struct kthread_work *work)
{
	struct kthread_worker *worker = READ_ONCE(work->worker);
	if (worker == NULL)
		return;
	wait_event(worker->idle, !kthread_work_busy(worker, work));
}


void
kthread_flush_worker(struct kthread_worker *worker)
{
	wait_event(worker->idle, ({
		bool empty;
		spin_lock(&worker->lock);
		empty = list_empty(&worker->work_list) && worker->current_work == NULL;
		spin_unlock(&worker->lock);
		empty;
	}));
}


bool
kthread_cancel_work_sync(struct kthread_work *work)
{
	struct kthread_worker *worker = READ_ONCE(work->worker);
	bool pending = false;
	if (worker == NULL)
		return false;
	spin_lock(&worker->lock);
	if (!list_empty(&work->node)) {
		list_del_init(&work->node);
		pending = true;
	}
	spin_unlock(&worker->lock);
	kthread_flush_work(work);
	return pending;
}


void
kthread_destroy_worker(struct kthread_worker *worker)
{
	kthread_flush_worker(worker);
	kthread_stop(worker->task);
	kfree(worker);
}
