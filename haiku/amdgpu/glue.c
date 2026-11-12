/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Linux side of the Haiku driver: starting the DRM core and amdgpu,
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

#include "amdgpu.h"
#include "amdgpu_dpm.h"
#include "amdgpu_hmm.h"
#include "amdgpu_object.h"
#include "dc.h"

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
int lx_module_init_amdgpu_init(void);
void lx_module_exit_amdgpu_exit(void);
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


/* the DRM devices: the probed GPUs, and the self test's fake one */
static int
drm_devices(struct drm_device **devices, int max)
{
	int count = 0, i;
	for (i = 0; i < probed_count && count < max; i++) {
		struct drm_device *dev = pci_get_drvdata(probed[i]);
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
		pr_info("amdgpu: parameter %s = %s%s\n", name, value,
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
			pr_info("amdgpu: memory: %s\n", line);
		}
		return error;
	}
	pr_info("amdgpu: no parameter %s\n", name);
	return -ENOENT;
}


/* #pragma mark - start and stop */


int
lx_glue_init(void)
{
	int error;

	/* the first message: opens log_file, if the settings name one */
	pr_info("amdgpu_drm: starting\n");
	lx_cpu_init();
	lx_drm_hotplug_hook = hotplug_event;
	lx_unmap_mapping_range_hook = lx_glue_unmap_mapping_range;
	error = lx_workqueue_init();
	if (error != 0)
		return error;
	/* without it, pages are freed to Haiku at once */
	if (lx_page_pool_init() != 0)
		pr_warn("amdgpu_drm: no page pool\n");

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
	error = lx_module_init_amdgpu_init();
	if (error != 0)
		goto err_sched;
	pr_info("amdgpu_drm: starting: modules initialized\n");
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


/*	Lab aid (settings: "lab_bar_bench 1"): the CPU's bandwidth to the VRAM
	aperture before amdgpu touches the GPU, through write-combined and
	uncached mappings of 4 MB at 128 MB into it, which nothing uses yet (the
	firmware's frame buffer is at its start). */
static int lab_bar_bench;
module_param(lab_bar_bench, int, 0644);

/* Write-combined GTT (amdgpu_bo_support_uswc): the GPU then reads and
 * writes system memory without snooping. Experimental, and NOT safe to turn
 * on yet: user mappings of buffers in several physical ranges stay
 * write-back (Haiku's vm_map_physical_memory_vecs() takes no memory type),
 * so the processor would cache what the GPU accesses past its caches.
 * Measured on the X399 (2026-10-13): GPU writes into system memory are no
 * faster without snooping (1.55 GB/s either way). */
int lx_haiku_uswc;
module_param_named(haiku_uswc, lx_haiku_uswc, int, 0444);

static void
bar_bench(struct pci_dev *pdev, const char *what, bool wc)
{
	const unsigned long offset = 128 << 20, size = 4 << 20, readSize = 64 << 10;
	resource_size_t base = pci_resource_start(pdev, 0);
	volatile u64 *map;
	ktime_t start, written, read;
	u64 sum = 0;
	unsigned long i;

	if (pci_resource_len(pdev, 0) < offset + size)
		return;
	map = wc ? ioremap_wc(base + offset, size) : ioremap(base + offset, size);
	if (map == NULL)
		return;
	start = ktime_get();
	for (i = 0; i < size / 8; i++)
		map[i] = i;
	written = ktime_get();
	for (i = 0; i < readSize / 8; i++)
		sum += map[i];
	read = ktime_get();
	pr_info("lab bar bench %s: aperture %pa, %s: write %llu MB/s, read %llu "
		"MB/s (%s)\n", what, &base, wc ? "write-combined" : "uncached",
		(u64)size / max_t(s64, ktime_to_us(ktime_sub(written, start)), 1),
		(u64)readSize / max_t(s64, ktime_to_us(ktime_sub(read, written)), 1),
		sum == (u64)(readSize / 8) * (readSize / 8 - 1) / 2 ? "ok" : "WRONG");
	iounmap((void __iomem *)map);
}


/*	The lab hook amdgpu_device.c calls after each step of the start: 4 KB
	read from the aperture at 200 MB and 64 KB written back unchanged,
	through a write-combined mapping (amdgpu maps the visible VRAM so: an
	uncached mapping of part of it would conflict, a panic with KDEBUG). */
void lx_lab_ip_hook(struct amdgpu_device *adev, const char *step);

void
lx_lab_ip_hook(struct amdgpu_device *adev, const char *step)
{
	const unsigned long offset = 200 << 20, size = 64 << 10, readSize = 4096;
	resource_size_t base = pci_resource_start(adev->pdev, 0);
	volatile u32 *map;
	ktime_t start, read, written;
	unsigned long i;
	static u32 values[(64 << 10) / 4];

	if (!lab_bar_bench || pci_resource_len(adev->pdev, 0) < offset + size)
		return;
	map = ioremap_wc(base + offset, size);
	if (map == NULL)
		return;
	start = ktime_get();
	for (i = 0; i < readSize / 4; i++)
		values[i] = map[i];
	read = ktime_get();
	for (i = 0; i < size / 4; i++)
		map[i] = values[i % (readSize / 4)] ^ values[i % (readSize / 4)] ^ map[i];
	written = ktime_get();
	iounmap((void __iomem *)map);
	pr_info("lab aperture after %s: read %lld us (4 KB), read+write %lld us "
		"(64 KB)\n", step, ktime_to_us(ktime_sub(read, start)),
		ktime_to_us(ktime_sub(written, read)));
	{
		/* the PCIe link: ASPM control, speed and width, here and upstream */
		struct pci_dev *dev = adev->pdev;
		int level;
		for (level = 0; dev != NULL && level < 3; level++,
				dev = pci_upstream_bridge(dev)) {
			u16 control = 0, status = 0;
			u32 capability = 0;
			pcie_capability_read_word(dev, PCI_EXP_LNKCTL, &control);
			pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &status);
			pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &capability);
			pr_info("lab   %s %04x:%04x: link control %#x (ASPM %s%s), status "
				"%#x (gen %u x%u), capability %#x\n", pci_name(dev),
				dev->vendor, dev->device, control,
				(control & PCI_EXP_LNKCTL_ASPM_L0S) ? "L0s " : "",
				(control & PCI_EXP_LNKCTL_ASPM_L1) ? "L1" : "",
				status, status & PCI_EXP_LNKSTA_CLS,
				(status & PCI_EXP_LNKSTA_NLW) >> 4, capability);
		}
	}
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

	pr_info("amdgpu: probing %s (%04x:%04x rev %02x)\n", pci_name(pdev),
		pdev->vendor, pdev->device, pdev->revision);
	if (lab_bar_bench) {
		bar_bench(pdev, "before amdgpu", true);
		bar_bench(pdev, "before amdgpu", false);
	}
	pdev->driver = driver;
	error = driver->probe(pdev, id);
	if (error != 0) {
		pr_err("amdgpu: probing %s failed: %d\n", pci_name(pdev), error);
		/* as Linux's driver core: what the probe left managed goes */
		devres_release_all(&pdev->dev);
		pdev->driver = NULL;
		return error;
	}
	probed[probed_count++] = pdev;
	if (lab_bar_bench)
		bar_bench(pdev, "after amdgpu", true);
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
			pr_info("amdgpu: shutting down %s\n", pci_name(pdev));
			pdev->driver->shutdown(pdev);
		}
	}
}



