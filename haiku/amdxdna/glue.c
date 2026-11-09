/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Linux side of the Haiku driver for the AMD NPU: starting the DRM
	core and amdxdna, the memory the NPU's buffers come from, powering the
	NPU up and down, and the operations on the device's files the hooks of
	driver.cpp forward. Everything here is called with plain C types
	(glue.h).

	Haiku has no IOMMU driver, so the NPU cannot be given a team's address
	space (PASID) as on Linux. amdxdna has a second mode, made for bringing
	up platforms: every buffer is taken from one range of physical memory,
	the "carveout", and the NPU is given physical addresses. Linux sets the
	range aside at boot; here it is a run of wired pages allocated when the
	NPU is started for the first time, 64 MiB aligned, as the device heap
	(64 MiB, aligned to its size) has to be. */

#include <lx/haiku.h>
#include <lx/vma.h>

#include <drm/drm_accel.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_mm.h>
#include <drm/drm_vma_manager.h>
#include <drm/gpu_scheduler.h>
#include <linux/dma-buf.h>

#include "aie2_pci.h"
#include "amdxdna_cbuf.h"
#include "amdxdna_ctx.h"
#include "amdxdna_gem.h"
#include "amdxdna_pci_drv.h"
#include "amdxdna_pm.h"

#include "glue.h"


/* module initialization functions (lx/device.h's module_init()) */
int lx_module_init_dma_buf_init(void);
int lx_module_init_drm_core_init(void);
void lx_module_exit_drm_core_exit(void);
int lx_module_init_drm_sched_fence_slab_init(void);
void lx_module_exit_drm_sched_fence_slab_fini(void);
int lx_module_init_amdxdna_pci_driver_init(void);

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

/* the NPU, once it was started, and its buffer memory */
static struct pci_dev *npu;
static struct amdxdna_dev *xdna_device;
static bool device_released;
static void *carveout_run;
static unsigned long carveout_pages;
static u64 carveout_address;
/* the device without hardware (lab aid) */
static struct device fake_parent;
static bool fake_started;


/* #pragma mark - parameters */

/*	The memory for the NPU's buffers, in MiB. It holds the device heap (64
	MiB: the instructions and configuration data of the designs loaded) and
	the command and data buffers of all clients; 128 MiB leave 64 MiB for
	those. Taken when the NPU is first started and kept until the driver is
	unloaded: a run this long is found most easily soon after a boot. If
	there is none, smaller ones are tried. */
static unsigned int haiku_carveout_mb = 128;
module_param_named(carveout_mb, haiku_carveout_mb, uint, 0444);

/* the device heap of amdxdna's AIE2 devices: aie2_pci.h's AIE2_DEVM_SIZE */
#define HEAP_BYTES			((u64)SZ_64M)
/* the least that is of use: a heap and 4 MiB of buffers */
#define CARVEOUT_MIN_BYTES	(HEAP_BYTES + (u64)SZ_4M)


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
		pr_info("amdxdna: parameter %s = %s%s\n", name, value,
			error != 0 ? " (not accepted)" : "");
		return error;
	}
	pr_info("amdxdna: no parameter %s\n", name);
	return -ENOENT;
}


/* #pragma mark - start and stop */


int
lx_glue_init(void)
{
	int error;

	/* the first message: opens log_file, if the settings name one */
	pr_info("amdxdna: starting the Linux side\n");
	lx_cpu_init();
	error = lx_workqueue_init();
	if (error != 0)
		return error;

	/* the NPU's buffers are dma-bufs (amdxdna_cbuf.c) */
	error = lx_module_init_dma_buf_init();
	if (error != 0)
		goto err_workqueues;
	error = lx_module_init_drm_core_init();
	if (error != 0)
		goto err_workqueues;
	error = lx_module_init_drm_sched_fence_slab_init();
	if (error != 0)
		goto err_drm;
	error = lx_module_init_amdxdna_pci_driver_init();
	if (error != 0)
		goto err_sched;
	return 0;

err_sched:
	lx_module_exit_drm_sched_fence_slab_fini();
err_drm:
	lx_module_exit_drm_core_exit();
err_workqueues:
	lx_workqueue_exit();
	lx_rcu_exit();
	lx_timer_exit();
	lx_haiku_set_task_exit_hook(NULL);
	lx_haiku_log_file(NULL);
	return error;
}


