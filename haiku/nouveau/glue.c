/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Linux side of the Haiku driver: starting the DRM core and nouveau,
	probing the GPUs, and the operations on DRM files the device hooks of
	driver.cpp forward. Everything here is called with plain C types
	(glue.h). */

#include <lx/haiku.h>
#include <lx/vma.h>

#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_haiku.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_vma_manager.h>
#include <drm/drm_connector.h>
#include <drm/drm_mode_object.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_vblank.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_plane.h>
#include <drm/gpu_scheduler.h>
#include <linux/dma-resv.h>
#include <linux/interval_tree_generic.h>
#include <drm/ttm/ttm_resource.h>

#include "nouveau_drv.h"
#include "nouveau_bo.h"
#include "nouveau_gem.h"
#include "userptr.h"

#include "glue.h"

extern void (*lx_drm_hotplug_hook)(struct drm_device *dev);
static void hotplug_event(struct drm_device *dev);
extern void (*lx_unmap_mapping_range_hook)(struct address_space *mapping,
	loff_t start, loff_t length);
static void lx_glue_unmap_mapping_range(struct address_space *as,
	loff_t start, loff_t length);

#ifndef DRM_MAJOR
#	define DRM_MAJOR 226
#endif


/* module initialization functions (lx/device.h's module_init()) */
int lx_module_init_dma_buf_init(void);
int lx_module_init_drm_core_init(void);
int lx_module_init_gpu_buddy_module_init(void);
int lx_module_init_drm_display_helper_module_init(void);
int lx_module_init_drm_sched_fence_slab_init(void);
int lx_module_init_nouveau_drm_init(void);
void lx_module_exit_nouveau_drm_exit(void);
void lx_trace_calls_start(void);
void lx_module_exit_drm_sched_fence_slab_fini(void);
void lx_module_exit_drm_display_helper_module_exit(void);
void lx_module_exit_gpu_buddy_module_exit(void);
void lx_module_exit_drm_core_exit(void);

int lx_workqueue_init(void);
void lx_cpu_init(void);
void lx_workqueue_exit(void);
void lx_rcu_exit(void);
void lx_timer_exit(void);
struct pci_dev *lx_pci_dev_get(int bus, int device, int function);
struct pci_driver *lx_pci_registered_driver(void);
const struct pci_device_id *lx_pci_match(struct pci_driver *driver,
	struct pci_dev *dev);

extern const struct lx_param __start_lx_params[];
extern const struct lx_param __stop_lx_params[];

#define MAX_GPUS 8
static struct pci_dev *probed[MAX_GPUS];
static int probed_count;
static struct drm_device *fake_drm;


/* nouveau's PCI driver data is its struct nouveau_drm (amdgpu's is the DRM
 * device itself) */
static struct drm_device *
drm_device_of(struct pci_dev *pdev)
{
	struct nouveau_drm *drm = pci_get_drvdata(pdev);
	return drm != NULL ? drm->dev : NULL;
}


/* the DRM devices: the probed GPUs, and the self test's fake one */
static int
drm_devices(struct drm_device **devices, int max)
{
	int count = 0, i;
	for (i = 0; i < probed_count && count < max; i++) {
		struct drm_device *dev = drm_device_of(probed[i]);
		if (dev != NULL)
			devices[count++] = dev;
	}
	if (fake_drm != NULL && count < max)
		devices[count++] = fake_drm;
	return count;
}


/* #pragma mark - parameters */


static int haiku_console_test;
static int haiku_lab_shutdown;
/* "drmparam haiku_mm_report 1": the watchdog's memory line, once (kmalloc,
   pages, the page pool) */
static int haiku_mm_report;
module_param(haiku_mm_report, int, 0644);
static void lx_glue_console_test(void);
static void lx_glue_watchdog_start(void);
static void lx_glue_fence_report(void);
static int haiku_fence_report;
static struct task_struct *watchdog_thread;
static void lx_glue_console_moved(void);
/* the kernel debugger's console, see lx_glue_update_debug_console() */
static void *console_base;
static int console_width, console_height, console_pitch;
static struct drm_device *console_device;
/* the buffer the console is in (a reference), and how near its screen is
   to the desktop's top left */
static struct nouveau_bo *console_bo;
static int console_distance = INT_MAX;

int
lx_glue_set_param(const char *name, const char *value)
{
	const struct lx_param *param;
	for (param = __start_lx_params; param < __stop_lx_params; param++) {
		int error = -EINVAL;
		if (strcmp(param->name, name) != 0)
			continue;
		if (!strcmp(param->type, "int"))
			error = kstrtoint(value, 0, param->value);
		else if (!strcmp(param->type, "uint") || !strcmp(param->type, "hexint"))
			error = kstrtouint(value, 0, param->value);
		else if (!strcmp(param->type, "long"))
			error = kstrtol(value, 0, param->value);
		else if (!strcmp(param->type, "ulong"))
			error = kstrtoul(value, 0, param->value);
		else if (!strcmp(param->type, "ullong"))
			error = kstrtoull(value, 0, param->value);
		else if (!strcmp(param->type, "bool"))
			error = kstrtobool(value, param->value);
		else if (!strcmp(param->type, "charp")) {
			char *copy = kstrdup(value, GFP_KERNEL);
			if (copy != NULL) {
				*(char **)param->value = copy;
				error = 0;
			}
		}
		pr_info("nouveau: parameter %s = %s%s\n", name, value,
			error != 0 ? " (not accepted)" : "");
		if (error == 0 && strcmp(name, "haiku_console_test") == 0
			&& haiku_console_test)
			lx_glue_console_test();
		if (error == 0 && strcmp(name, "haiku_console_connector") == 0)
			lx_glue_console_moved();
		if (error == 0 && strcmp(name, "haiku_watchdog_ms") == 0)
			lx_glue_watchdog_start();
		if (error == 0 && strcmp(name, "haiku_fence_report") == 0
			&& haiku_fence_report)
			lx_glue_fence_report();
		if (error == 0 && strcmp(name, "haiku_lab_shutdown") == 0
			&& haiku_lab_shutdown)
			lx_glue_shutdown();
		if (error == 0 && strcmp(name, "haiku_mm_report") == 0
			&& haiku_mm_report) {
			char line[400];
			lx_mm_stats(line, sizeof(line));
			pr_info("nouveau: memory: %s\n", line);
		}
		return error;
	}
	pr_info("nouveau: no parameter %s\n", name);
	return -ENOENT;
}


/* #pragma mark - start and stop */


int
lx_glue_init(void)
{
	int error;

	/* the first message: opens log_file, if the settings name one */
	pr_info("nouveau_drm: starting\n");
	lx_cpu_init();
	lx_drm_hotplug_hook = hotplug_event;
	lx_unmap_mapping_range_hook = lx_glue_unmap_mapping_range;
	error = lx_workqueue_init();
	if (error != 0)
		return error;
	/* without it, pages are freed to Haiku at once */
	if (lx_page_pool_init() != 0)
		pr_warn("nouveau_drm: no page pool\n");

	/* dma-buf needs a pseudo file system Haiku does not have: exporting
	   buffers fails, the rest works */
	lx_module_init_dma_buf_init();

	error = lx_module_init_drm_core_init();
	if (error != 0)
		goto err_workqueues;
	error = lx_module_init_gpu_buddy_module_init();
	if (error != 0)
		goto err_drm;
	error = lx_module_init_drm_display_helper_module_init();
	if (error != 0)
		goto err_buddy;
	error = lx_module_init_drm_sched_fence_slab_init();
	if (error != 0)
		goto err_display;
	error = lx_module_init_nouveau_drm_init();
	if (error != 0)
		goto err_sched;
	pr_info("nouveau_drm: starting: modules initialized\n");
	return 0;

err_sched:
	lx_module_exit_drm_sched_fence_slab_fini();
err_display:
	lx_module_exit_drm_display_helper_module_exit();
err_buddy:
	lx_module_exit_gpu_buddy_module_exit();
err_drm:
	lx_module_exit_drm_core_exit();
err_workqueues:
	lx_page_pool_exit();
	lx_workqueue_exit();
	return error;
}


int
lx_glue_probe(int bus, int device, int function)
{
	struct pci_driver *driver = lx_pci_registered_driver();
	const struct pci_device_id *id;
	struct pci_dev *pdev;
	int error;

	if (driver == NULL || probed_count >= MAX_GPUS)
		return -ENODEV;
	pdev = lx_pci_dev_get(bus, device, function);
	if (pdev == NULL)
		return -ENODEV;
	id = lx_pci_match(driver, pdev);
	if (id == NULL)
		return -ENODEV;

	pr_info("nouveau: probing %s (%04x:%04x rev %02x)\n", pci_name(pdev),
		pdev->vendor, pdev->device, pdev->revision);
	pdev->driver = driver;
	error = driver->probe(pdev, id);
	if (error != 0) {
		pr_err("nouveau: probing %s failed: %d\n", pci_name(pdev), error);
		/* as Linux's driver core: what the probe left managed goes */
		devres_release_all(&pdev->dev);
		pdev->driver = NULL;
		return error;
	}
	probed[probed_count++] = pdev;
	return 0;
}


