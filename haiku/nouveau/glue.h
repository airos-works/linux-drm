/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDGPU_GLUE_H
#define AMDGPU_GLUE_H

/*	Between the Haiku driver (driver.cpp, Haiku headers) and the Linux side
	(glue.c, Linux headers): plain C types only. Results are 0 or negative
	Linux error numbers. */

#ifdef __cplusplus
extern "C" {
#endif

int lx_glue_set_param(const char *name, const char *value);
int lx_glue_init(void);
int lx_glue_probe(int bus, int device, int function);
void lx_glue_uninit(void);
void lx_glue_shutdown(void);
int lx_glue_suspend(void);
void lx_glue_resume(void);

int lx_glue_minors(int *minors, int max);
int lx_glue_minor_location(int minor, int *bus, int *device, int *function);

void *lx_glue_open(int minor, int flags, int *error);
void lx_glue_close(void *file);
long lx_glue_ioctl(void *file, unsigned int cmd, void *arg);
long lx_glue_read(void *file, void *buffer, unsigned long length);
int lx_glue_force_connector(void *file, unsigned int connector_id, int force);
int lx_glue_backlight(void *file, int brightness, int *_level, int *_maximum);
int lx_glue_crc(void *file, unsigned crtc_index, int op, unsigned *crc,
	unsigned *_frame, const unsigned short *window);

/* DRM_HAIKU_IOCTL_SCANOUT, as struct drm_haiku_scanout */
struct lx_glue_scanout_args {
	unsigned int op;
	unsigned int handle;
	unsigned int width;
	unsigned int height;
	unsigned int pitch;
	unsigned int format;
	unsigned int generation;
	unsigned int reserved;
	unsigned long long size;
};
int lx_glue_scanout(void *file, struct lx_glue_scanout_args *args);
void *lx_glue_select(void *file, void (*notify)(void *cookie, unsigned int events),
	void *cookie, unsigned int *ready);
void lx_glue_deselect(void *select);

void *lx_glue_map(void *file, unsigned long long offset, unsigned long size,
	int writable, void (*invalidate)(void *owner, unsigned long first,
		unsigned long count), void *owner);
long lx_glue_map_fault(void *mapping, unsigned long first, unsigned long *pfns,
	unsigned long max, int *caching);
unsigned long lx_glue_map_prefault(void *mapping, unsigned long count);
void lx_glue_map_detach(void *mapping);
void lx_glue_unmap(void *mapping);
int lx_glue_is_error(const void *pointer);
int lx_glue_selftest(void);
int lx_glue_fake_device_start(void);
int lx_glue_lab_bench(void *file);
int lx_glue_wait_hotplug(unsigned int *generation, unsigned int timeout_ms);
int lx_glue_lab_perf_level(void *file, unsigned int level);

#ifdef __cplusplus
}
#endif

#endif /* AMDGPU_GLUE_H */