void
lx_glue_uninit(void)
{
	pr_info("amdxdna: stopping the Linux side\n");
	lx_module_exit_drm_sched_fence_slab_fini();
	lx_module_exit_drm_core_exit();
	lx_workqueue_exit();
	lx_rcu_exit();
	lx_timer_exit();
	pr_info("amdxdna: stopped\n");
	/* no code of this image may run after it is unloaded */
	lx_haiku_set_task_exit_hook(NULL);
	lx_haiku_log_file(NULL);
}


/* #pragma mark - the buffer memory */


static int
carveout_allocate(void)
{
	u64 sizes[4];
	int count = 0, i;

	if (carveout_run != NULL)
		return 0;

	sizes[count++] = max((u64)haiku_carveout_mb << 20, CARVEOUT_MIN_BYTES);
	if (sizes[0] > HEAP_BYTES + (u64)SZ_32M)
		sizes[count++] = HEAP_BYTES + (u64)SZ_32M;
	if (sizes[0] > HEAP_BYTES + (u64)SZ_8M)
		sizes[count++] = HEAP_BYTES + (u64)SZ_8M;
	for (i = 0; i < count; i++) {
		unsigned long long physical;
		ktime_t start = ktime_get();
		void *run = lx_haiku_page_run_alloc(sizes[i] >> PAGE_SHIFT,
			HEAP_BYTES, &physical);
		if (run == NULL) {
			pr_warn("amdxdna: no %llu MiB of contiguous memory free (looked "
				"for %lld ms)\n", sizes[i] >> 20,
				ktime_ms_delta(ktime_get(), start));
			continue;
		}
		carveout_run = run;
		carveout_pages = sizes[i] >> PAGE_SHIFT;
		carveout_address = physical;
		pr_info("amdxdna: buffer memory: %llu MiB at %#llx (found in %lld "
			"ms)\n", sizes[i] >> 20, physical,
			ktime_ms_delta(ktime_get(), start));
		return 0;
	}
	pr_err("amdxdna: no contiguous memory for the NPU's buffers; it is found "
		"most easily soon after a boot\n");
	return -ENOMEM;
}


static void
carveout_free(void)
{
	if (carveout_run == NULL)
		return;
	if (!device_released) {
		/* something still refers to the DRM device, and so may to buffers */
		pr_err("amdxdna: the device is still referenced: %lu MiB of buffer "
			"memory stay reserved\n", carveout_pages >> (20 - PAGE_SHIFT));
		return;
	}
	lx_haiku_page_run_free(carveout_run, carveout_pages);
	carveout_run = NULL;
	carveout_pages = 0;
}


/* the last reference to the DRM device is gone, and with it every buffer */
static void
glue_device_release(struct drm_device *ddev, void *unused)
{
	device_released = true;
}


/* What writing "<size>@<address>" to amdxdna's debugfs file does on Linux.
 * From then on every buffer comes from the range. */
static int
carveout_attach(struct amdxdna_dev *xdna)
{
	int error;

	device_released = false;
	error = drmm_add_action(&xdna->ddev, glue_device_release, NULL);
	if (error != 0)
		return error;
	return amdxdna_carveout_init(xdna, carveout_address,
		(u64)carveout_pages << PAGE_SHIFT);
}


/* #pragma mark - the NPU */