/* lab aid: "haiku_lab_shutdown 1" (drmparam) does what powering off does
 * to the GPUs, for trying a warm reboot without the kernel's hook */
module_param(haiku_lab_shutdown, int, 0644);

/* What Linux does for each GPU when the machine is powered off or rebooted
 * (amdgpu_pci_shutdown: every IP block suspended, the SMU told to unload).
 * An APU's GPU is not reset by a warm reboot; started again from the state
 * it was left in, it can hang the next boot's start. */
void
lx_glue_shutdown(void)
{
	int i;
	for (i = 0; i < probed_count; i++) {
		struct pci_dev *pdev = probed[i];
		if (pdev->driver != NULL && pdev->driver->shutdown != NULL) {
			pr_info("nouveau: shutting down %s\n", pci_name(pdev));
			pdev->driver->shutdown(pdev);
		}
	}
}



/* Sleeping: the GPUs stay as they are (discrete GPUs keep their power). */
int
lx_glue_suspend(void)
{
	return 0;
}


void
lx_glue_resume(void)
{
}


void lx_glue_fake_device_stop(void);

void
lx_glue_uninit(void)
{
	/* each step logged: release builds only stop when devfs reloads the
	 * driver (its binary changed), the one time this path runs */
	pr_info("nouveau_drm: stopping\n");
	if (watchdog_thread != NULL) {
		kthread_stop(watchdog_thread);
		watchdog_thread = NULL;
	}
	/* the console's frame buffer goes away */
	if (console_base != NULL) {
		lx_haiku_debug_console(NULL, 0, 0, 0);
		console_base = NULL;
	}
	if (console_bo != NULL) {
		drm_gem_object_put(&console_bo->bo.base);
		console_bo = NULL;
	}
	console_device = NULL;
	console_distance = INT_MAX;
	lx_glue_fake_device_stop();
	while (probed_count > 0) {
		struct pci_dev *pdev = probed[--probed_count];
		pr_info("nouveau_drm: stopping: removing %s\n", pci_name(pdev));
		if (pdev->driver != NULL && pdev->driver->remove != NULL)
			pdev->driver->remove(pdev);
		pr_info("nouveau_drm: stopping: removed, releasing its resources\n");
		devres_release_all(&pdev->dev);
		pdev->driver = NULL;
		pr_info("nouveau_drm: stopping: released\n");
	}
	pr_info("nouveau_drm: stopping: module exits\n");
	lx_module_exit_nouveau_drm_exit();
	lx_module_exit_drm_sched_fence_slab_fini();
	lx_module_exit_drm_display_helper_module_exit();
	lx_module_exit_gpu_buddy_module_exit();
	lx_module_exit_drm_core_exit();
	pr_info("nouveau_drm: stopping: page pool, workqueues, RCU, timers\n");
	lx_page_pool_exit();
	lx_workqueue_exit();
	lx_rcu_exit();
	lx_timer_exit();
	pr_info("nouveau_drm: stopped\n");
	/* no code of this image may run after it is unloaded */
	lx_haiku_set_task_exit_hook(NULL);
	lx_haiku_log_file(NULL);
	lx_unmap_mapping_range_hook = NULL;
}


/* #pragma mark - devices */


/* the DRM minors to publish: "dri/card0", "dri/renderD128", ... */
int
lx_glue_minors(int *minors, int max)
{
	struct drm_device *devices[MAX_GPUS + 1];
	int deviceCount = drm_devices(devices, MAX_GPUS + 1);
	int count = 0, i;
	for (i = 0; i < deviceCount; i++) {
		struct drm_device *dev = devices[i];
		if (dev->primary != NULL && count < max)
			minors[count++] = dev->primary->index;
		if (dev->render != NULL && count < max)
			minors[count++] = dev->render->index;
	}
	return count;
}


/* the PCI location of the GPU a minor belongs to */
int
lx_glue_minor_location(int minor, int *bus, int *device, int *function)
{
	int i;
	for (i = 0; i < probed_count; i++) {
		struct drm_device *dev = drm_device_of(probed[i]);
		if (dev == NULL)
			continue;
		if ((dev->primary != NULL && dev->primary->index == minor)
			|| (dev->render != NULL && dev->render->index == minor)) {
			*bus = probed[i]->bus->number;
			*device = PCI_SLOT(probed[i]->devfn);
			*function = PCI_FUNC(probed[i]->devfn);
			return 0;
		}
	}
	return -ENODEV;
}


static struct drm_device *
drm_device_of_minor(int minor)
{
	struct drm_device *devices[MAX_GPUS + 1];
	int count = drm_devices(devices, MAX_GPUS + 1), i;
	for (i = 0; i < count; i++) {
		struct drm_device *dev = devices[i];
		if ((dev->primary != NULL && dev->primary->index == minor)
			|| (dev->render != NULL && dev->render->index == minor))
			return dev;
	}
	return NULL;
}


void *
lx_glue_open(int minor, int flags, int *_error)
{
	struct drm_device *dev = drm_device_of_minor(minor);
	struct file *filp;
	struct inode *inode;
	int error;

	if (dev == NULL) {
		*_error = -ENODEV;
		return NULL;
	}

	filp = kzalloc(sizeof(*filp), GFP_KERNEL);
	inode = kzalloc(sizeof(*inode), GFP_KERNEL);
	if (filp == NULL || inode == NULL) {
		kfree(filp);
		kfree(inode);
		*_error = -ENOMEM;
		return NULL;
	}
	inode->i_rdev = MKDEV(DRM_MAJOR, minor);
	inode->i_mapping = &inode->i_data;
	atomic_set(&inode->i_count, 1);
	atomic_long_set(&filp->f_count, 1);
	spin_lock_init(&filp->f_lock);
	mutex_init(&filp->f_pos_lock);
	filp->f_flags = flags;
	filp->f_mode = FMODE_READ | FMODE_WRITE;
	filp->f_inode = inode;
	filp->f_op = dev->driver->fops;
	filp->lx_owner = current;

	error = filp->f_op->open(inode, filp);
	if (error != 0) {
		kfree(filp);
		kfree(inode);
		*_error = error;
		return NULL;
	}
	*_error = 0;
	return filp;
}


static void lx_scanout_file_closing(struct drm_file *file);

void
lx_glue_close(void *file)
{
	struct file *filp = file;
	lx_trace_calls_start();
	if (atomic_long_read(&filp->f_count) == 1)
		lx_scanout_file_closing(filp->private_data);
	fput(file);
}


/* #pragma mark - watchdog */

/* amdgpu's hang watchdog and fence report are not ported */
static int haiku_watchdog_ms;
module_param(haiku_watchdog_ms, int, 0644);
static int haiku_fence_report;
module_param(haiku_fence_report, int, 0644);


static void
lx_glue_fence_report(void)
{
}


static void
lx_glue_watchdog_report(long irq_thread, u64 running_ms)
{
}


static void
lx_glue_watchdog_start(void)
{
}


/* #pragma mark - the kernel debugger's console */

/* Haiku draws the kernel debugger into the frame buffer the boot loader
 * found, which goes when nouveau takes its GPU over (compat's
 * aperture_remove_conflicting_pci_devices()). So after every mode set, the
 * frame buffer the screen at the desktop's top left shows becomes the
 * console, through nouveau's kernel mapping of it (BAR1, write-combined); the
 * console keeps a reference to that buffer. With two GPUs, the one whose
 * screen is nearest the top left wins.
 * "haiku_console_connector DP-1" puts it on that connector's screen. */
static char *haiku_console_connector;
module_param(haiku_console_connector, charp, 0644);
/* lab aid: "haiku_console_test 1" paints a mark where the console is */
static int haiku_console_test;
module_param(haiku_console_test, int, 0644);


static void
lx_glue_update_debug_console(struct drm_device *dev)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_plane *plane;
	struct nouveau_bo *best_bo = NULL;
	u64 best_offset = 0;
	int width = 0, height = 0, pitch = 0, best = INT_MAX;
	int ret;

	drm_modeset_acquire_init(&ctx, 0);
