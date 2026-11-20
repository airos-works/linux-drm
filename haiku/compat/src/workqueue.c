/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Workqueues: each one has its own worker threads (one for ordered
	queues). A work item is pending from queue_work() until a worker takes
	it; it may be queued again while it runs, as in Linux. Delayed work is a
	timer that queues the work when it fires. */

#include <lx/haiku.h>


#define LX_WQ_MAX_WORKERS	4

struct lx_worker {
	struct task_struct *task;
	struct work_struct *current_work;
	struct workqueue_struct *wq;
};

struct workqueue_struct {
	char name[32];
	unsigned int flags;
	spinlock_t lock;
	struct list_head queue;
	unsigned int inflight;			/* queued or running */
	int nr_workers;
	struct lx_worker workers[LX_WQ_MAX_WORKERS];
	struct wait_queue_head more_work;
	struct wait_queue_head done;
	bool stopping;
};

struct workqueue_struct *system_wq;
struct workqueue_struct *system_highpri_wq;
struct workqueue_struct *system_long_wq;
struct workqueue_struct *system_unbound_wq;
struct workqueue_struct *system_freezable_wq;
struct workqueue_struct *system_power_efficient_wq;
struct workqueue_struct *system_freezable_power_efficient_wq;

static unsigned long *
work_data_bits(struct work_struct *work)
{
	return (unsigned long *)&work->data.counter;
}


static struct lx_worker *
current_worker(void)
{
	struct task_struct *task = current;
	return task->lx_work;
}


static int
worker_main(void *data)
{
	struct lx_worker *worker = data;
	struct workqueue_struct *wq = worker->wq;

	current->lx_work = worker;
	current->flags |= PF_WQ_WORKER;

	for (;;) {
		struct work_struct *work = NULL;

		spin_lock(&wq->lock);
		while (list_empty(&wq->queue) && !wq->stopping) {
			DEFINE_WAIT(wait);
			prepare_to_wait(&wq->more_work, &wait, TASK_UNINTERRUPTIBLE);
			spin_unlock(&wq->lock);
			current->lx_idle = 1;
			schedule();
			current->lx_idle = 0;
			finish_wait(&wq->more_work, &wait);
			spin_lock(&wq->lock);
		}
		if (list_empty(&wq->queue) && wq->stopping) {
			spin_unlock(&wq->lock);
			break;
		}
		work = list_first_entry(&wq->queue, struct work_struct, entry);
		list_del_init(&work->entry);
		worker->current_work = work;
		/* from here on it can be queued again */
		clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
		spin_unlock(&wq->lock);

		work->func(work);

		spin_lock(&wq->lock);
		worker->current_work = NULL;
		wq->inflight--;
		spin_unlock(&wq->lock);
		wake_up_all(&wq->done);
	}
	return 0;
}


struct workqueue_struct *
alloc_workqueue(const char *fmt, unsigned int flags, int max_active, ...)
{
	struct workqueue_struct *wq;
	va_list args;
	int i, workers;

	wq = kzalloc(sizeof(*wq), GFP_KERNEL);
	if (wq == NULL)
		return NULL;
	va_start(args, max_active);
	vsnprintf(wq->name, sizeof(wq->name), fmt, args);
	va_end(args);
	wq->flags = flags;
	spin_lock_init(&wq->lock);
	INIT_LIST_HEAD(&wq->queue);
	init_waitqueue_head(&wq->more_work);
	init_waitqueue_head(&wq->done);

	workers = (flags & __WQ_ORDERED) || max_active == 1 ? 1
		: max_active <= 0 ? LX_WQ_MAX_WORKERS
		: min(max_active, LX_WQ_MAX_WORKERS);
	for (i = 0; i < workers; i++) {
		struct task_struct *task;
		wq->workers[i].wq = wq;
		task = kthread_create(worker_main, &wq->workers[i], "%s", wq->name);
		if (IS_ERR(task))
			break;
		wq->workers[i].task = task;
		if (flags & WQ_HIGHPRI)
			sched_set_fifo_low(task);
		wake_up_process(task);
	}
	wq->nr_workers = i;
	if (i == 0) {
		kfree(wq);
		return NULL;
	}
	return wq;
}


static bool
__queue_work(struct workqueue_struct *wq, struct work_struct *work)
{
	if (wq == NULL)
		wq = system_wq;
	spin_lock(&wq->lock);
	if (unlikely(wq->stopping)) {
		spin_unlock(&wq->lock);
		clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
		return false;
	}
	work->wq = wq;
	list_add_tail(&work->entry, &wq->queue);
	wq->inflight++;
	spin_unlock(&wq->lock);
	wake_up(&wq->more_work);
	return true;
}


