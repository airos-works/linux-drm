/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_ALL_H
#define _LX_ALL_H

/*	The Linux kernel API on Haiku, as the DRM core, TTM, the scheduler,
	dma-buf and amdgpu use it.

	Every Linux source file gets all of it through the forced include
	(lx/kconfig.h), and every Linux header the compat layer provides is a
	stub including this file; so include order and transitive includes in the
	Linux sources do not matter. Plain Linux headers (lists, rbtrees, bit and
	overflow helpers ...) are used unchanged from linux/include.

	The Linux side never sees a Haiku header: the parts that need Haiku's
	kernel are lx_*() functions implemented in C++ (haiku/compat/haiku), and
	Haiku objects embedded in Linux structures are opaque storage whose size
	the C++ side checks. Errors are Linux's: negative E* values with Linux's
	numbers. */

#include <lx/base.h>
#include <lx/atomic.h>
#include <lx/lock.h>
#include <lx/time.h>
#include <lx/sched.h>
#include <lx/mm.h>
#include <lx/io.h>
#include <lx/device.h>
#include <lx/pci.h>
#include <lx/fs.h>
#include <lx/misc.h>
#include <lx/accel.h>

#endif /* _LX_ALL_H */