retry:
	ret = drm_modeset_lock_all_ctx(dev, &ctx);
	if (ret == -EDEADLK) {
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	if (ret == 0) {
		drm_for_each_plane(plane, dev) {
			struct drm_plane_state *state = plane->state;
			struct drm_framebuffer *fb;
			struct nouveau_bo *nvbo;
			int x, y, w, h, distance;

			if (plane->type != DRM_PLANE_TYPE_PRIMARY || state == NULL
				|| state->fb == NULL || state->crtc == NULL
				|| !state->crtc->state->active)
				continue;
			fb = state->fb;
			if (fb->format->cpp[0] != 4 || fb->obj[0] == NULL)
				continue;
			nvbo = nouveau_gem_object(fb->obj[0]);
			if (nvbo->bo.pin_count == 0 || nvbo->bo.resource == NULL
				|| nvbo->bo.resource->mem_type != TTM_PL_VRAM)
				continue;
			x = state->src_x >> 16;
			y = state->src_y >> 16;
			w = state->src_w >> 16;
			h = state->src_h >> 16;
			if (w <= 0 || h <= 0)
				continue;
			distance = x + y;
			if (haiku_console_connector != NULL && haiku_console_connector[0]) {
				struct drm_connector *connector;
				struct drm_connector_list_iter iter;
				drm_connector_list_iter_begin(dev, &iter);
				drm_for_each_connector_iter(connector, &iter) {
					if (connector->state != NULL
						&& connector->state->crtc == state->crtc
						&& strcmp(connector->name, haiku_console_connector) == 0)
						distance = -1;
				}
				drm_connector_list_iter_end(&iter);
			}
			if (distance >= best)
				continue;
			best = distance;
			best_bo = nvbo;
			best_offset = fb->offsets[0] + (u64)y * fb->pitches[0] + (u64)x * 4;
			width = w;
			height = h;
			pitch = fb->pitches[0];
		}
		/* another GPU's screen nearer the top left keeps the console */
		if (best_bo != NULL && (console_device == NULL || console_device == dev
				|| best <= console_distance)) {
			void *base = NULL;
			if (best_bo->kmap.virtual == NULL)
				nouveau_bo_map(best_bo);
			if (best_bo->kmap.virtual != NULL)
				base = (u8 *)best_bo->kmap.virtual + best_offset;
			if (base != NULL && (base != console_base || width != console_width
					|| height != console_height || pitch != console_pitch)) {
				struct nouveau_bo *previous = console_bo;
				drm_gem_object_get(&best_bo->bo.base);
				console_bo = best_bo;
				console_base = base;
				console_width = width;
				console_height = height;
				console_pitch = pitch;
				console_device = dev;
				console_distance = best;
				lx_haiku_debug_console(base, width, height, pitch);
				pr_info("nouveau_drm: kernel debugger console %dx%d on %s\n",
					width, height, dev_name(dev->dev));
				if (previous != NULL)
					drm_gem_object_put(&previous->bo.base);
			}
		}
	}
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}


static void
lx_glue_console_moved(void)
{
	if (console_device != NULL) {
		console_distance = INT_MAX;
		lx_glue_update_debug_console(console_device);
	}
}


static void
lx_glue_console_test(void)
{
	int x, y;
	if (console_base == NULL)
		return;
	for (y = 0; y < 64 && y < console_height; y++) {
		u32 *pixel = (u32 *)((u8 *)console_base + (size_t)y * console_pitch);
		for (x = 0; x < 64 && x < console_width; x++)
			pixel[x] = ((x / 32 + y / 32) & 1) ? 0xffff00ff : 0xff00ffff;
	}
	pr_info("nouveau_drm: marked the kernel debugger console\n");
}



long
lx_glue_ioctl(void *_file, unsigned int cmd, void *arg)
{
	struct file *filp = _file;
	long result;
	if (filp->f_op->unlocked_ioctl == NULL)
		return -ENOTTY;
	result = filp->f_op->unlocked_ioctl(filp, cmd, (unsigned long)arg);
	if (result == 0 && (cmd == DRM_IOCTL_MODE_ATOMIC
			|| cmd == DRM_IOCTL_MODE_SETCRTC)) {
		struct drm_file *file = filp->private_data;
		lx_glue_update_debug_console(file->minor->dev);
	}
	return result;
}


int
lx_glue_lab_perf_level(void *_file, unsigned int level)
{
	return -ENODEV;
}


int
lx_glue_lab_bench(void *_file)
{
	return -ENODEV;
}



/* #pragma mark - hot plugging */

/* Linux sends a uevent when monitors come and go; here waiters see a
 * generation count go up. */
static atomic_t hotplug_generation = ATOMIC_INIT(1);
static DECLARE_WAIT_QUEUE_HEAD(hotplug_wait);

static void
hotplug_event(struct drm_device *dev)
{
	atomic_inc(&hotplug_generation);
	wake_up_all(&hotplug_wait);
}


int
lx_glue_wait_hotplug(unsigned int *generation, unsigned int timeout_ms)
{
	unsigned int seen = *generation;
	long r = wait_event_interruptible_timeout(hotplug_wait,
		(unsigned int)atomic_read(&hotplug_generation) != seen,
		msecs_to_jiffies(timeout_ms));
	*generation = atomic_read(&hotplug_generation);
	return r < 0 ? (int)r : 0;
}


/* What writing "off", "on" or "detect" to a connector's status in sysfs
 * does on Linux: forces it disconnected or connected, or lets the hardware
 * say again, and tells everyone that monitors changed. For trying out
 * monitors coming and going without anybody at the machine.
 * force: 0 detect, 1 off, 2 on. */
int
lx_glue_force_connector(void *_file, unsigned int connector_id, int force)
{
	struct file *filp = _file;
	struct drm_file *file = filp->private_data;
	struct drm_device *dev = file->minor->dev;
	struct drm_connector *connector;
	enum drm_connector_force old_force;
	enum drm_connector_status old_status;
	int changed;

	if (force < 0 || force > 2)
		return -EINVAL;
	connector = drm_connector_lookup(dev, file, connector_id);
	if (connector == NULL)
		return -ENOENT;
	mutex_lock(&dev->mode_config.mutex);
	old_force = connector->force;
	old_status = connector->status;
	connector->force = force == 1 ? DRM_FORCE_OFF
		: force == 2 ? DRM_FORCE_ON : DRM_FORCE_UNSPECIFIED;
	if (old_force != connector->force || !connector->force) {
		connector->funcs->fill_modes(connector, dev->mode_config.max_width,
			dev->mode_config.max_height);
	}
	changed = connector->status != old_status;
	mutex_unlock(&dev->mode_config.mutex);
	pr_info("nouveau_drm: connector %s forced %s: %s\n", connector->name,
		force == 1 ? "off" : force == 2 ? "on" : "back to detection",
		connector->status == connector_status_connected ? "connected"
			: "disconnected");
	drm_connector_put(connector);
	if (changed)
		drm_kms_helper_hotplug_event(dev);
	return 0;
}


long
lx_glue_read(void *_file, void *buffer, unsigned long length)
{
	struct file *filp = _file;
	loff_t pos = 0;
	if (filp->f_op->read == NULL)
		return -EINVAL;
	return filp->f_op->read(filp, buffer, length, &pos);
}


/* #pragma mark - select */


struct lx_select {
	struct wait_queue_entry wait;
	struct wait_queue_head *head;
	void (*notify)(void *cookie, unsigned int events);
	void *cookie;
	poll_table table;
};


static int
lx_select_wake(struct wait_queue_entry *wait, unsigned mode, int sync,
	void *key)
{
	struct lx_select *select = container_of(wait, struct lx_select, wait);
	unsigned int events = (unsigned int)(uintptr_t)key;
	select->notify(select->cookie, events != 0 ? events : POLLIN);
	return 0;
}


static void
lx_select_queue(struct file *filp, struct wait_queue_head *head, poll_table *p)
{
	struct lx_select *select = container_of(p, struct lx_select, table);
	if (select->head != NULL)
		return;
	select->head = head;
	init_waitqueue_func_entry(&select->wait, lx_select_wake);
	add_wait_queue(head, &select->wait);
}


/* starts watching the file; returns the events ready now */
void *
lx_glue_select(void *_file, void (*notify)(void *cookie, unsigned int events),
	void *cookie, unsigned int *ready)
{
	struct file *filp = _file;
	struct lx_select *select = kzalloc(sizeof(*select), GFP_KERNEL);
	if (select == NULL)
		return NULL;
	select->notify = notify;
	select->cookie = cookie;
	select->table._qproc = lx_select_queue;
	select->table._key = ~0;
	*ready = filp->f_op->poll != NULL ? filp->f_op->poll(filp, &select->table)
		: 0;
	return select;
}


void
lx_glue_deselect(void *_select)
{
	struct lx_select *select = _select;
	if (select == NULL)
		return;
	if (select->head != NULL)
		remove_wait_queue(select->head, &select->wait);
	kfree(select);
}


/* #pragma mark - mapping buffers */

/*	A buffer mapped into teams: its Linux vm_area_struct, through which the
	Haiku driver runs the buffer's fault handler for the pages a team
	touches. When TTM moves the buffer it calls unmap_mapping_range(); the
	driver then unmaps the pages from every team, which fault them in again
	at the buffer's new place. Without that, a team would keep writing to
	the pages the buffer had before: VRAM another buffer has now, or memory
	Haiku has given back to someone else. TTM does so for every buffer it
	destroys as well, so the mappings are in an interval tree by the pages
	of the fake offsets they map: walking all of them, every mapped buffer
	of every team, cost 0.13 ms per buffer (jmgasper/haiku#36). */