bool
queue_work_on(int cpu, struct workqueue_struct *wq, struct work_struct *work)
{
	if (test_and_set_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return false;
	return __queue_work(wq, work);
}


void
lx_delayed_work_timer_fn(struct timer_list *t)
{
	struct delayed_work *dwork = from_timer(dwork, t, timer);
	__queue_work(dwork->wq, &dwork->work);
}


bool
queue_delayed_work_on(int cpu, struct workqueue_struct *wq,
	struct delayed_work *dwork, unsigned long delay)
{
	if (test_and_set_bit(WORK_STRUCT_PENDING_BIT,
			work_data_bits(&dwork->work)))
		return false;
	dwork->wq = wq != NULL ? wq : system_wq;
	if (delay == 0)
		return __queue_work(dwork->wq, &dwork->work);
	if (dwork->timer.function == NULL)
		timer_setup(&dwork->timer, lx_delayed_work_timer_fn, 0);
	mod_timer(&dwork->timer, jiffies + delay);
	return true;
}


/*	Takes a pending work off its queue or timer; true if it was pending.
	A timer that is firing right now is waited for: it is about to queue the
	work, which is then taken off the queue. */
static bool
try_to_grab_pending(struct work_struct *work, struct delayed_work *dwork)
{
	struct workqueue_struct *wq;

	if (!test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return false;

	if (dwork != NULL && timer_delete_sync(&dwork->timer)) {
		clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
		return true;
	}

	wq = READ_ONCE(work->wq);
	if (wq == NULL)
		return false;
	spin_lock(&wq->lock);
	if (!list_empty(&work->entry)) {
		list_del_init(&work->entry);
		wq->inflight--;
		clear_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work));
		spin_unlock(&wq->lock);
		wake_up_all(&wq->done);
		return true;
	}
	spin_unlock(&wq->lock);
	return false;
}


bool
mod_delayed_work_on(int cpu, struct workqueue_struct *wq,
	struct delayed_work *dwork, unsigned long delay)
{
	bool pending = try_to_grab_pending(&dwork->work, dwork);
	/* a racing timer may have queued it in between: then it stays queued */
	queue_delayed_work_on(cpu, wq, dwork, delay);
	return pending;
}


bool
work_busy(struct work_struct *work)
{
	struct workqueue_struct *wq = READ_ONCE(work->wq);
	int i;
	bool busy;
	if (test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work)))
		return true;
	if (wq == NULL)
		return false;
	spin_lock(&wq->lock);
	busy = false;
	for (i = 0; i < wq->nr_workers; i++) {
		if (wq->workers[i].current_work == work)
			busy = true;
	}
	spin_unlock(&wq->lock);
	return busy;
}


static bool
work_running(struct workqueue_struct *wq, struct work_struct *work)
{
	int i;
	bool running = false;
	spin_lock(&wq->lock);
	for (i = 0; i < wq->nr_workers; i++) {
		if (wq->workers[i].current_work == work)
			running = true;
	}
	spin_unlock(&wq->lock);
	return running;
}


bool
flush_work(struct work_struct *work)
{
	struct workqueue_struct *wq = READ_ONCE(work->wq);
	struct lx_worker *self = current_worker();
	bool waited = false;

	if (wq == NULL)
		return false;
	/* a work flushing itself would wait forever */
	if (self != NULL && self->current_work == work)
		return false;

	wait_event(wq->done, ({
		bool idle = !test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(work))
			&& !work_running(wq, work);
		if (!idle)
			waited = true;
		idle;
	}));
	return waited;
}


bool
flush_delayed_work(struct delayed_work *dwork)
{
	/* a timer firing right now has queued the work once this returns */
	if (timer_delete_sync(&dwork->timer))
		__queue_work(dwork->wq, &dwork->work);
	return flush_work(&dwork->work);
}


bool
cancel_work(struct work_struct *work)
{
	return try_to_grab_pending(work, NULL);
}


bool
cancel_work_sync(struct work_struct *work)
{
	bool pending = try_to_grab_pending(work, NULL);
	struct workqueue_struct *wq = READ_ONCE(work->wq);
	struct lx_worker *self = current_worker();
	if (wq != NULL && !(self != NULL && self->current_work == work))
		wait_event(wq->done, !work_running(wq, work));
	return pending;
}


bool
cancel_delayed_work(struct delayed_work *dwork)
{
	return try_to_grab_pending(&dwork->work, dwork);
}


bool
cancel_delayed_work_sync(struct delayed_work *dwork)
{
	struct work_struct *work = &dwork->work;
	struct lx_worker *self = current_worker();
	struct workqueue_struct *wq;
	bool pending = try_to_grab_pending(work, dwork);

	wq = READ_ONCE(work->wq);
	if (wq != NULL && !(self != NULL && self->current_work == work))
		wait_event(wq->done, !work_running(wq, work));
	return pending;
}


static void
rcu_work_callback(struct rcu_head *head)
{
	struct rcu_work *rwork = container_of(head, struct rcu_work, rcu);
	__queue_work(rwork->wq, &rwork->work);
}


bool
queue_rcu_work(struct workqueue_struct *wq, struct rcu_work *rwork)
{
	if (test_and_set_bit(WORK_STRUCT_PENDING_BIT,
			work_data_bits(&rwork->work)))
		return false;
	rwork->wq = wq;
	call_rcu(&rwork->rcu, rcu_work_callback);
	return true;
}