/* Suspend to idle (deep sleeps only; "haiku_s2idle 0" turns it off): what Linux does
 * for an APU when the system suspends to idle (amdgpu_pmops_prepare and
 * _suspend with in_s0ix): the display state is saved and the display turned
 * off, the buffers in VRAM go to system memory, GFX, SDMA and the PSP stay
 * where they are (in GFXOFF), the SMU keeps running. The SoC cannot reach
 * its deepest state (S0i3) with the GPU awake. Discrete GPUs are left alone
 * (they would need power removed). */
static int haiku_s2idle = 1;
module_param(haiku_s2idle, int, 0644);

/* The GPU in D3hot for the sleep, as Linux' PCI core leaves it after the
 * driver's suspend (and as the platform's S0i3 constraints ask). The
 * function has no No_Soft_Reset, so its configuration space is saved and
 * written back. */
static int haiku_s2idle_d3 = 1;
module_param(haiku_s2idle_d3, int, 0644);

static bool suspended[MAX_GPUS];
static bool in_d3[MAX_GPUS];
static u32 saved_config[MAX_GPUS][64];

static void
gpu_to_d3(int i)
{
	struct pci_dev *pdev = probed[i];
	u8 pm = pci_find_capability(pdev, PCI_CAP_ID_PM);
	u16 control;
	int j;

	if (!haiku_s2idle_d3 || pm == 0)
		return;
	for (j = 0; j < 64; j++)
		pci_read_config_dword(pdev, j * 4, &saved_config[i][j]);
	pci_read_config_word(pdev, pm + PCI_PM_CTRL, &control);
	pci_write_config_word(pdev, pm + PCI_PM_CTRL,
		(control & ~PCI_PM_CTRL_STATE_MASK) | PCI_D3hot);
	msleep(10);
	in_d3[i] = true;
	pci_read_config_word(pdev, pm + PCI_PM_CTRL, &control);
	pr_info("amdgpu: %s in D3hot (pmcsr %#x)\n", pci_name(pdev), control);
}