struct lx_mapping {
	struct vm_area_struct vma;
	struct lx_vma_pages *pages;
	struct rb_node node;		/* in lx_mappings while attached */
	unsigned long first, last;	/* the fake offsets' pages it maps */
	unsigned long subtree_last;
	bool attached;
	struct mutex fault_lock;	/* faults share pages->pfns */
	void (*invalidate)(void *owner, unsigned long first, unsigned long count);
	void *owner;
};

#define LX_MAPPING_FIRST(mapping)	((mapping)->first)
#define LX_MAPPING_LAST(mapping)	((mapping)->last)

INTERVAL_TREE_DEFINE(struct lx_mapping, node, unsigned long, subtree_last,
	LX_MAPPING_FIRST, LX_MAPPING_LAST, static, lx_mapping_tree)

static struct rb_root_cached lx_mappings = RB_ROOT_CACHED;
static DEFINE_MUTEX(lx_mappings_lock);


/*	unmap_mapping_range(): \a start and \a length are bytes of the fake
	offsets (DRM_IOCTL_*_GEM_MMAP), a length of 0 meaning to the end. TTM
	calls it holding the buffer's reservation, before it moves the buffer. */
static void
lx_glue_unmap_mapping_range(struct address_space *as, loff_t start,
	loff_t length)
{
	unsigned long first = start >> PAGE_SHIFT;
	unsigned long last = length == 0 ? ULONG_MAX
		: (unsigned long)((start + length + PAGE_SIZE - 1) >> PAGE_SHIFT) - 1;
	struct lx_mapping *mapping;

	if (last < first)
		return;
	mutex_lock(&lx_mappings_lock);
	for (mapping = lx_mapping_tree_iter_first(&lx_mappings, first, last);
			mapping != NULL;
			mapping = lx_mapping_tree_iter_next(mapping, first, last)) {
		unsigned long from = max(first, mapping->first);
		unsigned long to = min(last, mapping->last);
		if (as != NULL && mapping->vma.vm_file->f_mapping != as)
			continue;
		mapping->invalidate(mapping->owner, from - mapping->first,
			to - from + 1);
	}
	mutex_unlock(&lx_mappings_lock);
}


/*	Looks up the buffer at a fake offset (DRM_IOCTL_*_GEM_MMAP) and sets up
	a mapping of it; lx_glue_map_fault() then finds the pages. Until
	lx_glue_map_detach(), \a invalidate is called (with the mappings' lock
	held) for the pages that move: their indices within the mapping. */
void *
lx_glue_map(void *_file, unsigned long long offset, unsigned long size,
	int writable, void (*invalidate)(void *owner, unsigned long first,
		unsigned long count), void *owner)
{
	struct file *filp = _file;
	struct lx_mapping *mapping;
	unsigned long count = size >> PAGE_SHIFT;
	int error;

	if (size == 0 || (size & ~PAGE_MASK) != 0 || (offset & ~PAGE_MASK) != 0)
		return ERR_PTR(-EINVAL);
	if (filp->f_op->mmap == NULL)
		return ERR_PTR(-ENODEV);

	mapping = kzalloc(sizeof(*mapping), GFP_KERNEL);
	if (mapping == NULL)
		return ERR_PTR(-ENOMEM);
	mapping->pages = kvzalloc(struct_size(mapping->pages, pfns, count),
		GFP_KERNEL);
	if (mapping->pages == NULL) {
		kfree(mapping);
		return ERR_PTR(-ENOMEM);
	}
	mapping->pages->count = count;
	mutex_init(&mapping->fault_lock);
	mapping->invalidate = invalidate;
	mapping->owner = owner;

	/* any page aligned address: only offsets within the area count */
	mapping->vma.vm_start = 0x100000000UL;
	mapping->vma.vm_end = mapping->vma.vm_start + size;
	mapping->vma.vm_pgoff = offset >> PAGE_SHIFT;
	mapping->vma.vm_file = filp;
	mapping->vma.vm_flags = VM_READ | VM_SHARED | VM_MAYREAD | VM_MAYSHARE
		| (writable ? VM_WRITE | VM_MAYWRITE : 0);
	mapping->vma.vm_page_prot = PAGE_SHARED;

	error = filp->f_op->mmap(filp, &mapping->vma);
	if (error != 0)
		goto err;
	if (mapping->vma.vm_ops == NULL || mapping->vma.vm_ops->fault == NULL) {
		error = -ENODEV;
		goto err_close;
	}

	mapping->first = mapping->vma.vm_pgoff;
	mapping->last = mapping->first + count - 1;
	mutex_lock(&lx_mappings_lock);
	lx_mapping_tree_insert(mapping, &lx_mappings);
	mapping->attached = true;
	mutex_unlock(&lx_mappings_lock);
	return mapping;

err_close:
	if (mapping->vma.vm_ops != NULL && mapping->vma.vm_ops->close != NULL)
		mapping->vma.vm_ops->close(&mapping->vma);
err:
	kvfree(mapping->pages);
	kfree(mapping);
	return ERR_PTR(error);
}


/*	Runs the buffer's fault handler for page \a first of the mapping and the
	ones after it, up to \a max pages or the end of the mapping, and returns
	how many pages from \a first on it found (their page frames in \a pfns,
	and the caching, LX_HAIKU_MAP_*, in \a caching), 0 when it should be
	tried again (a signal interrupted a wait), or a negative error. The
	buffer may move again right after; the caller has to know whether
	\a invalidate was called meanwhile. */
long
lx_glue_map_fault(void *_mapping, unsigned long first, unsigned long *pfns,
	unsigned long max, int *caching)
{
	struct lx_mapping *mapping = _mapping;
	struct lx_vma_pages *pages = mapping->pages;
	unsigned long end, i, found = 0;
	long result = 0;

	if (first >= pages->count)
		return -EFAULT;
	end = min(pages->count, first + max);

	mutex_lock(&mapping->fault_lock);
	memset(&pages->pfns[first], 0, (end - first) * sizeof(pages->pfns[0]));
	mapping->vma.lx_haiku = pages;
	for (i = first; i < end; i++) {
		struct vm_fault vmf = {};
		vm_fault_t fault;
		if (pages->pfns[i] != 0)
			continue;
		vmf.vma = &mapping->vma;
		vmf.address = mapping->vma.vm_start + i * PAGE_SIZE;
		vmf.pgoff = mapping->vma.vm_pgoff + i;
		vmf.flags = (mapping->vma.vm_flags & VM_WRITE) ? FAULT_FLAG_WRITE : 0;
		vmf.gfp_mask = GFP_KERNEL;
		fault = mapping->vma.vm_ops->fault(&vmf);
		if (fault & VM_FAULT_ERROR) {
			if (i == first)
				result = -EFAULT;
			break;
		}
		if (pages->pfns[i] == 0)
			break;
	}
	mapping->vma.lx_haiku = NULL;
	while (first + found < end && pages->pfns[first + found] != 0) {
		pfns[found] = pages->pfns[first + found];
		found++;
	}
	switch (pages->prot & _PAGE_CACHE_MASK) {
		case 0x18:
			*caching = LX_HAIKU_MAP_UC;
			break;
		case 0x8:
			*caching = LX_HAIKU_MAP_WC;
			break;
		default:
			*caching = LX_HAIKU_MAP_WB;
			break;
	}
	mutex_unlock(&mapping->fault_lock);
	return result < 0 ? result : (long)found;
}


/*	How many pages at the start of a new mapping DRM_HAIKU_IOCTL_MAP maps
	right away, of \a count; the rest fault in when the team first touches
	them, as on Linux, where mmap() maps nothing (airos-works/haiku#110:
	mapping all of a buffer at once cost ~0.27 us a page, used or not).
	None while the GPU still clears or moves the buffer (a kernel fence,
	which the fault handler would wait for). "drmparam haiku_map_prefault
	-1" maps whole buffers again. */
static int haiku_map_prefault = 16;
module_param(haiku_map_prefault, int, 0644);

unsigned long
lx_glue_map_prefault(void *_mapping, unsigned long count)
{
	struct lx_mapping *mapping = _mapping;
	/* drm_gem_mmap()'s object, or the TTM buffer that starts with it */
	struct drm_gem_object *object = mapping->vma.vm_private_data;
	int prefault = READ_ONCE(haiku_map_prefault);

	if (prefault < 0)
		return count;
	if (object != NULL && object->resv != NULL
		&& !dma_resv_test_signaled(object->resv, DMA_RESV_USAGE_KERNEL))
		return 0;
	return min_t(unsigned long, prefault, count);
}


/*	Ends the calls to the mapping's invalidate function: when this returns,
	none is running or will be. */
