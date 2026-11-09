/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Interrupt handlers. Haiku's interrupt handler only wakes a thread; the
	Linux handler, and its threaded part, run on that thread. Linux code can
	therefore block in its interrupt handler; the GPU's interrupts are MSIs,
	so nothing has to be masked in the meantime. */

#include <lx/haiku.h>


struct lx_irq {
	struct list_head entry;
	unsigned int irq;
	irq_handler_t handler;
	irq_handler_t thread_fn;
	void *dev_id;
	const char *name;
	void *haiku_hook;
	void *event;
	struct task_struct *thread;
	int disable_depth;
	bool running;
	bool stop;
	struct wait_queue_head idle;
	/* for lx_irq_stats(): interrupts that came, handler runs, and when the
	 * thread last started one */
	unsigned long arrived;
	unsigned long handled;
	u64 handler_started;
};

static LIST_HEAD(lx_irqs);
static DEFINE_MUTEX(lx_irqs_lock);


static void
lx_irq_top_half(void *data)
{
	struct lx_irq *desc = data;
	WRITE_ONCE(desc->arrived, desc->arrived + 1);
	lx_haiku_event_signal(desc->event);
}


static int
lx_irq_thread(void *data)
{
	struct lx_irq *desc = data;
	for (;;) {
		irqreturn_t result;
		lx_haiku_event_wait(desc->event, -1);
		if (READ_ONCE(desc->stop))
			break;
		WRITE_ONCE(desc->running, true);
		WRITE_ONCE(desc->handler_started, ktime_get_ns());
		WRITE_ONCE(desc->handled, desc->handled + 1);
		if (READ_ONCE(desc->disable_depth) > 0) {
			WRITE_ONCE(desc->running, false);
			wake_up_all(&desc->idle);
			continue;
		}
		result = desc->handler != NULL
			? desc->handler(desc->irq, desc->dev_id) : IRQ_WAKE_THREAD;
		if (result == IRQ_WAKE_THREAD && desc->thread_fn != NULL)
			desc->thread_fn(desc->irq, desc->dev_id);
		WRITE_ONCE(desc->running, false);
		wake_up_all(&desc->idle);
	}
	return 0;
}


int
request_threaded_irq(unsigned int irq, irq_handler_t handler,
	irq_handler_t thread_fn, unsigned long flags, const char *name, void *dev)
{
	struct lx_irq *desc = kzalloc(sizeof(*desc), GFP_KERNEL);
	if (desc == NULL)
		return -ENOMEM;
	desc->irq = irq;
	desc->handler = handler;
	desc->thread_fn = thread_fn;
	desc->dev_id = dev;
	desc->name = name;
	init_waitqueue_head(&desc->idle);
	desc->event = lx_haiku_event_create();
	if (desc->event == NULL) {
		kfree(desc);
		return -ENOMEM;
	}
	desc->thread = kthread_create(lx_irq_thread, desc, "irq %u %s", irq,
		name != NULL ? name : "");
	if (IS_ERR(desc->thread)) {
		lx_haiku_event_delete(desc->event);
		kfree(desc);
		return -ENOMEM;
	}
	lx_haiku_set_priority(desc->thread->pid, LX_HAIKU_PRIORITY_REALTIME);
	wake_up_process(desc->thread);

	desc->haiku_hook = lx_haiku_install_interrupt(irq, lx_irq_top_half, desc);
	if (desc->haiku_hook == NULL) {
		WRITE_ONCE(desc->stop, true);
		lx_haiku_event_signal(desc->event);
		kthread_stop(desc->thread);
		lx_haiku_event_delete(desc->event);
		kfree(desc);
		return -EBUSY;
	}

	mutex_lock(&lx_irqs_lock);
	list_add_tail(&desc->entry, &lx_irqs);
	mutex_unlock(&lx_irqs_lock);
	return 0;
}