static void
gpu_to_d0(int i)
{
	struct pci_dev *pdev = probed[i];
	u8 pm = pci_find_capability(pdev, PCI_CAP_ID_PM);
	u16 control;
	u32 value;
	int j;

	if (!in_d3[i])
		return;
	in_d3[i] = false;
	pci_read_config_word(pdev, pm + PCI_PM_CTRL, &control);
	pci_write_config_word(pdev, pm + PCI_PM_CTRL,
		control & ~PCI_PM_CTRL_STATE_MASK);
	msleep(10);

	/* back to front, as Linux' pci_restore_config_space(): the BARs before
	 * the command register; the PM control register stays as it is now */
	for (j = 63; j >= 1; j--) {
		if (j * 4 == ((pm + PCI_PM_CTRL) & ~3))
			continue;
		pci_read_config_dword(pdev, j * 4, &value);
		if (value != saved_config[i][j])
			pci_write_config_dword(pdev, j * 4, saved_config[i][j]);
	}
	pci_read_config_word(pdev, pm + PCI_PM_CTRL, &control);
	pci_read_config_dword(pdev, PCI_BASE_ADDRESS_0, &value);
	pr_info("amdgpu: %s in D0 (pmcsr %#x, bar0 %#x)\n", pci_name(pdev),
		control, value);
}

int
lx_glue_suspend(void)
{
	int i, count = 0;

	if (!haiku_s2idle)
		return 0;

	for (i = 0; i < probed_count; i++) {
		struct drm_device *dev = pci_get_drvdata(probed[i]);
		struct amdgpu_device *adev;
		int r;

		if (dev == NULL)
			continue;
		adev = drm_to_adev(dev);
		if (!(adev->flags & AMD_IS_APU))
			continue;

		pr_info("amdgpu: suspending %s to idle\n", pci_name(probed[i]));
		adev->in_s0ix = true;
		r = amdgpu_device_prepare(dev);
		if (r == 0) {
			r = amdgpu_device_suspend(dev, true);
			if (r != 0)
				amdgpu_device_resume(dev, true);
		}
		if (r != 0) {
			pr_err("amdgpu: suspending %s failed: %d\n", pci_name(probed[i]),
				r);
			adev->in_s0ix = false;
			amdgpu_device_complete(dev);
			lx_glue_resume();
			return r;
		}
		suspended[i] = true;
		gpu_to_d3(i);
		count++;
	}
	return count;
}


void
lx_glue_resume(void)
{
	int i;
	for (i = 0; i < probed_count; i++) {
		struct drm_device *dev = pci_get_drvdata(probed[i]);
		struct amdgpu_device *adev;
		int r;

		if (!suspended[i] || dev == NULL)
			continue;
		suspended[i] = false;
		adev = drm_to_adev(dev);
		gpu_to_d0(i);

		pr_info("amdgpu: resuming %s\n", pci_name(probed[i]));
		r = amdgpu_device_resume(dev, true);
		if (r != 0)
			pr_err("amdgpu: resuming %s failed: %d\n", pci_name(probed[i]), r);
		adev->in_s0ix = false;
		amdgpu_device_complete(dev);
	}
}

void lx_glue_fake_device_stop(void);