void
lx_glue_map_detach(void *_mapping)
{
	struct lx_mapping *mapping = _mapping;
	mutex_lock(&lx_mappings_lock);
	if (mapping->attached) {
		lx_mapping_tree_remove(mapping, &lx_mappings);
		mapping->attached = false;
	}
	mutex_unlock(&lx_mappings_lock);
}


/*	Ends a mapping (detached, and with no lx_glue_map_fault() running). */
void
lx_glue_unmap(void *_mapping)
{
	struct lx_mapping *mapping = _mapping;
	lx_glue_map_detach(mapping);
	if (mapping->vma.vm_ops != NULL && mapping->vma.vm_ops->close != NULL)
		mapping->vma.vm_ops->close(&mapping->vma);
	mutex_destroy(&mapping->fault_lock);
	kvfree(mapping->pages);
	kfree(mapping);
}


int
lx_glue_is_error(const void *pointer)
{
	return IS_ERR(pointer) ? PTR_ERR(pointer) : 0;
}


/* #pragma mark - self test */

/*	Exercises the compat layer without a GPU (settings: "haiku_selftest 1"),
	for virtual machines and before trusting a new build with the hardware. */

static struct completion selftest_done;
static atomic_t selftest_counter;
static DEFINE_MUTEX(selftest_mutex);
static void lx_glue_watchdog_report(long irq_thread, u64 running_ms);


static void *
selftest_current(void)
{
	return current;
}


static int
selftest_blocker(void *unused)
{
	mutex_lock(&selftest_mutex);
	atomic_inc(&selftest_counter);
	mutex_unlock(&selftest_mutex);
	return 0;
}

static int
selftest_thread(void *data)
{
	atomic_inc(&selftest_counter);
	complete(&selftest_done);
	while (!kthread_should_stop())
		schedule_timeout_interruptible(msecs_to_jiffies(10));
	return 42;
}


static void
selftest_work(struct work_struct *work)
{
	atomic_inc(&selftest_counter);
}


static void
selftest_timer(struct timer_list *timer)
{
	atomic_inc(&selftest_counter);
}


static enum hrtimer_restart
selftest_hrtimer(struct hrtimer *timer)
{
	atomic_inc(&selftest_counter);
	return HRTIMER_NORESTART;
}


static void
selftest_rcu(struct rcu_head *head)
{
	atomic_inc(&selftest_counter);
}


#define SELFTEST(condition, what) \
	do { \
		if (!(condition)) { \
			pr_err("nouveau selftest: FAILED: %s\n", what); \
			failures++; \
		} else \
			pr_info("nouveau selftest: ok: %s\n", what); \
	} while (0)

