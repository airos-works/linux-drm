/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Wait queues, completions and the hashed wait queues of wait_var_event()
	and the bit waits: Linux's algorithms (kernel/sched/wait.c,
	completion.c) on the task sleep of sched.c. */

#include <lx/haiku.h>


int
default_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
	int flags, void *key)
{
	return wake_up_state(wq_entry->private, mode);
}


int
autoremove_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
	int sync, void *key)
{
	int ret = default_wake_function(wq_entry, mode, sync, key);
	if (ret)
		list_del_init_careful(&wq_entry->entry);
	return ret;
}


int
woken_wake_function(struct wait_queue_entry *wq_entry, unsigned mode,
	int sync, void *key)
{
	smp_mb();
	wq_entry->flags |= WQ_FLAG_WOKEN;
	return default_wake_function(wq_entry, mode, sync, key);
}


void
add_wait_queue(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry)
{
	wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq_head->lock);
	__add_wait_queue(wq_head, wq_entry);
	spin_unlock(&wq_head->lock);
}


void
add_wait_queue_exclusive(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry)
{
	wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq_head->lock);
	__add_wait_queue_entry_tail(wq_head, wq_entry);
	spin_unlock(&wq_head->lock);
}


void
remove_wait_queue(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry)
{
	spin_lock(&wq_head->lock);
	__remove_wait_queue(wq_head, wq_entry);
	spin_unlock(&wq_head->lock);
}


static int
__wake_up_common(struct wait_queue_head *wq_head, unsigned int mode,
	int nr_exclusive, int wake_flags, void *key)
{
	struct wait_queue_entry *curr, *next;

	list_for_each_entry_safe(curr, next, &wq_head->head, entry) {
		unsigned flags = curr->flags;
		int ret = curr->func(curr, mode, wake_flags, key);
		if (ret < 0)
			break;
		if (ret && (flags & WQ_FLAG_EXCLUSIVE) && !--nr_exclusive)
			break;
	}
	return nr_exclusive;
}


void
__wake_up(struct wait_queue_head *wq_head, unsigned int mode, int nr_exclusive,
	void *key)
{
	spin_lock(&wq_head->lock);
	__wake_up_common(wq_head, mode, nr_exclusive, 0, key);
	spin_unlock(&wq_head->lock);
}


void
__wake_up_locked(struct wait_queue_head *wq_head, unsigned int mode, int nr)
{
	__wake_up_common(wq_head, mode, nr, 0, NULL);
}


void
__wake_up_locked_key(struct wait_queue_head *wq_head, unsigned int mode,
	void *key)
{
	__wake_up_common(wq_head, mode, 1, 0, key);
}


void
init_wait_entry(struct wait_queue_entry *wq_entry, int flags)
{
	wq_entry->flags = flags;
	wq_entry->private = current;
	wq_entry->func = autoremove_wake_function;
	INIT_LIST_HEAD(&wq_entry->entry);
}


/*	As on Linux, a signal ends an interruptible (or, a fatal one, a killable)
	wait_event*() here, before the next sleep: the sleep itself returns at
	once while a signal is pending, so without this the wait went round
	until its condition came true or its time ran out - and its time does
	not run out, since schedule_timeout() rounds what is left up to whole
	jiffies. A thread killed in DRM_HAIKU_IOCTL_WAIT_HOTPLUG (app_server's
	hot plug thread) spun in the kernel for good, and its team never died
	(haiku#134). */
long
prepare_to_wait_event(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry, int state)
{
	spin_lock(&wq_head->lock);
	if (signal_pending_state(state, current)) {
		/* an exclusive waiter that a wake up chose and that leaves now
		 * finds its condition true in ___wait_event() and consumes it */
		list_del_init(&wq_entry->entry);
		spin_unlock(&wq_head->lock);
		return -ERESTARTSYS;
	}
	if (list_empty(&wq_entry->entry)) {
		if (wq_entry->flags & WQ_FLAG_EXCLUSIVE)
			__add_wait_queue_entry_tail(wq_head, wq_entry);
		else
			__add_wait_queue(wq_head, wq_entry);
	}
	set_current_state(state);
	spin_unlock(&wq_head->lock);
	return 0;
}


void
prepare_to_wait(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry, int state)
{
	wq_entry->flags &= ~WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq_head->lock);
	if (list_empty(&wq_entry->entry))
		__add_wait_queue(wq_head, wq_entry);
	set_current_state(state);
	spin_unlock(&wq_head->lock);
}


bool
prepare_to_wait_exclusive(struct wait_queue_head *wq_head,
	struct wait_queue_entry *wq_entry, int state)
{
	bool was_empty = false;
	wq_entry->flags |= WQ_FLAG_EXCLUSIVE;
	spin_lock(&wq_head->lock);
	if (list_empty(&wq_entry->entry)) {
		was_empty = list_empty(&wq_head->head);
		__add_wait_queue_entry_tail(wq_head, wq_entry);
	}
	set_current_state(state);
	spin_unlock(&wq_head->lock);
	return was_empty;
}


void
finish_wait(struct wait_queue_head *wq_head, struct wait_queue_entry *wq_entry)
{
	__set_current_state(TASK_RUNNING);
	if (!list_empty_careful(&wq_entry->entry)) {
		spin_lock(&wq_head->lock);
		list_del_init(&wq_entry->entry);
		spin_unlock(&wq_head->lock);
	}
}