void
lx_glue_uninit(void)
{
	/* each step logged: release builds only stop when devfs reloads the
	 * driver (its binary changed), the one time this path runs */
	pr_info("amdgpu_drm: stopping\n");
	if (watchdog_thread != NULL) {
		kthread_stop(watchdog_thread);
		watchdog_thread = NULL;
	}
	/* amdgpu's mapping of the VRAM goes away */
	if (console_base != NULL) {
		lx_haiku_debug_console(NULL, 0, 0, 0);
		console_base = NULL;
	}
	console_device = NULL;
	lx_glue_fake_device_stop();
	while (probed_count > 0) {
		struct pci_dev *pdev = probed[--probed_count];
		pr_info("amdgpu_drm: stopping: removing %s\n", pci_name(pdev));
		if (pdev->driver != NULL && pdev->driver->remove != NULL)
			pdev->driver->remove(pdev);
		pr_info("amdgpu_drm: stopping: removed, releasing its resources\n");
		devres_release_all(&pdev->dev);
		pdev->driver = NULL;
		pr_info("amdgpu_drm: stopping: released\n");
	}
	pr_info("amdgpu_drm: stopping: module exits\n");
	lx_module_exit_amdgpu_exit();
	lx_module_exit_drm_sched_fence_slab_fini();
	lx_module_exit_drm_display_helper_module_exit();
	lx_module_exit_gpu_buddy_module_exit();
	lx_module_exit_drm_core_exit();
	pr_info("amdgpu_drm: stopping: page pool, workqueues, RCU, timers\n");
	lx_page_pool_exit();
	lx_workqueue_exit();
	lx_rcu_exit();
	lx_timer_exit();
	pr_info("amdgpu_drm: stopped\n");
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
		struct drm_device *dev = pci_get_drvdata(probed[i]);
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

/* Lab aid for hangs that stop the whole machine: "haiku_watchdog_ms 1000"
 * (settings, or drmparam with log_file set) logs once a period how many
 * interrupts came and were handled, the IH ring's read pointer, the RCU
 * backlog, and the rings with work outstanding (completed/emitted fence) -
 * the last lines before the machine stopped say what the GPU was doing.
 * When the interrupt handler has been running for haiku_watchdog_slow_ms
 * (10), it also logs what the handler's thread is doing: its state and,
 * blocked, its stack, the lock it waits for and that lock's owner (and
 * what the owner waits for), every 50 ms while that lasts. */
static int haiku_watchdog_ms;
module_param(haiku_watchdog_ms, int, 0644);
static int haiku_watchdog_slow_ms = 10;
module_param(haiku_watchdog_slow_ms, int, 0644);

/* "drmparam haiku_fence_report 1": the scheduler fences that are alive, by
 * scheduler - how many have finished (signaled), have been scheduled, have
 * a hardware fence and whether that has signaled, how old they are, how
 * many references they have and which DRM clients (negative: the kernel's
 * jobs) made them; then the files open, the buffers TTM has and the jobs
 * the schedulers hold. Fences that live on after their work is done have a
 * holder that does not let go. Only fences made while "haiku_track_objects
 * 1" (compat/src/mm.c) is set are seen. */
static int haiku_fence_report;
module_param(haiku_fence_report, int, 0644);

struct fence_report_row {
	struct drm_gpu_scheduler *sched;
	unsigned long count, finished, scheduled, parent, parent_signaled;
	unsigned long older_10s, older_60s, refs[4], scheduled_refs[3];
	u64 clients[4];
	unsigned long client_counts[4], other_clients;
};

struct fence_report {
	struct fence_report_row rows[24];
	int row_count;
	unsigned long other;
};


static void
fence_report_one(void *object, s64 age_ns, void *data)
{
	struct fence_report *report = data;
	struct drm_sched_fence *fence = object;
	struct fence_report_row *row = NULL;
	unsigned int refs;
	int i;

	for (i = 0; i < report->row_count; i++) {
		if (report->rows[i].sched == fence->sched)
			row = &report->rows[i];
	}
	if (row == NULL) {
		if (report->row_count == ARRAY_SIZE(report->rows)) {
			report->other++;
			return;
		}
		row = &report->rows[report->row_count++];
		row->sched = fence->sched;
	}
	row->count++;
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->finished.flags))
		row->finished++;
	if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->scheduled.flags))
		row->scheduled++;
	if (fence->parent != NULL) {
		row->parent++;
		if (test_bit(DMA_FENCE_FLAG_SIGNALED_BIT, &fence->parent->flags))
			row->parent_signaled++;
	}
	if (age_ns > 10 * NSEC_PER_SEC)
		row->older_10s++;
	if (age_ns > 60 * NSEC_PER_SEC)
		row->older_60s++;
	refs = kref_read(&fence->finished.refcount);
	row->refs[min(refs, 3u)]++;
	/* the scheduled fence holds a reference to the finished one */
	refs = kref_read(&fence->scheduled.refcount);
	row->scheduled_refs[min(refs, 2u)]++;
	for (i = 0; i < 4; i++) {
		if (row->client_counts[i] == 0 || row->clients[i] == fence->drm_client_id) {
			row->clients[i] = fence->drm_client_id;
			row->client_counts[i]++;
			return;
		}
	}
	row->other_clients++;
}


/* the buffers TTM has in VRAM, GTT and system memory, how many of them wait
 * to be deleted, and the fences their reservations hold */
static void
lx_glue_buffer_report(struct amdgpu_device *adev)
{
	static const struct {
		int type;
		const char *name;
	} types[] = {
		{ TTM_PL_VRAM, "vram" }, { TTM_PL_TT, "gtt" }, { TTM_PL_SYSTEM, "system" },
	};
	struct ttm_device *bdev = &adev->mman.bdev;
	unsigned int t;

	for (t = 0; t < ARRAY_SIZE(types); t++) {
		struct ttm_resource_manager *man = ttm_manager_type(bdev, types[t].type);
		struct ttm_resource_cursor cursor;
		struct ttm_resource *res;
		unsigned long buffers = 0, deleted = 0, fences = 0, sched_fences = 0;
		unsigned long deleted_fences = 0, most = 0;
		if (man == NULL)
			continue;
		spin_lock(&bdev->lru_lock);
		ttm_resource_cursor_init(&cursor, man);
		ttm_resource_manager_for_each_res(&cursor, res) {
			struct ttm_buffer_object *bo = res->bo;
			struct dma_resv_iter iter;
			struct dma_fence *fence;
			unsigned long count = 0;
			if (bo == NULL)
				continue;
			buffers++;
			if (bo->deleted)
				deleted++;
			dma_resv_iter_begin(&iter, bo->base.resv, DMA_RESV_USAGE_BOOKKEEP);
			dma_resv_for_each_fence_unlocked(&iter, fence) {
				count++;
				if (to_drm_sched_fence(fence) != NULL)
					sched_fences++;
			}
			dma_resv_iter_end(&iter);
			fences += count;
			most = max(most, count);
			if (bo->deleted)
				deleted_fences += count;
		}
		ttm_resource_cursor_fini(&cursor);
		spin_unlock(&bdev->lru_lock);
		pr_info("fences: %s: %lu buffers (%lu waiting to be deleted), %lu "
			"fences in their reservations (%lu of schedulers, %lu of deleted "
			"buffers, at most %lu in one)\n", types[t].name, buffers, deleted,
			fences, sched_fences, deleted_fences, most);
	}
}