int
lx_glue_selftest(void)
{
	static DEFINE_WW_CLASS(selftest_ww_class);
	struct task_struct *thread, *blocker;
	struct work_struct work;
	struct delayed_work dwork;
	struct timer_list timer;
	struct hrtimer hrtimer;
	struct rcu_head rcu;
	struct xarray xa;
	struct idr idr;
	struct ww_mutex a, b;
	struct ww_acquire_ctx ctx;
	struct page *page, *pages[4];
	char buffer[128];
	void *p, *mapped;
	u32 id;
	int failures = 0, i, result;

	snprintf(buffer, sizeof(buffer), "%d %u %x %s %pe %03d", -5, 7u, 0xbeef,
		"str", ERR_PTR(-EINVAL), 9);
	SELFTEST(!strcmp(buffer, "-5 7 beef str -EINVAL 009"), buffer);

	p = kmalloc(100, GFP_KERNEL);
	SELFTEST(p != NULL && ksize(p) == 100, "kmalloc and ksize");
	p = krealloc(p, 5000, GFP_KERNEL | __GFP_ZERO);
	SELFTEST(p != NULL && ((char *)p)[4999] == 0, "krealloc with zeroing");
	kfree(p);
	p = kmalloc(4096, GFP_KERNEL);
	SELFTEST(p != NULL && ((uintptr_t)p & 4095) == 0, "page sized kmalloc aligned");
	kfree(p);
	p = vzalloc(1 << 20);
	SELFTEST(p != NULL && is_vmalloc_addr(p), "vzalloc");
	kvfree(p);

	page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 2);
	SELFTEST(page != NULL && pfn_to_page(page_to_pfn(page) + 3) == page + 3,
		"alloc_pages order 2 and pfn_to_page");
	if (page != NULL) {
		memset(page_address(page), 0x5a, 4 * PAGE_SIZE);
		for (i = 0; i < 4; i++)
			pages[i] = page + 3 - i;
		mapped = vmap(pages, 4, VM_MAP, PAGE_KERNEL);
		SELFTEST(mapped != NULL && ((u8 *)mapped)[3 * PAGE_SIZE + 17] == 0x5a,
			"vmap");
		if (mapped != NULL)
			vunmap(mapped);
		__free_pages(page, 2);
	}
	SELFTEST(alloc_pages(GFP_KERNEL | __GFP_NORETRY, 4) == NULL,
		"opportunistic high-order runs refused");
	{
		/* the pages of a 1 MiB buffer in GTT, as TTM takes them */
		struct page **single = kcalloc(256, sizeof(*single), GFP_KERNEL);
		u64 start = ktime_get_ns(), allocated, freed;
		bool found = single != NULL;
		for (i = 0; found && i < 256; i++) {
			single[i] = alloc_page(GFP_HIGHUSER | __GFP_ZERO);
			found = single[i] != NULL
				&& pfn_to_page(page_to_pfn(single[i])) == single[i]
				&& ((u32 *)page_address(single[i]))[1023] == 0;
		}
		allocated = ktime_get_ns();
		while (single != NULL && i-- > 0) {
			if (single[i] != NULL)
				__free_page(single[i]);
		}
		freed = ktime_get_ns();
		SELFTEST(found, "single pages: zeroed, found by pfn_to_page");
		pr_info("nouveau selftest: 256 pages allocated in %llu us, freed in "
			"%llu us\n", (allocated - start) / 1000, (freed - allocated) / 1000);
		kfree(single);
	}
	{
		/* the page pool: freed pages written all over come back zeroed,
		   right away (zeroed on the spot) and after the worker ran */
		struct page **single = kcalloc(256, sizeof(*single), GFP_KERNEL);
		int round;
		for (round = 0; single != NULL && round < 3; round++) {
			u64 start = ktime_get_ns(), allocated, freed;
			bool zeroed = true, found = true;
			int j;
			for (i = 0; i < 256; i++) {
				single[i] = alloc_page(GFP_HIGHUSER | __GFP_ZERO);
				if (single[i] == NULL
					|| pfn_to_page(page_to_pfn(single[i])) != single[i]
					|| page_count(single[i]) != 1)
					found = false;
			}
			allocated = ktime_get_ns();
			for (i = 0; i < 256 && single[i] != NULL; i++) {
				u64 *words = page_address(single[i]);
				for (j = 0; j < PAGE_SIZE / 8; j++)
					zeroed &= words[j] == 0;
				memset(words, 0xa5, PAGE_SIZE);
			}
			start += ktime_get_ns() - allocated;
			allocated = ktime_get_ns();
			for (i = 0; i < 256; i++) {
				if (single[i] != NULL)
					__free_page(single[i]);
			}
			freed = ktime_get_ns();
			SELFTEST(found && zeroed, round == 0 ? "pool: pages come back zeroed"
				: round == 1 ? "pool: pages written and freed come back zeroed "
					"(right after)"
				: "pool: pages written and freed come back zeroed (worker)");
			pr_info("nouveau selftest: pool round %d: 256 pages allocated in "
				"%llu us, freed in %llu us\n", round,
				(allocated - start) / 1000, (freed - allocated) / 1000);
			if (round > 0)
				msleep(100);
		}
		kfree(single);
	}
	{
		/* 2 MiB runs, as TTM asks for them: the pool has none at first and
		   collects some; one written all over comes back zeroed, right
		   away and after the worker ran */
		const gfp_t gfp = GFP_HIGHUSER | __GFP_ZERO | __GFP_NORETRY
			| __GFP_NOWARN;
		struct page *run = alloc_pages(gfp, 9);
		int round;
		if (run != NULL)
			__free_pages(run, 9);
		msleep(300);
		for (round = 0; round < 3; round++) {
			u64 start = ktime_get_ns(), taken;
			bool zeroed = true, found;
			run = alloc_pages(gfp, 9);
			taken = ktime_get_ns() - start;
			found = run != NULL && (page_to_pfn(run) & 511) == 0
				&& pfn_to_page(page_to_pfn(run) + 511) == run + 511
				&& page_count(run) == 1;
			if (run != NULL) {
				for (i = 0; i < 512; i++) {
					u64 *words = page_address(run + i);
					int j;
					for (j = 0; j < PAGE_SIZE / 8; j++)
						zeroed &= words[j] == 0;
					memset(words, 0xa5, PAGE_SIZE);
				}
				start = ktime_get_ns();
				__free_pages(run, 9);
				pr_info("nouveau selftest: run round %d: taken in %llu us, freed "
					"in %llu us\n", round, taken / 1000,
					(ktime_get_ns() - start) / 1000);
			}
			SELFTEST(found && zeroed, round == 0
				? "pool: a 2 MiB run collected, aligned, zeroed"
				: round == 1 ? "pool: a run written and freed comes back zeroed "
					"(right after)"
				: "pool: a run written and freed comes back zeroed (worker)");
			if (round > 0)
				msleep(100);
		}
		{
			/* pieces split off a run: aligned, zeroed, their own */
			struct page *eight = alloc_pages(gfp, 8);
			struct page *four = alloc_pages(gfp, 4);
			bool zeroed = true, found = eight != NULL && four != NULL
				&& (page_to_pfn(eight) & 255) == 0
				&& (page_to_pfn(four) & 15) == 0
				&& pfn_to_page(page_to_pfn(eight) + 255) == eight + 255
				&& pfn_to_page(page_to_pfn(four) + 15) == four + 15;
			for (i = 0; found && i < 256; i++) {
				u64 *words = page_address(eight + i);
				int j;
				for (j = 0; j < PAGE_SIZE / 8; j++)
					zeroed &= words[j] == 0;
			}
			SELFTEST(found && zeroed,
				"pool: 1 MiB and 64 KiB pieces of a run, aligned and zeroed");
			if (eight != NULL)
				__free_pages(eight, 8);
			if (four != NULL)
				__free_pages(four, 4);
		}
		{
			char line[400];
			lx_mm_stats(line, sizeof(line));
			pr_info("nouveau selftest: %s\n", line);
		}
	}

	xa_init_flags(&xa, XA_FLAGS_ALLOC1);
	result = xa_alloc(&xa, &id, &xa, xa_limit_32b, GFP_KERNEL);
	SELFTEST(result == 0 && id == 1 && xa_load(&xa, 1) == &xa, "xa_alloc");
	xa_lock(&xa);
	SELFTEST(xa_load(&xa, 1) == &xa, "xa_load with xa_lock held");
	xa_unlock(&xa);
	xa_erase(&xa, 1);
	SELFTEST(xa_empty(&xa), "xa_erase");
	xa_destroy(&xa);
	idr_init_base(&idr, 1);
	result = idr_alloc(&idr, &idr, 1, 0, GFP_KERNEL);
	SELFTEST(result == 1 && idr_find(&idr, 1) == &idr, "idr_alloc");
	{
		/* walked from 0, as idr_for_each_entry() does, and allocated from
		 * below its base: amdgpu's contexts and DRM's handles */
		struct idr based;
		void *entry;
		int id, seen = 0, a, b, c;
		idr_init_base(&based, 1);
		a = idr_alloc(&based, &based, 0, 0, GFP_KERNEL);
		b = idr_alloc(&based, &seen, 1, 0, GFP_KERNEL);
		c = idr_alloc_cyclic(&based, &id, 1, 0, GFP_KERNEL);
		idr_for_each_entry(&based, entry, id)
			seen++;
		SELFTEST(a == 1 && b == 2 && c == 3 && seen == 3
			&& idr_find(&based, 0) == NULL && idr_find(&based, 3) == &id,
			"idr_init_base(1): allocation and idr_for_each_entry");
		idr_destroy(&based);
	}
	idr_destroy(&idr);

	ww_mutex_init(&a, &selftest_ww_class);
	ww_mutex_init(&b, &selftest_ww_class);
	ww_acquire_init(&ctx, &selftest_ww_class);
	SELFTEST(ww_mutex_lock(&a, &ctx) == 0 && ww_mutex_lock(&b, &ctx) == 0
		&& ww_mutex_lock(&a, &ctx) == -EALREADY, "ww_mutex");
	ww_mutex_unlock(&b);
	ww_mutex_unlock(&a);
	ww_acquire_fini(&ctx);

	atomic_set(&selftest_counter, 0);
	init_completion(&selftest_done);
	thread = kthread_run(selftest_thread, NULL, "selftest");
	SELFTEST(!IS_ERR(thread) && wait_for_completion_timeout(&selftest_done,
		msecs_to_jiffies(2000)) != 0, "kthread start and completion");
	if (!IS_ERR(thread))
		SELFTEST(kthread_stop(thread) == 42, "kthread_stop");

	INIT_WORK(&work, selftest_work);
	queue_work(system_wq, &work);
	flush_work(&work);
	SELFTEST(atomic_read(&selftest_counter) == 2, "work and flush_work");

	INIT_DELAYED_WORK(&dwork, (work_func_t)selftest_work);
	schedule_delayed_work(&dwork, msecs_to_jiffies(20));
	msleep(100);
	SELFTEST(atomic_read(&selftest_counter) == 3, "delayed work");
	cancel_delayed_work_sync(&dwork);

	timer_setup(&timer, selftest_timer, 0);
	mod_timer(&timer, jiffies + msecs_to_jiffies(10));
	msleep(60);
	SELFTEST(atomic_read(&selftest_counter) == 4, "timer_list");
	timer_delete_sync(&timer);

	hrtimer_setup(&hrtimer, selftest_hrtimer, CLOCK_MONOTONIC,
		HRTIMER_MODE_REL);
	hrtimer_start(&hrtimer, ms_to_ktime(5), HRTIMER_MODE_REL);
	msleep(50);
	SELFTEST(atomic_read(&selftest_counter) == 5, "hrtimer");
	hrtimer_cancel(&hrtimer);

	call_rcu(&rcu, selftest_rcu);
	rcu_barrier();
	SELFTEST(atomic_read(&selftest_counter) == 6, "call_rcu and rcu_barrier");

	/* a thread that ended takes its task with it */
	SELFTEST(lx_haiku_selftest_task_exit(selftest_current) == 0,
		"task of an ended thread freed");

	/* the watchdog's report on a thread blocked on a lock */
	mutex_lock(&selftest_mutex);
	blocker = kthread_run(selftest_blocker, NULL, "selftest blocker");
	msleep(50);
	SELFTEST(!IS_ERR(blocker)
		&& lx_lock_waited_for(blocker->pid) == &selftest_mutex.l,
		"lock waiter table");
	if (!IS_ERR(blocker))
		lx_glue_watchdog_report(blocker->pid, 0);
	mutex_unlock(&selftest_mutex);
	msleep(50);
	SELFTEST(atomic_read(&selftest_counter) == 7, "blocked thread got the lock");

	pr_info("nouveau selftest: %d failure(s)\n", failures);
	return failures;
}


/* #pragma mark - fake device */

/*	A DRM device without hardware ("haiku_selftest 2"), so that opening
	/dev/dri nodes, ioctls, mapping buffers and closing can be tried in a
	virtual machine (tests/fakemap.c). Its buffers are pages of RAM; the
	"move" ioctl gives one new pages the way TTM moves a buffer (unmap the
	CPU mappings, copy, under the reservation) and poisons the old ones, so
	that a mapping that did not follow reads the poison. */

struct drm_haiku_fake_create {
	__u64 size;
	__u32 handle;
	__u32 flags;		/* 1: the CPU cannot reach it (faults fail) */
	__u64 offset;		/* for DRM_HAIKU_IOCTL_MAP */
};

struct drm_haiku_fake_move {
	__u32 handle;
	__u32 pad;
};

#define DRM_HAIKU_FAKE_CREATE	0x00
#define DRM_HAIKU_FAKE_MOVE		0x01
#define DRM_IOCTL_HAIKU_FAKE_CREATE	DRM_IOWR(DRM_COMMAND_BASE \
	+ DRM_HAIKU_FAKE_CREATE, struct drm_haiku_fake_create)
#define DRM_IOCTL_HAIKU_FAKE_MOVE	DRM_IOW(DRM_COMMAND_BASE \
	+ DRM_HAIKU_FAKE_MOVE, struct drm_haiku_fake_move)

#define FAKE_POISON	0x5a

#define FAKE_OLD_KEPT	8

struct fake_bo {
	struct drm_gem_object base;
	unsigned long count;
	struct page **pages;
	struct list_head old;		/* earlier pages, poisoned; older ones are
								   freed, as TTM frees them */
	unsigned int old_count;
	bool unreachable;
};

struct fake_old_pages {
	struct list_head entry;
	struct page **pages;
};


static void
fake_free_pages(struct page **pages, unsigned long count)
{
	unsigned long i;
	for (i = 0; i < count; i++) {
		if (pages[i] != NULL)
			__free_page(pages[i]);
	}
	kvfree(pages);
}


static struct page **
fake_alloc_pages(unsigned long count)
{
	struct page **pages = kvcalloc(count, sizeof(*pages), GFP_KERNEL);
	unsigned long i;
	if (pages == NULL)
		return NULL;
	for (i = 0; i < count; i++) {
		pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (pages[i] == NULL) {
			fake_free_pages(pages, count);
			return NULL;
		}
	}
	return pages;
}