long
wait_woken(struct wait_queue_entry *wq_entry, unsigned mode, long timeout)
{
	set_current_state(mode);
	if (!(wq_entry->flags & WQ_FLAG_WOKEN))
		timeout = schedule_timeout(timeout);
	__set_current_state(TASK_RUNNING);
	smp_store_mb(wq_entry->flags, wq_entry->flags & ~WQ_FLAG_WOKEN);
	return timeout;
}


/* #pragma mark - hashed wait queues */


#define LX_WAIT_TABLE_BITS	8
static struct wait_queue_head lx_wait_table[1 << LX_WAIT_TABLE_BITS];
static bool lx_wait_table_ready;

struct wait_queue_head *
lx_var_waitqueue(void *var)
{
	if (unlikely(!READ_ONCE(lx_wait_table_ready))) {
		static DEFINE_SPINLOCK(init_lock);
		spin_lock(&init_lock);
		if (!lx_wait_table_ready) {
			int i;
			for (i = 0; i < (1 << LX_WAIT_TABLE_BITS); i++)
				init_waitqueue_head(&lx_wait_table[i]);
			smp_store_release(&lx_wait_table_ready, true);
		}
		spin_unlock(&init_lock);
	}
	return &lx_wait_table[hash_ptr(var, LX_WAIT_TABLE_BITS)];
}


void
wake_up_bit(void *word, int bit)
{
	wake_up_all(lx_var_waitqueue((char *)word + bit));
}


int
wait_on_bit(unsigned long *word, int bit, unsigned mode)
{
	wait_event(*lx_var_waitqueue((char *)word + bit), !test_bit(bit, word));
	return 0;
}


int
wait_on_bit_timeout(unsigned long *word, int bit, unsigned mode,
	unsigned long timeout)
{
	return wait_event_timeout(*lx_var_waitqueue((char *)word + bit),
		!test_bit(bit, word), timeout) ? 0 : -EAGAIN;
}


/* #pragma mark - completions */


void
complete(struct completion *x)
{
	spin_lock(&x->wait.lock);
	if (x->done != UINT_MAX)
		x->done++;
	__wake_up_locked(&x->wait, TASK_NORMAL, 1);
	spin_unlock(&x->wait.lock);
}


void
complete_all(struct completion *x)
{
	spin_lock(&x->wait.lock);
	x->done = UINT_MAX;
	__wake_up_locked(&x->wait, TASK_NORMAL, 0);
	spin_unlock(&x->wait.lock);
}


static long
do_wait_for_common(struct completion *x, long timeout, int state)
{
	if (!x->done) {
		DECLARE_WAITQUEUE(wait, current);
		wait.flags |= WQ_FLAG_EXCLUSIVE;
		__add_wait_queue_entry_tail(&x->wait, &wait);
		do {
			/* a signal ends an interruptible or killable wait, as on
			 * Linux (see prepare_to_wait_event()) */
			if (signal_pending_state(state, current)) {
				timeout = -ERESTARTSYS;
				break;
			}
			__set_current_state(state);
			spin_unlock(&x->wait.lock);
			timeout = schedule_timeout(timeout);
			spin_lock(&x->wait.lock);
		} while (!x->done && timeout);
		__remove_wait_queue(&x->wait, &wait);
		if (!x->done)
			return timeout;
	}
	if (x->done != UINT_MAX)
		x->done--;
	return timeout ?: 1;
}


static long
wait_for_common(struct completion *x, long timeout, int state)
{
	spin_lock(&x->wait.lock);
	timeout = do_wait_for_common(x, timeout, state);
	spin_unlock(&x->wait.lock);
	return timeout;
}


void
wait_for_completion(struct completion *x)
{
	wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_UNINTERRUPTIBLE);
}


unsigned long
wait_for_completion_timeout(struct completion *x, unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_UNINTERRUPTIBLE);
}


int
wait_for_completion_interruptible(struct completion *x)
{
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_INTERRUPTIBLE);
	return t == -ERESTARTSYS ? (int)t : 0;
}


long
wait_for_completion_interruptible_timeout(struct completion *x,
	unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_INTERRUPTIBLE);
}


int
wait_for_completion_killable(struct completion *x)
{
	long t = wait_for_common(x, MAX_SCHEDULE_TIMEOUT, TASK_KILLABLE);
	return t == -ERESTARTSYS ? (int)t : 0;
}


long
wait_for_completion_killable_timeout(struct completion *x,
	unsigned long timeout)
{
	return wait_for_common(x, timeout, TASK_KILLABLE);
}


bool
try_wait_for_completion(struct completion *x)
{
	bool ret = true;
	if (!READ_ONCE(x->done))
		return false;
	spin_lock(&x->wait.lock);
	if (!x->done)
		ret = false;
	else if (x->done != UINT_MAX)
		x->done--;
	spin_unlock(&x->wait.lock);
	return ret;
}


bool
completion_done(struct completion *x)
{
	if (!READ_ONCE(x->done))
		return false;
	spin_lock(&x->wait.lock);
	spin_unlock(&x->wait.lock);
	return true;
}