bool
flush_rcu_work(struct rcu_work *rwork)
{
	if (test_bit(WORK_STRUCT_PENDING_BIT, work_data_bits(&rwork->work)))
		rcu_barrier();
	return flush_work(&rwork->work);
}


void
flush_workqueue(struct workqueue_struct *wq)
{
	struct lx_worker *self = current_worker();
	if (wq == NULL || (self != NULL && self->wq == wq))
		return;
	wait_event(wq->done, ({
		unsigned int inflight;
		spin_lock(&wq->lock);
		inflight = wq->inflight;
		spin_unlock(&wq->lock);
		inflight == 0;
	}));
}


void
drain_workqueue(struct workqueue_struct *wq)
{
	flush_workqueue(wq);
}


void
destroy_workqueue(struct workqueue_struct *wq)
{
	int i;
	if (wq == NULL)
		return;
	flush_workqueue(wq);
	spin_lock(&wq->lock);
	wq->stopping = true;
	spin_unlock(&wq->lock);
	wake_up_all(&wq->more_work);
	for (i = 0; i < wq->nr_workers; i++)
		kthread_stop(wq->workers[i].task);
	kfree(wq);
}


struct work_struct *
current_work(void)
{
	struct lx_worker *self = current_worker();
	return self != NULL ? self->current_work : NULL;
}


void
lx_irq_work_fn(struct work_struct *work)
{
	struct irq_work *irq_work = container_of(work, struct irq_work, work);
	irq_work->func(irq_work);
}


/* #pragma mark - tasklets */

static struct workqueue_struct *sTaskletQueue;


static void
tasklet_work(struct work_struct *work)
{
	struct tasklet_struct *t = container_of(work, struct tasklet_struct,
		lx_work);

	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;
	if (tasklet_trylock(t)) {
		if (atomic_read(&t->count) == 0) {
			if (test_and_clear_bit(TASKLET_STATE_SCHED, &t->state)) {
				if (t->use_callback)
					t->callback(t);
				else
					t->func(t->data);
			}
			tasklet_unlock(t);
			return;
		}
		tasklet_unlock(t);
		/* disabled: tasklet_enable() queues it again */
		return;
	}
	/* running on another CPU (or called directly): try again */
	queue_work(sTaskletQueue, &t->lx_work);
}


void
tasklet_setup(struct tasklet_struct *t,
	void (*callback)(struct tasklet_struct *))
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->callback = callback;
	t->use_callback = true;
	t->data = 0;
	INIT_WORK(&t->lx_work, tasklet_work);
}


void
tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
	unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->use_callback = false;
	t->data = data;
	INIT_WORK(&t->lx_work, tasklet_work);
}


void
tasklet_schedule(struct tasklet_struct *t)
{
	if (!test_and_set_bit(TASKLET_STATE_SCHED, &t->state))
		queue_work(sTaskletQueue, &t->lx_work);
}


void
tasklet_enable(struct tasklet_struct *t)
{
	smp_mb__before_atomic();
	if (atomic_dec_and_test(&t->count)
		&& test_bit(TASKLET_STATE_SCHED, &t->state))
		queue_work(sTaskletQueue, &t->lx_work);
}


void
tasklet_unlock_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &t->state))
		cpu_relax();
}


void
tasklet_unlock_spin_wait(struct tasklet_struct *t)
{
	while (test_bit(TASKLET_STATE_RUN, &t->state))
		cpu_relax();
}


void
tasklet_kill(struct tasklet_struct *t)
{
	/* let a scheduled run finish (unless disabled: then it is dropped) */
	while (test_bit(TASKLET_STATE_SCHED, &t->state)
		&& atomic_read(&t->count) == 0)
		flush_work(&t->lx_work);
	set_bit(TASKLET_STATE_SCHED, &t->state);
	cancel_work_sync(&t->lx_work);
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}


int lx_workqueue_init(void);

int
lx_workqueue_init(void)
{
	sTaskletQueue = alloc_workqueue("linux tasklets", WQ_HIGHPRI, 2);
	system_wq = alloc_workqueue("linux events", 0, 4);
	system_highpri_wq = alloc_workqueue("linux events high", WQ_HIGHPRI, 2);
	system_long_wq = alloc_workqueue("linux events long", 0, 2);
	system_unbound_wq = alloc_workqueue("linux events unbound", WQ_UNBOUND, 4);
	system_freezable_wq = system_wq;
	system_power_efficient_wq = system_wq;
	system_freezable_power_efficient_wq = system_wq;
	if (system_wq == NULL || system_highpri_wq == NULL
		|| system_long_wq == NULL || system_unbound_wq == NULL
		|| sTaskletQueue == NULL)
		return -ENOMEM;
	return 0;
}


void lx_workqueue_exit(void);

void
lx_workqueue_exit(void)
{
	destroy_workqueue(sTaskletQueue);
	destroy_workqueue(system_unbound_wq);
	destroy_workqueue(system_long_wq);
	destroy_workqueue(system_highpri_wq);
	destroy_workqueue(system_wq);
}