static void
fake_bo_free(struct drm_gem_object *obj)
{
	struct fake_bo *bo = container_of(obj, struct fake_bo, base);
	struct fake_old_pages *old, *next;
	list_for_each_entry_safe(old, next, &bo->old, entry) {
		fake_free_pages(old->pages, bo->count);
		kfree(old);
	}
	fake_free_pages(bo->pages, bo->count);
	drm_gem_free_mmap_offset(obj);
	drm_gem_object_release(obj);
	kfree(bo);
}


static vm_fault_t
fake_fault(struct vm_fault *vmf)
{
	struct fake_bo *bo = vmf->vma->vm_private_data;
	unsigned long page = vmf->pgoff - drm_vma_node_start(&bo->base.vma_node);
	vm_fault_t result;
	/* like TTM's: under the reservation, which a move holds */
	if (dma_resv_lock_interruptible(bo->base.resv, NULL) != 0)
		return VM_FAULT_NOPAGE;
	if (page >= bo->count || bo->unreachable)
		result = VM_FAULT_SIGBUS;
	else {
		result = vmf_insert_pfn_prot(vmf->vma, vmf->address,
			page_to_pfn(bo->pages[page]), PAGE_KERNEL);
	}
	dma_resv_unlock(bo->base.resv);
	return result;
}


static const struct vm_operations_struct fake_vm_ops = {
	.fault = fake_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs fake_gem_funcs = {
	.free = fake_bo_free,
	.vm_ops = &fake_vm_ops,
};


static int
fake_create_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_haiku_fake_create *args = data;
	struct fake_bo *bo;
	int error;

	if (args->size == 0 || args->size > (64 << 20))
		return -EINVAL;
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return -ENOMEM;
	bo->count = PAGE_ALIGN(args->size) >> PAGE_SHIFT;
	bo->unreachable = (args->flags & 1) != 0;
	INIT_LIST_HEAD(&bo->old);
	bo->base.funcs = &fake_gem_funcs;
	drm_gem_private_object_init(dev, &bo->base, bo->count << PAGE_SHIFT);
	bo->pages = fake_alloc_pages(bo->count);
	if (bo->pages == NULL) {
		drm_gem_object_release(&bo->base);
		kfree(bo);
		return -ENOMEM;
	}
	error = drm_gem_create_mmap_offset(&bo->base);
	if (error == 0)
		error = drm_gem_handle_create(file, &bo->base, &args->handle);
	if (error == 0)
		args->offset = drm_vma_node_offset_addr(&bo->base.vma_node);
	drm_gem_object_put(&bo->base);
	return error;
}


static int
fake_move_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_haiku_fake_move *args = data;
	struct drm_gem_object *obj = drm_gem_object_lookup(file, args->handle);
	struct fake_bo *bo;
	struct fake_old_pages *old;
	struct page **pages;
	unsigned long i;

	if (obj == NULL)
		return -ENOENT;
	bo = container_of(obj, struct fake_bo, base);
	old = kzalloc(sizeof(*old), GFP_KERNEL);
	pages = fake_alloc_pages(bo->count);
	if (old == NULL || pages == NULL) {
		kfree(old);
		if (pages != NULL)
			fake_free_pages(pages, bo->count);
		drm_gem_object_put(obj);
		return -ENOMEM;
	}
	dma_resv_lock(obj->resv, NULL);
	/* what ttm_bo_handle_move_mem() does first */
	drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping);
	for (i = 0; i < bo->count; i++) {
		memcpy(page_address(pages[i]), page_address(bo->pages[i]), PAGE_SIZE);
		memset(page_address(bo->pages[i]), FAKE_POISON, PAGE_SIZE);
	}
	old->pages = bo->pages;
	list_add_tail(&old->entry, &bo->old);
	bo->pages = pages;
	if (++bo->old_count > FAKE_OLD_KEPT) {
		old = list_first_entry(&bo->old, struct fake_old_pages, entry);
		list_del(&old->entry);
		fake_free_pages(old->pages, bo->count);
		kfree(old);
		bo->old_count--;
	}
	dma_resv_unlock(obj->resv);
	drm_gem_object_put(obj);
	return 0;
}


/*	"userptr": a buffer of the calling team's own pages, wired as amdgpu's
	userptr buffers are (userptr.c), to try their life in a virtual machine
	(tests/fakeuserptr.c). The ioctl writes a pattern into the pages through
	their physical addresses, as a GPU would, and then, if asked, the device
	keeps writing the first word of each page for a while behind a fence on
	the buffer, like GPU work the team submitted before it died. Freeing the
	buffer lets the pages go only once that fence has signalled, in a work
	item, as TTM does. */

struct drm_haiku_fake_userptr {
	__u64 address;
	__u64 size;
	__u32 flags;		/* 1: read only, 2: anonymous memory only */
	__u32 handle;
	__u32 busy_ms;		/* how long the device keeps writing */
	__u32 pattern;		/* word i of the buffer becomes pattern + i */
};

#define DRM_HAIKU_FAKE_USERPTR	0x02
#define DRM_IOCTL_HAIKU_FAKE_USERPTR	DRM_IOWR(DRM_COMMAND_BASE \
	+ DRM_HAIKU_FAKE_USERPTR, struct drm_haiku_fake_userptr)

struct fake_userptr {
	struct drm_gem_object base;
	struct lx_userptr *userptr;
	struct page **pages;		/* as the device sees them */
	unsigned long count;
	struct dma_fence *fence;	/* until the device is done writing */
	struct delayed_work busy;
	unsigned long busy_until;
	u32 busy_count;
	struct work_struct release;
};

static DEFINE_SPINLOCK(fake_fence_lock);
static u64 fake_fence_context;
static atomic64_t fake_fence_seqno;


static const char *
fake_fence_name(struct dma_fence *fence)
{
	return "haiku_fake";
}


static const struct dma_fence_ops fake_fence_ops = {
	.get_driver_name = fake_fence_name,
	.get_timeline_name = fake_fence_name,
};


/* a pass over the pages every millisecond until the time is up */
static void
fake_userptr_busy(struct work_struct *work)
{
	struct fake_userptr *bo = container_of(to_delayed_work(work),
		struct fake_userptr, busy);
	struct dma_fence *fence;
	unsigned long i;

	bo->busy_count++;
	for (i = 0; i < bo->count; i++)
		WRITE_ONCE(*(u32 *)page_address(bo->pages[i]), bo->busy_count);
	if (time_before(jiffies, bo->busy_until)) {
		queue_delayed_work(system_unbound_wq, &bo->busy, msecs_to_jiffies(1));
		return;
	}
	fence = bo->fence;
	bo->fence = NULL;
	dma_fence_signal(fence);
	dma_fence_put(fence);
}


static void
fake_userptr_release(struct work_struct *work)
{
	struct fake_userptr *bo = container_of(work, struct fake_userptr, release);

	dma_resv_wait_timeout(bo->base.resv, DMA_RESV_USAGE_BOOKKEEP, false,
		MAX_SCHEDULE_TIMEOUT);
	cancel_delayed_work_sync(&bo->busy);
	lx_userptr_release(bo->userptr);
	drm_gem_object_release(&bo->base);
	kvfree(bo->pages);
	kfree(bo);
}


static void
fake_userptr_free(struct drm_gem_object *obj)
{
	struct fake_userptr *bo = container_of(obj, struct fake_userptr, base);

	lx_userptr_detach(bo->userptr);
	INIT_WORK(&bo->release, fake_userptr_release);
	queue_work(system_unbound_wq, &bo->release);
}


static const struct drm_gem_object_funcs fake_userptr_funcs = {
	.free = fake_userptr_free,
};