int
lx_glue_start(int bus, int device, int function)
{
	struct pci_driver *driver = lx_pci_registered_driver();
	const struct pci_device_id *id;
	struct pci_dev *pdev;
	int error;

	if (driver == NULL || npu != NULL || fake_started)
		return -ENODEV;
	pdev = lx_pci_dev_get(bus, device, function);
	if (pdev == NULL)
		return -ENODEV;
	id = lx_pci_match(driver, pdev);
	if (id == NULL)
		return -ENODEV;

	/* before anything touches the device: without memory, nothing starts */
	error = carveout_allocate();
	if (error != 0)
		return error;

	pr_info("amdxdna: starting %s (%04x:%04x rev %02x)\n", pci_name(pdev),
		pdev->vendor, pdev->device, pdev->revision);
	pdev->driver = driver;
	/* the firmware is loaded and started, the NPU powered up and asked for
	 * its versions: each step has a timeout */
	error = driver->probe(pdev, id);
	if (error == 0) {
		error = carveout_attach(pci_get_drvdata(pdev));
		if (error != 0 && driver->remove != NULL)
			driver->remove(pdev);
	}
	if (error != 0) {
		pr_err("amdxdna: starting %s failed: %d\n", pci_name(pdev), error);
		/* as Linux's driver core: what the probe left managed goes */
		devres_release_all(&pdev->dev);
		pci_free_irq_vectors(pdev);
		pci_clear_master(pdev);
		pdev->driver = NULL;
		/* the device was never registered, or is unplugged again */
		device_released = true;
		carveout_free();
		return error;
	}
	npu = pdev;
	xdna_device = pci_get_drvdata(pdev);
	pr_info("amdxdna: %s started: firmware %u.%u.%u.%u, %s\n", pci_name(pdev),
		xdna_device->fw_ver.major, xdna_device->fw_ver.minor,
		xdna_device->fw_ver.sub, xdna_device->fw_ver.build,
		xdna_device->vbnv != NULL ? xdna_device->vbnv : "?");
	return 0;
}


/*	Powers the NPU down, as Linux does when no command has run for a while
	(runtime PM) and when the system sleeps: the contexts are taken from the
	firmware, the firmware is stopped and the SMU told to power the NPU off.
	Linux then puts the PCI function into D3hot; here it stays in D0, as
	Haiku left it before the driver started, but without bus mastering. */
int
lx_glue_suspend(void)
{
	int error;

	if (npu == NULL)
		return 0;
	error = amdxdna_pm_suspend(&npu->dev);
	if (error != 0) {
		pr_err("amdxdna: powering the NPU down failed: %d\n", error);
		return error;
	}
	pci_clear_master(npu);
	pr_info("amdxdna: NPU powered down\n");
	return 0;
}


int
lx_glue_resume(void)
{
	int error;

	if (npu == NULL)
		return 0;
	error = amdxdna_pm_resume(&npu->dev);
	if (error != 0) {
		pr_err("amdxdna: powering the NPU up failed: %d\n", error);
		pci_clear_master(npu);
		return error;
	}
	pr_info("amdxdna: NPU powered up\n");
	return 0;
}


static void fake_stop(void);


void
lx_glue_stop(void)
{
	fake_stop();
	if (npu == NULL)
		return;
	pr_info("amdxdna: removing %s\n", pci_name(npu));
	if (npu->driver != NULL && npu->driver->remove != NULL)
		npu->driver->remove(npu);
	devres_release_all(&npu->dev);
	pci_free_irq_vectors(npu);
	pci_clear_master(npu);
	npu->driver = NULL;
	npu = NULL;
	xdna_device = NULL;
	carveout_free();
	pr_info("amdxdna: removed\n");
}


int
lx_glue_location(int *bus, int *device, int *function)
{
	if (npu == NULL)
		return -ENODEV;
	*bus = npu->bus->number;
	*device = PCI_SLOT(npu->devfn);
	*function = PCI_FUNC(npu->devfn);
	return 0;
}


/* #pragma mark - files */