/*	For a watchdog: of the first interrupt handler of \a dev_id, how many
	interrupts came and how many handler runs started, for how long the
	running one has been running (0 when none is), and the Haiku thread the
	handler runs on. */
void
lx_irq_stats(void *dev_id, unsigned long *arrived, unsigned long *handled,
	u64 *running_ns, long *thread)
{
	struct lx_irq *desc;
	*arrived = *handled = 0;
	*running_ns = 0;
	*thread = 0;
	mutex_lock(&lx_irqs_lock);
	list_for_each_entry(desc, &lx_irqs, entry) {
		if (desc->dev_id != dev_id)
			continue;
		*arrived = READ_ONCE(desc->arrived);
		*handled = READ_ONCE(desc->handled);
		if (READ_ONCE(desc->running))
			*running_ns = ktime_get_ns() - READ_ONCE(desc->handler_started);
		*thread = desc->thread->pid;
		break;
	}
	mutex_unlock(&lx_irqs_lock);
}


/* interrupt handlers installed now: one that is never removed shows as a
 * count that grows */
int lx_irq_count(void);

int
lx_irq_count(void)
{
	struct lx_irq *desc;
	int count = 0;
	mutex_lock(&lx_irqs_lock);
	list_for_each_entry(desc, &lx_irqs, entry)
		count++;
	mutex_unlock(&lx_irqs_lock);
	return count;
}


int
request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
	const char *name, void *dev)
{
	return request_threaded_irq(irq, handler, NULL, flags, name, dev);
}


static struct lx_irq *
find_irq(unsigned int irq, void *dev_id)
{
	struct lx_irq *desc, *found = NULL;
	mutex_lock(&lx_irqs_lock);
	list_for_each_entry(desc, &lx_irqs, entry) {
		if (desc->irq == irq && (dev_id == NULL || desc->dev_id == dev_id)) {
			found = desc;
			break;
		}
	}
	mutex_unlock(&lx_irqs_lock);
	return found;
}


const void *
free_irq(unsigned int irq, void *dev_id)
{
	struct lx_irq *desc = find_irq(irq, dev_id);
	const char *name;
	if (desc == NULL)
		return NULL;
	mutex_lock(&lx_irqs_lock);
	list_del(&desc->entry);
	mutex_unlock(&lx_irqs_lock);

	lx_haiku_remove_interrupt(desc->haiku_hook);
	WRITE_ONCE(desc->stop, true);
	lx_haiku_event_signal(desc->event);
	kthread_stop(desc->thread);
	lx_haiku_event_delete(desc->event);
	name = desc->name;
	kfree(desc);
	return name;
}


void
synchronize_irq(unsigned int irq)
{
	struct lx_irq *desc = find_irq(irq, NULL);
	if (desc == NULL || current == desc->thread)
		return;
	wait_event(desc->idle, !READ_ONCE(desc->running));
}


void
disable_irq_nosync(unsigned int irq)
{
	struct lx_irq *desc = find_irq(irq, NULL);
	if (desc != NULL)
		__atomic_add_fetch(&desc->disable_depth, 1, __ATOMIC_SEQ_CST);
}


void
disable_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	synchronize_irq(irq);
}


void
enable_irq(unsigned int irq)
{
	struct lx_irq *desc = find_irq(irq, NULL);
	if (desc == NULL)
		return;
	if (__atomic_sub_fetch(&desc->disable_depth, 1, __ATOMIC_SEQ_CST) == 0)
		lx_haiku_event_signal(desc->event);	/* catch up on lost ones */
}


struct irq_domain {
	const struct irq_domain_ops *ops;
	void *host_data;
	unsigned int size;
};


struct irq_domain *
lx_irq_domain_create(unsigned int size, const struct irq_domain_ops *ops,
	void *host_data)
{
	struct irq_domain *domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (domain == NULL)
		return NULL;
	domain->ops = ops;
	domain->host_data = host_data;
	domain->size = size;
	return domain;
}