static int
fake_userptr_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct drm_haiku_fake_userptr *args = data;
	bool read_only = (args->flags & 1) != 0;
	struct fake_userptr *bo;
	struct hmm_range *range;
	unsigned long i, j;
	int error;

	if (args->size == 0 || args->size > (256 << 20)
		|| offset_in_page(args->address | args->size) || (args->flags & ~3)
		|| (read_only && args->busy_ms != 0))
		return -EINVAL;
	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (bo == NULL)
		return -ENOMEM;
	bo->count = args->size >> PAGE_SHIFT;
	bo->pages = kvmalloc_array(bo->count, sizeof(*bo->pages), GFP_KERNEL);
	if (bo->pages == NULL) {
		kfree(bo);
		return -ENOMEM;
	}
	bo->base.funcs = &fake_userptr_funcs;
	drm_gem_private_object_init(dev, &bo->base, args->size);
	INIT_DELAYED_WORK(&bo->busy, fake_userptr_busy);
	error = lx_userptr_create(&bo->base, args->address, bo->count, !read_only,
		(args->flags & 2) != 0, &bo->userptr);
	if (error != 0) {
		drm_gem_object_release(&bo->base);
		kvfree(bo->pages);
		kfree(bo);
		return error;
	}

	error = lx_userptr_get_pages(bo->userptr, &range);
	if (error == 0) {
		for (i = 0; i < bo->count; i++)
			bo->pages[i] = hmm_pfn_to_page(range->hmm_pfns[i]);
		lx_userptr_get_pages_done(range);
	}
	for (i = 0; error == 0 && !read_only && i < bo->count; i++) {
		u32 *words = page_address(bo->pages[i]);
		for (j = 0; j < PAGE_SIZE / 4; j++)
			words[j] = args->pattern + i * (PAGE_SIZE / 4) + j;
	}
	if (error == 0 && args->busy_ms != 0) {
		bo->fence = kzalloc(sizeof(*bo->fence), GFP_KERNEL);
		if (bo->fence == NULL)
			error = -ENOMEM;
	}
	if (error == 0 && args->busy_ms != 0) {
		dma_fence_init(bo->fence, &fake_fence_ops, &fake_fence_lock,
			fake_fence_context, atomic64_inc_return(&fake_fence_seqno));
		dma_resv_lock(bo->base.resv, NULL);
		error = dma_resv_reserve_fences(bo->base.resv, 1);
		if (error == 0) {
			dma_resv_add_fence(bo->base.resv, bo->fence,
				DMA_RESV_USAGE_WRITE);
			bo->busy_until = jiffies + msecs_to_jiffies(args->busy_ms);
			queue_delayed_work(system_unbound_wq, &bo->busy, 0);
		} else {
			dma_fence_signal(bo->fence);
			dma_fence_put(bo->fence);
			bo->fence = NULL;
		}
		dma_resv_unlock(bo->base.resv);
	}
	if (error == 0)
		error = drm_gem_handle_create(file, &bo->base, &args->handle);
	drm_gem_object_put(&bo->base);
	return error;
}


static const struct drm_ioctl_desc fake_ioctls[] = {
	DRM_IOCTL_DEF_DRV(HAIKU_FAKE_CREATE, fake_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HAIKU_FAKE_MOVE, fake_move_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(HAIKU_FAKE_USERPTR, fake_userptr_ioctl,
		DRM_RENDER_ALLOW),
};


static const struct file_operations fake_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = drm_gem_mmap,
	.fop_flags = FOP_UNSIGNED_OFFSET,
};

static const struct drm_driver fake_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER,
	.ioctls = fake_ioctls,
	.num_ioctls = ARRAY_SIZE(fake_ioctls),
	.fops = &fake_fops,
	.name = "haiku_fake",
	.desc = "Haiku compat test device",
	.major = 1,
	.minor = 0,
};

static struct device fake_parent;


int
lx_glue_fake_device_start(void)
{
	int error;
	fake_fence_context = dma_fence_context_alloc(1);
	device_initialize(&fake_parent);
	dev_set_name(&fake_parent, "haiku-fake");
	fake_drm = drm_dev_alloc(&fake_driver, &fake_parent);
	if (IS_ERR(fake_drm)) {
		error = PTR_ERR(fake_drm);
		fake_drm = NULL;
		return error;
	}
	error = drm_dev_register(fake_drm, 0);
	if (error != 0) {
		drm_dev_put(fake_drm);
		fake_drm = NULL;
		return error;
	}
	pr_info("nouveau selftest: fake DRM device registered\n");
	return 0;
}


void
lx_glue_fake_device_stop(void)
{
	if (fake_drm == NULL)
		return;
	drm_dev_unregister(fake_drm);
	drm_dev_put(fake_drm);
	fake_drm = NULL;
}


/* DRM_HAIKU_IOCTL_CRC: amdgpu's display engine only */
int
lx_glue_crc(void *_file, unsigned crtc_index, int op, unsigned *crc,
	unsigned *_frame, const unsigned short *window)
{
	return -ENODEV;
}



/* #pragma mark - the screen's frame buffer */

/*	DRM_HAIKU_IOCTL_SCANOUT: the buffer the screen shows, for a renderer that
	copies its finished frames straight into it (VkImportScanoutMemoryHAIKU)
	instead of handing them to app_server, which would copy them there from
	system memory. The accelerant publishes the buffer it scans out from;
	another client gets a handle to it on its own file, as GEM_OPEN of a
	flink name would give one (which render nodes may not use). One buffer
	per GPU: amdgpu_kms has one frame buffer for all monitors. */

struct lx_scanout {
	struct drm_device *dev;
	struct drm_file *publisher;
	struct drm_gem_object *object;
	u32 width, height, pitch, format;
	u32 generation;
};

#define LX_SCANOUT_DEVICES	4
static struct lx_scanout lx_scanouts[LX_SCANOUT_DEVICES];
static u32 lx_scanout_generation;
static DEFINE_MUTEX(lx_scanout_lock);


static struct lx_scanout *
lx_scanout_of(struct drm_device *dev, bool create)
{
	struct lx_scanout *unused = NULL;
	int i;
	for (i = 0; i < LX_SCANOUT_DEVICES; i++) {
		if (lx_scanouts[i].dev == dev)
			return &lx_scanouts[i];
		if (lx_scanouts[i].dev == NULL && unused == NULL)
			unused = &lx_scanouts[i];
	}
	if (!create || unused == NULL)
		return NULL;
	unused->dev = dev;
	return unused;
}


static void
lx_scanout_withdraw(struct lx_scanout *scanout)
{
	if (scanout->object != NULL)
		drm_gem_object_put(scanout->object);
	scanout->object = NULL;
	scanout->publisher = NULL;
	scanout->generation = ++lx_scanout_generation;
}


/* the publisher's file is closing: its buffer is no longer the screen's */
static void
lx_scanout_file_closing(struct drm_file *file)
{
	int i;
	if (file == NULL)
		return;
	mutex_lock(&lx_scanout_lock);
	for (i = 0; i < LX_SCANOUT_DEVICES; i++) {
		if (lx_scanouts[i].publisher == file)
			lx_scanout_withdraw(&lx_scanouts[i]);
	}
	mutex_unlock(&lx_scanout_lock);
}


int
lx_glue_scanout(void *_file, struct lx_glue_scanout_args *args)
{
	struct file *filp = _file;
	struct drm_file *file = filp->private_data;
	struct drm_device *dev = file->minor->dev;
	struct drm_gem_object *object = NULL;
	struct lx_scanout *scanout;
	int error = 0;

	if (args->op == DRM_HAIKU_SCANOUT_PUBLISH) {
		if (file->minor->type != DRM_MINOR_PRIMARY)
			return -EACCES;
		if (args->width == 0 || args->height == 0
			|| args->pitch < args->width * 4)
			return -EINVAL;
		object = drm_gem_object_lookup(file, args->handle);
		if (object == NULL)
			return -ENOENT;
		if ((u64)args->pitch * args->height > object->size) {
			drm_gem_object_put(object);
			return -EINVAL;
		}
	} else if (args->op == DRM_HAIKU_SCANOUT_WITHDRAW) {
		if (file->minor->type != DRM_MINOR_PRIMARY)
			return -EACCES;
	} else if (args->op != DRM_HAIKU_SCANOUT_GET)
		return -EINVAL;

	mutex_lock(&lx_scanout_lock);
	scanout = lx_scanout_of(dev, args->op == DRM_HAIKU_SCANOUT_PUBLISH);
	switch (args->op) {
		case DRM_HAIKU_SCANOUT_PUBLISH:
			if (scanout == NULL) {
				drm_gem_object_put(object);
				error = -ENOSPC;
				break;
			}
			lx_scanout_withdraw(scanout);
			scanout->object = object;
			scanout->publisher = file;
			scanout->width = args->width;
			scanout->height = args->height;
			scanout->pitch = args->pitch;
			scanout->format = args->format;
			args->generation = scanout->generation;
			break;
		case DRM_HAIKU_SCANOUT_WITHDRAW:
			if (scanout != NULL && scanout->publisher == file) {
				lx_scanout_withdraw(scanout);
				args->generation = scanout->generation;
			}
			break;
		case DRM_HAIKU_SCANOUT_GET:
			if (scanout == NULL || scanout->object == NULL) {
				error = -ENOENT;
				break;
			}
			error = drm_gem_handle_create(file, scanout->object,
				&args->handle);
			if (error != 0)
				break;
			args->width = scanout->width;
			args->height = scanout->height;
			args->pitch = scanout->pitch;
			args->format = scanout->format;
			args->size = scanout->object->size;
			args->generation = scanout->generation;
			break;
	}
	mutex_unlock(&lx_scanout_lock);
	return error;
}


/* DRM_HAIKU_IOCTL_BACKLIGHT: no built-in panels on nouveau's GPUs here
 * (CONFIG_DRM_NOUVEAU_BACKLIGHT is off) */
int
lx_glue_backlight(void *_file, int brightness, int *_level, int *_maximum)
{
	return -ENODEV;
}
