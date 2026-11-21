/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Bring-up aid: a call trace. The files a build lists in TRACE_CALLS are
	compiled with -finstrument-functions; while tracing is on, every call
	into one of their functions is logged (log_file keeps it across a crash)
	as "T<thread> <offset>", the function's distance from
	__cyg_profile_func_enter. haiku/tools/symbolize-trace.sh turns that back
	into names. Tracing starts at the first close of a DRM file when the
	trace_calls parameter is 1, when the driver stops (devfs unloads it)
	when it is 2, and stays on. With 3 only threads that are closing a DRM
	file are traced (lx_trace_close_begin() to lx_trace_close_end()), and
	the close's steps are logged. With 4 tracing is on from the start
	(bring-up: where a first start stops; trace only a few files). */

#include <lx/haiku.h>


static int trace_calls;
module_param(trace_calls, int, 0644);

static int trace_active;


void lx_trace_calls_start(void);
void lx_trace_calls_start_stop(void);
void lx_trace_close_begin(void);
void lx_trace_close_step(const char *step);
void lx_trace_close_end(void);

#define TRACE_CLOSERS 16
static int trace_closers[TRACE_CLOSERS];


static bool __attribute__((no_instrument_function))
is_closer(int thread)
{
	int i;
	for (i = 0; i < TRACE_CLOSERS; i++) {
		if (READ_ONCE(trace_closers[i]) == thread)
			return true;
	}
	return false;
}


void
lx_trace_close_begin(void)
{
	int thread = lx_haiku_current_thread();
	int i;
	if (READ_ONCE(trace_calls) != 3)
		return;
	printk("trace: close T%d begins\n", thread);
	for (i = 0; i < TRACE_CLOSERS; i++) {
		if (cmpxchg(&trace_closers[i], 0, thread) == 0)
			return;
	}
}


void
lx_trace_close_step(const char *step)
{
	if (READ_ONCE(trace_calls) == 3)
		printk("trace: close T%d %s\n", lx_haiku_current_thread(), step);
}


void
lx_trace_close_end(void)
{
	int thread = lx_haiku_current_thread();
	int i;
	if (READ_ONCE(trace_calls) != 3)
		return;
	for (i = 0; i < TRACE_CLOSERS; i++) {
		if (READ_ONCE(trace_closers[i]) == thread)
			WRITE_ONCE(trace_closers[i], 0);
	}
	printk("trace: close T%d done\n", thread);
}

void
lx_trace_calls_start(void)
{
	if (READ_ONCE(trace_calls) == 1 && !READ_ONCE(trace_active)) {
		printk("trace: calls from here on\n");
		WRITE_ONCE(trace_active, 1);
	}
}


void
lx_trace_calls_start_stop(void)
{
	if (READ_ONCE(trace_calls) == 2 && !READ_ONCE(trace_active)) {
		printk("trace: calls of the stop from here on\n");
		WRITE_ONCE(trace_active, 1);
	}
}


void __cyg_profile_func_enter(void *function, void *site)
	__attribute__((no_instrument_function));
void __cyg_profile_func_exit(void *function, void *site)
	__attribute__((no_instrument_function));


void
__cyg_profile_func_enter(void *function, void *site)
{
	if (unlikely(READ_ONCE(trace_calls) == 3)) {
		int thread = lx_haiku_current_thread();
		if (is_closer(thread))
			printk("T%d %lx\n", thread, (unsigned long)function
				- (unsigned long)__cyg_profile_func_enter);
		return;
	}
	if (likely(!READ_ONCE(trace_active)) && READ_ONCE(trace_calls) != 4)
		return;
	printk("T%d %lx\n", lx_haiku_current_thread(),
		(unsigned long)function - (unsigned long)__cyg_profile_func_enter);
}


void
__cyg_profile_func_exit(void *function, void *site)
{
}