/* the jobs each ring's scheduler still has: done ones wait for free_job */
static void
lx_glue_job_report(struct amdgpu_device *adev)
{
	int r;
	for (r = 0; r < AMDGPU_MAX_RINGS; r++) {
		struct amdgpu_ring *ring = adev->rings[r];
		struct drm_gpu_scheduler *sched;
		struct drm_sched_job *job;
		unsigned long pending = 0, done = 0;
		/* the KIQ and others have no scheduler, but sched.ready is set
		 * once their ring test passed: their lists are zeroes */
		if (ring == NULL || ring->no_scheduler || !ring->sched.ready
			|| ring->sched.ops == NULL || ring->sched.pending_list.next == NULL)
			continue;
		sched = &ring->sched;
		spin_lock(&sched->job_list_lock);
		list_for_each_entry(job, &sched->pending_list, list) {
			pending++;
			if (job->s_fence != NULL && test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
					&job->s_fence->finished.flags))
				done++;
		}
		spin_unlock(&sched->job_list_lock);
		if (pending > 0) {
			pr_info("fences: %s: %lu jobs pending, %lu of them done\n",
				ring->name, pending, done);
		}
	}
}


static void
lx_glue_fence_report(void)
{
	struct fence_report *report = kzalloc(sizeof(*report), GFP_KERNEL);
	int i;
	if (report == NULL)
		return;
	lx_kmem_cache_for_each("drm_sched_fence", fence_report_one, report);
	for (i = 0; i < report->row_count; i++) {
		struct fence_report_row *row = &report->rows[i];
		pr_info("fences: %s: %lu alive, %lu finished, %lu scheduled, %lu with "
			"a hardware fence (%lu signaled), %lu older than 10 s, %lu than "
			"60 s, references 0/1/2/3+: %lu/%lu/%lu/%lu, scheduled fence "
			"references 0/1/2+: %lu/%lu/%lu, clients %llu:%lu %llu:%lu %llu:%lu "
			"%llu:%lu +%lu\n",
			row->sched != NULL ? row->sched->name : "(no scheduler)",
			row->count, row->finished, row->scheduled, row->parent,
			row->parent_signaled, row->older_10s, row->older_60s,
			row->refs[0], row->refs[1], row->refs[2], row->refs[3],
			row->scheduled_refs[0], row->scheduled_refs[1],
			row->scheduled_refs[2],
			(unsigned long long)row->clients[0], row->client_counts[0],
			(unsigned long long)row->clients[1], row->client_counts[1],
			(unsigned long long)row->clients[2], row->client_counts[2],
			(unsigned long long)row->clients[3], row->client_counts[3],
			row->other_clients);
	}
	if (report->other > 0)
		pr_info("fences: %lu more of other schedulers\n", report->other);
	kfree(report);

	/* the files open on the GPUs, with their contexts and buffer handles:
	 * a file a team left behind keeps its contexts' last fences */
	for (i = 0; i < probed_count; i++) {
		struct drm_device *dev = pci_get_drvdata(probed[i]);
		struct drm_file *file;
		int files = 0, contexts = 0, handles = 0, id;
		void *entry;
		if (dev == NULL)
			continue;
		mutex_lock(&dev->filelist_mutex);
		list_for_each_entry(file, &dev->filelist, lhead) {
			struct amdgpu_fpriv *fpriv = file->driver_priv;
			files++;
			if (fpriv != NULL) {
				idr_for_each_entry(&fpriv->ctx_mgr.ctx_handles, entry, id)
					contexts++;
			}
			spin_lock(&file->table_lock);
			idr_for_each_entry(&file->object_idr, entry, id)
				handles++;
			spin_unlock(&file->table_lock);
		}
		mutex_unlock(&dev->filelist_mutex);
		pr_info("fences: %d files open, %d contexts, %d buffer handles\n",
			files, contexts, handles);
		lx_glue_buffer_report(drm_to_adev(dev));
		lx_glue_job_report(drm_to_adev(dev));
	}
}