void *
lx_glue_open(int flags, int *_error)
{
	struct drm_device *dev = xdna_device != NULL ? &xdna_device->ddev : NULL;
	struct file *filp;
	struct inode *inode;
	int error;

	if (dev == NULL || dev->accel == NULL) {
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
	inode->i_rdev = MKDEV(ACCEL_MAJOR, dev->accel->index);
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


void
lx_glue_close(void *file)
{
	fput(file);
}


long
lx_glue_ioctl(void *_file, unsigned int cmd, void *arg)
{
	struct file *filp = _file;
	if (filp->f_op->unlocked_ioctl == NULL)
		return -ENOTTY;
	return filp->f_op->unlocked_ioctl(filp, cmd, (unsigned long)arg);
}


/* #pragma mark - mapping buffers */

/*	A buffer mapped into a team (DRM_HAIKU_IOCTL_MAP): its Linux
	vm_area_struct, through which the Haiku driver runs the buffer's fault
	handler to learn the page frames. The buffers are ranges of the carveout
	memory and never move, so nothing has to be unmapped behind a team's
	back (as haiku/amdgpu has to do for buffers TTM moves).

	The mapping keeps the buffer's memory: amdxdna maps a buffer as the
	dma-buf it is made of (amdxdna_cbuf.c), and the mapping holds that
	dma-buf's file. A team that closes its handle with the buffer still
	mapped keeps the range to itself until the mapping ends. */

struct lx_mapping {
	struct vm_area_struct vma;
	struct lx_vma_pages *pages;
	struct file *held;			/* the dma-buf's file, referenced */
	struct mutex fault_lock;	/* faults share pages->pfns */
};


/*	Looks up the buffer at a fake offset (DRM_IOCTL_AMDXDNA_GET_BO_INFO's
	map_offset) and sets up a mapping of it at \a address of the calling
	team; lx_glue_map_fault() then finds the pages. The driver records the
	address of a buffer's first mapping (the device heap has to be mapped
	before buffers can be made in it) and reports it to its clients. */
void *
lx_glue_map(void *_file, unsigned long long offset, unsigned long size,
	int writable, unsigned long address)
{
	struct file *filp = _file;
	struct lx_mapping *mapping;
	unsigned long count = size >> PAGE_SHIFT;
	int error;

	if (size == 0 || (size & ~PAGE_MASK) != 0 || (offset & ~PAGE_MASK) != 0
		|| (address & ~PAGE_MASK) != 0)
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

	mapping->vma.vm_start = address;
	mapping->vma.vm_end = address + size;
	mapping->vma.vm_pgoff = offset >> PAGE_SHIFT;
	mapping->vma.vm_file = filp;
	mapping->vma.vm_mm = current->mm;
	mapping->vma.vm_flags = VM_READ | VM_SHARED | VM_MAYREAD | VM_MAYSHARE
		| (writable ? VM_WRITE | VM_MAYWRITE : 0);
	mapping->vma.vm_page_prot = PAGE_SHARED;

	error = filp->f_op->mmap(filp, &mapping->vma);
	if (error != 0)
		goto err;
	/* Linux's vma_set_file() references the file a mapping is handed over
	 * to; the compat layer's only notes it */
	if (mapping->vma.vm_file != filp && mapping->vma.vm_file != NULL)
		mapping->held = get_file(mapping->vma.vm_file);
	if (mapping->vma.vm_ops == NULL || mapping->vma.vm_ops->fault == NULL) {
		error = -ENODEV;
		goto err_close;
	}
	return mapping;

err_close:
	if (mapping->vma.vm_ops != NULL && mapping->vma.vm_ops->close != NULL)
		mapping->vma.vm_ops->close(&mapping->vma);
	if (mapping->held != NULL)
		fput(mapping->held);
err:
	mutex_destroy(&mapping->fault_lock);
	kvfree(mapping->pages);
	kfree(mapping);
	return ERR_PTR(error);
}


/*	Runs the buffer's fault handler for page \a first of the mapping and the
	ones after it, up to \a max pages or the end of the mapping, and returns
	how many pages from \a first on it found (their page frames in \a pfns,
	and the caching, LX_HAIKU_MAP_*, in \a caching), or a negative error. */
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
	/* ordinary memory, write-back cached like the kernel's own mapping */
	*caching = LX_HAIKU_MAP_WB;
	mutex_unlock(&mapping->fault_lock);
	return result < 0 ? result : (long)found;
}


/*	Ends a mapping (with no lx_glue_map_fault() running). */
void
lx_glue_unmap(void *_mapping)
{
	struct lx_mapping *mapping = _mapping;
	if (mapping->vma.vm_ops != NULL && mapping->vma.vm_ops->close != NULL)
		mapping->vma.vm_ops->close(&mapping->vma);
	if (mapping->held != NULL)
		fput(mapping->held);
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

/*	Without an NPU (settings: "haiku_selftest 1"), for virtual machines and
	before trusting a new build with the hardware: what this driver needs of
	the compat layer beyond what haiku/amdgpu's self test covers. */

#define SELFTEST(condition, what) \
	do { \
		if (!(condition)) { \
			pr_err("amdxdna selftest: FAILED: %s\n", what); \
			failures++; \
		} else \
			pr_info("amdxdna selftest: ok: %s\n", what); \
	} while (0)

static int selftest_released;


static void
selftest_release(struct dma_buf *dmabuf)
{
	selftest_released++;
}


static struct sg_table *
selftest_map(struct dma_buf_attachment *attachment,
	enum dma_data_direction direction)
{
	return ERR_PTR(-ENODEV);
}


static void
selftest_unmap(struct dma_buf_attachment *attachment, struct sg_table *table,
	enum dma_data_direction direction)
{
}


static const struct dma_buf_ops selftest_dmabuf_ops = {
	.map_dma_buf = selftest_map,
	.unmap_dma_buf = selftest_unmap,
	.release = selftest_release,
};


static int
selftest_reader(void *data)
{
	struct srcu_struct *srcu = data;
	int index = srcu_read_lock(srcu);
	msleep(150);
	srcu_read_unlock(srcu, index);
	return 0;
}


int
lx_glue_selftest(void)
{
	DEFINE_DMA_BUF_EXPORT_INFO(export);
	struct srcu_struct srcu = {};
	struct task_struct *reader;
	struct dma_buf *dmabuf;
	unsigned long long physical = 0;
	unsigned long *words;
	struct drm_mm mm;
	struct drm_mm_node heap = {}, small = {};
	ktime_t start;
	void *run, *mapped;
	int failures = 0;

	/* a run as the carveout is, smaller: 2 MiB aligned */
	run = lx_haiku_page_run_alloc(SZ_4M >> PAGE_SHIFT, SZ_2M, &physical);
	SELFTEST(run != NULL && (physical & (SZ_2M - 1)) == 0,
		"aligned run of wired pages");
	if (run != NULL) {
		/* zeroed, and the same memory through memremap() as through the
		 * kernel's map of physical memory */
		mapped = memremap(physical + SZ_1M, PAGE_SIZE, MEMREMAP_WB);
		words = phys_to_virt(physical + SZ_1M);
		SELFTEST(mapped != NULL && words[0] == 0 && words[511] == 0,
			"run zeroed, memremap()");
		if (mapped != NULL) {
			((unsigned long *)mapped)[7] = 0x1234abcd5678ef00UL;
			SELFTEST(words[7] == 0x1234abcd5678ef00UL,
				"memremap() and the physical map agree");
			memunmap(mapped);
		}

		/* the heap's place in it, as amdxdna_cbuf.c looks for it */
		drm_mm_init(&mm, physical, SZ_4M);
		SELFTEST(drm_mm_insert_node_generic(&mm, &small, PAGE_SIZE, 0, 0,
				DRM_MM_INSERT_BEST) == 0
			&& drm_mm_insert_node_generic(&mm, &heap, SZ_2M, SZ_2M, 0,
				DRM_MM_INSERT_BEST) == 0
			&& (heap.start & (SZ_2M - 1)) == 0,
			"aligned range after a small one (drm_mm)");
		if (drm_mm_node_allocated(&heap))
			drm_mm_remove_node(&heap);
		if (drm_mm_node_allocated(&small))
			drm_mm_remove_node(&small);
		drm_mm_takedown(&mm);
		lx_haiku_page_run_free(run, SZ_4M >> PAGE_SHIFT);
	}

	/* a dma-buf: exported into a file of dma-buf's file system, released
	 * with the file's last reference */
	export.ops = &selftest_dmabuf_ops;
	export.size = PAGE_SIZE;
	export.flags = O_RDWR;
	export.priv = &selftest_released;
	dmabuf = dma_buf_export(&export);
	SELFTEST(!IS_ERR(dmabuf) && dmabuf->file != NULL
		&& file_count(dmabuf->file) == 1, "dma_buf_export()");
	if (!IS_ERR(dmabuf)) {
		get_dma_buf(dmabuf);
		dma_buf_put(dmabuf);
		SELFTEST(selftest_released == 0, "dma-buf kept by a reference");
		dma_buf_put(dmabuf);
		SELFTEST(selftest_released == 1, "dma-buf released with its file");
	}

	/* SRCU: a sleeping reader holds up its own srcu_struct, not RCU */
	reader = kthread_run(selftest_reader, &srcu, "selftest srcu");
	msleep(30);
	start = ktime_get();
	synchronize_rcu();
	SELFTEST(!IS_ERR(reader) && ktime_ms_delta(ktime_get(), start) < 60,
		"RCU not held up by an SRCU reader");
	synchronize_srcu(&srcu);
	SELFTEST(ktime_ms_delta(ktime_get(), start) >= 60,
		"synchronize_srcu() waits for the reader");
	if (!IS_ERR(reader))
		kthread_stop(reader);

	SELFTEST(!hypervisor_is_type(X86_HYPER_MS_HYPERV), "no Hyper-V");
	pr_info("amdxdna selftest: running %s a hypervisor\n",
		hypervisor_is_type(X86_HYPER_NATIVE) ? "without" : "under");

	pr_info("amdxdna selftest: %d failure(s)\n", failures);
	return failures;
}


/* #pragma mark - device without hardware */

/*	"haiku_selftest 2": amdxdna's DRM device without an NPU behind it, so
	that everything short of running a design can be tried in a virtual
	machine: opening the node, buffers in the carveout memory and their
	mappings (tests/xdna_test.c), and what a client does around its
	commands - it gets made-up answers about the array and the firmware,
	hardware contexts that hold nothing, and a refusal (ENODEV) for every
	command it submits, which is what a client has to cope with when the
	NPU fails. Made as amdxdna_probe() makes the real device, without the
	hardware's part. */

static int
fake_get_aie_info(struct amdxdna_client *client,
	struct amdxdna_drm_get_info *args)
{
	/* an array like the NPU of Strix and Krackan Point */
	static const struct amdxdna_drm_query_aie_metadata metadata = {
		.col_size = 0x100000,
		.cols = 8,
		.rows = 6,
		.version = { .major = 0, .minor = 0 },
		.core = { .row_count = 4, .row_start = 2, .dma_channel_count = 2,
			.lock_count = 16, .event_reg_count = 4 },
		.mem = { .row_count = 1, .row_start = 1, .dma_channel_count = 6,
			.lock_count = 64, .event_reg_count = 8 },
		.shim = { .row_count = 1, .row_start = 0, .dma_channel_count = 2,
			.lock_count = 16, .event_reg_count = 4 },
	};
	static const struct amdxdna_drm_query_aie_version version = {};
	static const struct amdxdna_drm_query_firmware_version firmware = {};
	const void *answer;
	size_t size;

	switch (args->param) {
	case DRM_AMDXDNA_QUERY_AIE_METADATA:
		answer = &metadata;
		size = sizeof(metadata);
		break;
	case DRM_AMDXDNA_QUERY_AIE_VERSION:
		answer = &version;
		size = sizeof(version);
		break;
	case DRM_AMDXDNA_QUERY_FIRMWARE_VERSION:
		answer = &firmware;
		size = sizeof(firmware);
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (copy_to_user(u64_to_user_ptr(args->buffer), answer,
			min_t(size_t, args->buffer_size, size)))
		return -EFAULT;
	return 0;
}


static int
fake_hwctx_init(struct amdxdna_hwctx *hwctx)
{
	return 0;
}


static void
fake_hwctx_fini(struct amdxdna_hwctx *hwctx)
{
}


/* a compute unit's configuration has to be a buffer of the device heap, as
 * for the real device (aie2_config_cu()) */
static int
fake_hwctx_config(struct amdxdna_hwctx *hwctx, u32 type, u64 value, void *buf,
	u32 size)
{
	struct amdxdna_hwctx_param_config_cu *config = buf;
	struct amdxdna_gem_obj *abo;
	u32 i;

	if (type != DRM_AMDXDNA_HWCTX_CONFIG_CU)
		return -EOPNOTSUPP;
	if (config->num_cus == 0
		|| struct_size(config, cu_configs, config->num_cus) > size)
		return -EINVAL;
	for (i = 0; i < config->num_cus; i++) {
		abo = amdxdna_gem_get_obj(hwctx->client, config->cu_configs[i].cu_bo,
			AMDXDNA_BO_DEV);
		if (abo == NULL)
			return -EINVAL;
		amdxdna_gem_put_obj(abo);
	}
	return 0;
}


static int
fake_cmd_submit(struct amdxdna_hwctx *hwctx, struct amdxdna_sched_job *job,
	u64 *seq)
{
	return -ENODEV;
}


static const struct amdxdna_dev_ops fake_ops = {
	.get_aie_info = fake_get_aie_info,
	.hwctx_init = fake_hwctx_init,
	.hwctx_fini = fake_hwctx_fini,
	.hwctx_config = fake_hwctx_config,
	.cmd_submit = fake_cmd_submit,
};


/* as amdxdna_pci_drv.c's own release action, which amdxdna_probe() adds */
static void
fake_device_release(struct drm_device *ddev, void *xdna)
{
	amdxdna_carveout_fini(xdna);
}

static const struct amdxdna_dev_info fake_info = {
	.dev_mem_buf_shift = 15,
	.dev_mem_base = AIE2_DEVM_BASE,
	.dev_mem_size = AIE2_DEVM_SIZE,
	.dev_heap_max_size = AIE2_DEVM_MAX_SIZE,
	.default_vbnv = "no hardware",
	.device_type = AMDXDNA_DEV_TYPE_KMQ,
	.ops = &fake_ops,
};


int
lx_glue_fake_start(void)
{
	struct amdxdna_dev *xdna;
	struct drm_device *ddev;
	int error;

	if (npu != NULL || fake_started)
		return -EBUSY;
	error = carveout_allocate();
	if (error != 0)
		return error;

	device_initialize(&fake_parent);
	dev_set_name(&fake_parent, "haiku-fake-npu");
	xdna = devm_drm_dev_alloc(&fake_parent, &amdxdna_drm_drv, typeof(*xdna),
		ddev);
	if (IS_ERR(xdna)) {
		error = PTR_ERR(xdna);
		goto err;
	}
	ddev = &xdna->ddev;
	xdna->dev_info = &fake_info;
	xdna->vbnv = fake_info.default_vbnv;
	init_rwsem(&xdna->notifier_lock);
	INIT_LIST_HEAD(&xdna->client_list);
	error = drmm_mutex_init(ddev, &xdna->client_lock);
	if (error == 0)
		error = drmm_mutex_init(ddev, &xdna->dev_lock);
	if (error == 0) {
		xdna->notifier_wq = drmm_alloc_ordered_workqueue(ddev, "notifier_wq",
			WQ_MEM_RECLAIM);
		if (IS_ERR(xdna->notifier_wq))
			error = PTR_ERR(xdna->notifier_wq);
	}
	if (error == 0)
		error = drmm_add_action(ddev, fake_device_release, xdna);
	if (error == 0)
		error = drm_dev_register(ddev, 0);
	if (error == 0) {
		error = carveout_attach(xdna);
		if (error != 0)
			drm_dev_unplug(ddev);
	}
	if (error != 0)
		goto err;

	xdna_device = xdna;
	fake_started = true;
	pr_info("amdxdna selftest: device without hardware registered\n");
	return 0;

err:
	devres_release_all(&fake_parent);
	device_released = true;
	carveout_free();
	return error;
}


static void
fake_stop(void)
{
	struct amdxdna_dev *xdna = xdna_device;

	if (!fake_started)
		return;
	fake_started = false;
	xdna_device = NULL;
	drm_dev_unplug(&xdna->ddev);
	devres_release_all(&fake_parent);
	carveout_free();
	pr_info("amdxdna selftest: device without hardware removed\n");
}
