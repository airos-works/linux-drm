/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDXDNA_GLUE_H
#define AMDXDNA_GLUE_H

/*	Between the Haiku driver (driver.cpp, Haiku headers) and the Linux side
	(glue.c, Linux headers): plain C types only. Results are 0 or negative
	Linux error numbers. */

#ifdef __cplusplus
extern "C" {
#endif

int lx_glue_set_param(const char *name, const char *value);

/* when the driver is loaded and unloaded: the memory for the NPU's buffers
   (unless the settings leave that to the first start) */
int lx_glue_reserve(void);
void lx_glue_unreserve(void);

/* the Linux side without a device: nothing here touches the hardware */
int lx_glue_init(void);
void lx_glue_uninit(void);

/* the NPU: start reserves the buffer memory, loads the firmware and powers
   the NPU up; suspend powers it down (no client left, or the system
   sleeps), resume up again; stop gives everything back */
int lx_glue_start(int bus, int device, int function);
int lx_glue_suspend(void);
int lx_glue_resume(void);
void lx_glue_stop(void);

/* how long the NPU stays powered after the last close (ms), and whether it
   is powered down at all (settings: autosuspend_ms, keep_powered) */
unsigned int lx_glue_autosuspend_ms(void);
int lx_glue_keep_powered(void);
/* the last client has gone: now and then, a line of counts for the log */
void lx_glue_note_idle(void);
/* lab aids: a device without hardware (buffers and mappings only), and a
   PCI function of a virtual machine with a thread that plays the NPU's
   firmware, SMU and PSP, started, suspended and resumed as the NPU is */
int lx_glue_selftest(void);
int lx_glue_fake_start(void);
int lx_glue_emulated_start(void);

void *lx_glue_open(int flags, int *error);
void lx_glue_close(void *file);
long lx_glue_ioctl(void *file, unsigned int cmd, void *arg);

/* what DRM_HAIKU_IOCTL_DEVICE_INFO reports */
int lx_glue_location(int *bus, int *device, int *function);

void *lx_glue_map(void *file, unsigned long long offset, unsigned long size,
	int writable, unsigned long address);
long lx_glue_map_fault(void *mapping, unsigned long first, unsigned long *pfns,
	unsigned long max, int *caching);
void lx_glue_unmap(void *mapping);
int lx_glue_is_error(const void *pointer);

#ifdef __cplusplus
}
#endif

#endif /* AMDXDNA_GLUE_H */