static void
lx_glue_watchdog_report(long irq_thread, u64 running_ms)
{
	long thread = irq_thread;
	int depth;

	pr_info("watchdog: handler running for %llu ms\n",
		(unsigned long long)running_ms);
	lx_haiku_log_thread(irq_thread, "handler");
	for (depth = 0; depth < 3; depth++) {
		struct lx_lock *lock = lx_lock_waited_for(thread);
		long owner;
		if (lock == NULL)
			break;
		owner = READ_ONCE(lock->owner);
		pr_info("  waits for lock %s (%#lx), held by thread %ld, locked at "
			"%#lx\n", lock->name != NULL ? lock->name : "?",
			(unsigned long)lock, owner,
			(unsigned long)READ_ONCE(lock->owner_site));
		if (owner == 0 || owner == thread)
			break;
		lx_haiku_log_thread(owner, "owner");
		thread = owner;
	}
}


static int
lx_glue_watchdog(void *unused)
{
	u64 last_line = 0;
	int reports = 0;
	bool incident = false;

	while (!kthread_should_stop()) {
		int period = READ_ONCE(haiku_watchdog_ms), i;
		bool slow = false;
		u64 now;
		if (period <= 0) {
			msleep(500);
			continue;
		}
		msleep(incident ? 50 : period);
		now = ktime_get_ns();
		for (i = 0; i < probed_count; i++) {
			struct drm_device *dev = pci_get_drvdata(probed[i]);
			struct amdgpu_device *adev;
			unsigned long arrived, handled;
			u64 running, running_ms;
			long irq_thread;
			char line[640];
			int n, r, slow_ms = READ_ONCE(haiku_watchdog_slow_ms);

			if (dev == NULL)
				continue;
			adev = drm_to_adev(dev);
			lx_irq_stats(dev, &arrived, &handled, &running, &irq_thread);
			running_ms = running / 1000000;
			if (slow_ms > 0 && running_ms >= (u64)slow_ms) {
				slow = true;
				if (reports++ < 40)
					lx_glue_watchdog_report(irq_thread, running_ms);
			}
			if (now - last_line < (u64)period * 1000000 * 9 / 10)
				continue;
			n = scnprintf(line, sizeof(line),
				"watchdog: irq %lu/%lu running %llu ms ih %u rcu %lu", arrived,
				handled, (unsigned long long)running_ms,
				adev->irq.ih.rptr, lx_rcu_backlog());
			for (r = 0; r < AMDGPU_MAX_RINGS && n < (int)sizeof(line) - 48;
					r++) {
				struct amdgpu_ring *ring = adev->rings[r];
				u32 signaled, emitted;
				if (ring == NULL || !ring->sched.ready)
					continue;
				signaled = atomic_read(&ring->fence_drv.last_seq);
				emitted = ring->fence_drv.sync_seq;
				if (signaled != emitted) {
					n += scnprintf(line + n, sizeof(line) - n, " %s %u/%u",
						ring->name, signaled, emitted);
				}
			}
			pr_info("%s\n", line);
			n = lx_mm_stats(line, sizeof(line));
			n += scnprintf(line + n, sizeof(line) - n, " | ");
			n += lx_haiku_resource_stats(line + n, sizeof(line) - n);
			n += scnprintf(line + n, sizeof(line) - n, " | sites");
			lx_kmalloc_top(line + n, sizeof(line) - n, 4);
			pr_info("watchdog: %s\n", line);
		}
		if (now - last_line >= (u64)period * 1000000 * 9 / 10)
			last_line = now;
		if (!slow)
			reports = 0;
		incident = slow;
	}
	return 0;
}


static void
lx_glue_watchdog_start(void)
{
	struct task_struct *thread;
	if (READ_ONCE(haiku_watchdog_ms) <= 0 || watchdog_thread != NULL)
		return;
	thread = kthread_run(lx_glue_watchdog, NULL, "amdgpu watchdog");
	if (!IS_ERR(thread))
		watchdog_thread = thread;
}


/* #pragma mark - the kernel debugger's console */

/* Haiku draws the kernel debugger into the frame buffer the boot loader
 * found (the firmware's), which the display engine no longer shows once a
 * mode is set; app_server reports its own mapping of the new one, a user
 * address the kernel turns down. So after every mode set, the part of a frame
 * buffer the screen at the desktop's top left shows becomes the console,
 * through amdgpu's kernel mapping of the visible VRAM. */
/* "haiku_console_connector DP-2" (settings or drmparam) puts the console on
 * that connector's screen, e.g. one a KVM captures, instead of the one at the
 * desktop's top left */
static char *haiku_console_connector;
module_param(haiku_console_connector, charp, 0644);

/* lab aid: "haiku_console_test 1" (drmparam) paints a mark where the
 * console is, to check that it is on screen */
static int haiku_console_test;
module_param(haiku_console_test, int, 0644);

static void
lx_glue_update_debug_console(struct drm_device *dev)
{
	struct amdgpu_device *adev = drm_to_adev(dev);
	struct drm_modeset_acquire_ctx ctx;
	struct drm_plane *plane;
	void *base = NULL;
	int width = 0, height = 0, pitch = 0, best = INT_MAX;
	int ret;

	if (adev->mman.aper_base_kaddr == NULL)
		return;
	console_device = dev;
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
			struct amdgpu_bo *abo;
			u64 offset;
			int x, y, w, h, distance;

			if (plane->type != DRM_PLANE_TYPE_PRIMARY || state == NULL
				|| state->fb == NULL || state->crtc == NULL
				|| !state->crtc->state->active)
				continue;
			fb = state->fb;
			if (fb->format->cpp[0] != 4 || fb->obj[0] == NULL)
				continue;
			abo = gem_to_amdgpu_bo(fb->obj[0]);
			if (abo->tbo.pin_count == 0 || abo->tbo.resource == NULL
				|| abo->tbo.resource->mem_type != TTM_PL_VRAM)
				continue;
			x = state->src_x >> 16;
			y = state->src_y >> 16;
			w = state->src_w >> 16;
			h = state->src_h >> 16;
			offset = amdgpu_bo_gpu_offset(abo) - adev->gmc.vram_start
				+ fb->offsets[0] + (u64)y * fb->pitches[0] + (u64)x * 4;
			if (w <= 0 || h <= 0 || offset + (u64)(h - 1) * fb->pitches[0]
					+ (u64)w * 4 > adev->gmc.visible_vram_size)
				continue;
			distance = x + y;
			if (haiku_console_connector != NULL && haiku_console_connector[0]) {
				/* the named connector's screen comes first */
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
			base = (u8 *)adev->mman.aper_base_kaddr + offset;
			width = w;
			height = h;
			pitch = fb->pitches[0];
		}
		if (base != console_base || width != console_width
			|| height != console_height || pitch != console_pitch) {
			console_base = base;
			console_width = width;
			console_height = height;
			console_pitch = pitch;
			lx_haiku_debug_console(base, width, height, pitch);
			pr_info("amdgpu_drm: kernel debugger console %dx%d\n", width,
				height);
		}
	}
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
}


/* a changed haiku_console_connector is applied at once, on the device the
 * console was last set up for */
static void
lx_glue_console_moved(void)
{
	if (console_device != NULL)
		lx_glue_update_debug_console(console_device);
}


static void
lx_glue_console_test(void)
{
	u8 *row = console_base;
	int x, y;

	if (row == NULL || console_width < 512 || console_height < 64)
		return;
	for (y = 0; y < 64; y++, row += console_pitch) {
		u32 *pixel = (u32 *)row;
		for (x = 0; x < 512; x++)
			pixel[x] = ((x / 32 + y / 32) & 1) ? 0xffff00ff : 0xff00ffff;
	}
	pr_info("amdgpu_drm: marked the kernel debugger console\n");
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


/*	Lab aid: forces a DPM performance level (AMD_DPM_FORCED_LEVEL_*), as
	Linux's power_dpm_force_performance_level file does. */
int
lx_glue_lab_perf_level(void *_file, unsigned int level)
{
	struct file *filp = _file;
	struct drm_file *file = filp->private_data;
	struct amdgpu_device *adev = drm_to_adev(file->minor->dev);
	int r = amdgpu_dpm_force_performance_level(adev, level);
	pr_info("lab: performance level %#x: %d\n", level, r);
	return r;
}


/*	Lab aid: the CPU's bandwidth to a VRAM buffer through the kernel's
	mapping of it (amdgpu_bo_kmap(): write-combined). */
int
lx_glue_lab_bench(void *_file)
{
	struct file *filp = _file;
	struct drm_file *file = filp->private_data;
	struct amdgpu_device *adev = drm_to_adev(file->minor->dev);
	const unsigned long size = 16 << 20, readSize = 256 << 10;
	struct amdgpu_bo *bo = NULL;
	u64 gpuAddress, sum = 0;
	volatile u64 *cpu = NULL;
	ktime_t start, written, read;
	unsigned long i;
	int r = amdgpu_bo_create_kernel(adev, size, PAGE_SIZE,
		AMDGPU_GEM_DOMAIN_VRAM, &bo, &gpuAddress, (void **)&cpu);
	if (r != 0)
		return r;
	start = ktime_get();
	for (i = 0; i < size / 8; i++)
		cpu[i] = i;
	written = ktime_get();
	for (i = 0; i < readSize / 8; i++)
		sum += cpu[i];
	read = ktime_get();
	pr_info("lab bench: VRAM at %#llx, kernel mapping %px: write %llu MB/s, "
		"read %llu MB/s (%s)\n", gpuAddress, cpu,
		(u64)size * 1000 / max_t(s64, ktime_to_us(ktime_sub(written, start)), 1) / 1000,
		(u64)readSize * 1000 / max_t(s64, ktime_to_us(ktime_sub(read, written)), 1) / 1000,
		sum == (u64)(readSize / 8) * (readSize / 8 - 1) / 2 ? "ok" : "WRONG");
	amdgpu_bo_free_kernel(&bo, &gpuAddress, (void **)&cpu);
	return 0;
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
	pr_info("amdgpu_drm: connector %s forced %s: %s\n", connector->name,
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
			pr_err("amdgpu selftest: FAILED: %s\n", what); \
			failures++; \
		} else \
			pr_info("amdgpu selftest: ok: %s\n", what); \
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
		pr_info("amdgpu selftest: 256 pages allocated in %llu us, freed in "
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
			pr_info("amdgpu selftest: pool round %d: 256 pages allocated in "
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
				pr_info("amdgpu selftest: run round %d: taken in %llu us, freed "
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
			pr_info("amdgpu selftest: %s\n", line);
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

	pr_info("amdgpu selftest: %d failure(s)\n", failures);
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
	pr_info("amdgpu selftest: fake DRM device registered\n");
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


/* DRM_HAIKU_IOCTL_CRC: the display controller's CRC of the frames a CRTC
 * scans out (DC's stream CRC, as amdgpu_dm_crc.c uses for debugfs, which
 * this build does without). op 1 starts it (continuous), 2 reads the last
 * complete frame's, 0 stops it. */
int
lx_glue_crc(void *_file, unsigned crtc_index, int op, unsigned *crc,
	unsigned *_frame, const unsigned short *window)
{
	struct crc_params params = {}, *window_params = NULL;
	struct file *filp = _file;
	struct drm_file *file = filp->private_data;
	struct drm_device *dev = file->minor->dev;
	struct amdgpu_device *adev = drm_to_adev(dev);
	struct drm_crtc *crtc = NULL, *each;
	struct dm_crtc_state *crtc_state;
	struct dc_stream_state *stream;
	uint32_t r, g, b;
	bool ok;

	drm_for_each_crtc(each, dev) {
		if (drm_crtc_index(each) == crtc_index) {
			crtc = each;
			break;
		}
	}
	if (crtc == NULL || crtc->state == NULL)
		return -ENODEV;
	crtc_state = to_dm_crtc_state(crtc->state);
	stream = crtc_state->stream;
	if (stream == NULL)
		return -ENODEV;

	if (op == 2) {
		if (!dc_stream_get_crc(stream->ctx->dc, stream, 0, &r, &g, &b))
			return -EIO;
		crc[0] = r;
		crc[1] = g;
		crc[2] = b;
		*_frame = drm_crtc_vblank_count(crtc);
		return 0;
	}

	if (op == 1 && (window[2] != 0 || window[3] != 0)) {
		params.windowa_x_start = params.windowb_x_start = window[0];
		params.windowa_y_start = params.windowb_y_start = window[1];
		params.windowa_x_end = params.windowb_x_end = window[2];
		params.windowa_y_end = params.windowb_y_end = window[3];
		window_params = &params;
	}
	mutex_lock(&adev->dm.dc_lock);
	ok = dc_stream_configure_crc(stream->ctx->dc, stream, window_params,
		op == 1, op == 1, 0, true);
	/* Temporal dithering changes the pixels every frame; as amdgpu_dm's
	 * CRC capture, truncate instead while the CRC runs */
	if (op == 1) {
		dc_stream_set_dither_option(stream, DITHER_OPTION_TRUN8);
		dc_stream_set_dyn_expansion(stream->ctx->dc, stream,
			DYN_EXPANSION_DISABLE);
	} else {
		dc_stream_set_dither_option(stream, DITHER_OPTION_DEFAULT);
		dc_stream_set_dyn_expansion(stream->ctx->dc, stream,
			DYN_EXPANSION_AUTO);
	}
	mutex_unlock(&adev->dm.dc_lock);
	return ok ? 0 : -EINVAL;
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


/* DRM_HAIKU_IOCTL_BACKLIGHT: what writing /sys/class/backlight/amdgpu_bl<n>/
 * brightness does, for the first built-in panel of the file's GPU; a
 * negative brightness only asks. */
int
lx_glue_backlight(void *_file, int brightness, int *_level, int *_maximum)
{
	struct file *filp = _file;
	struct drm_file *file = filp->private_data;
	struct amdgpu_device *adev = drm_to_adev(file->minor->dev);
	struct backlight_device *bd = NULL;
	int i, error = 0;

	for (i = 0; i < AMDGPU_DM_MAX_NUM_EDP && bd == NULL; i++)
		bd = adev->dm.backlight_dev[i];
	if (bd == NULL)
		return -ENODEV;

	if (brightness >= 0)
		error = backlight_device_set_brightness(bd, brightness);
	/* not "current": Linux's macro for the running task */
	*_level = bd->props.brightness;
	*_maximum = bd->props.max_brightness;
	return error;
}
