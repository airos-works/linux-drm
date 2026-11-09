/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_ACCEL_H
#define _LX_ACCEL_H

/*	What the AMD NPU driver (drivers/accel/amdxdna, from a newer Linux than
	the DRM core) needs beyond what the GPU drivers use.

	Haiku has no IOMMU driver. So everything about I/O virtual addresses,
	shared virtual addressing (PASID) and following a process's page tables
	(HMM, MMU notifiers) is declared here and fails or does nothing; the
	driver then runs in its mode for physically contiguous buffers at
	physical addresses (amdxdna_cbuf.c, see haiku/amdxdna). */


/* #pragma mark - small things */

#ifndef ____cacheline_aligned_in_smp
#	define ____cacheline_aligned_in_smp	__attribute__((__aligned__(64)))
#endif
#define COMPLETION_INITIALIZER_ONSTACK(work) \
	(*({ init_completion(&(work)); &(work); }))
#define kvmalloc_objs(p, n, ...) \
	((typeof(p) *)kvmalloc_array(n, sizeof(typeof(p)), \
		lx_default_gfp(__VA_ARGS__)))
#define linear_page_delta(vma, address) \
	((pgoff_t)(((address) - (vma)->vm_start) >> PAGE_SHIFT))

/* X86_HYPER_NATIVE is what a machine that is not a virtual one runs on
 * (lx/mm.h says no to every type, which suits the GPU drivers' questions) */
#undef hypervisor_is_type
#define hypervisor_is_type(type) \
	((type) == X86_HYPER_NATIVE && !boot_cpu_has(X86_FEATURE_HYPERVISOR))

/* a PCI driver's registration, as a module initialization function
 * (lx_module_init_<driver>_init) */
#ifdef LX_ADDON_AMDXDNA
#undef module_pci_driver
#define module_pci_driver(driver) \
	static int driver##_init(void) \
	{ \
		return pci_register_driver(&driver); \
	} \
	module_init(driver##_init)
#endif

/* memory locked by a process: no limit on Haiku */
#define RLIMIT_MEMLOCK		8
#define rlimit(resource)	(~0UL)
#define can_do_mlock()		true

/* pages of ordinary memory mapped at once (amdxdna: buffers from shmem
 * files, which Haiku does not have) */
#define vm_insert_pages(vma, addr, pages, num)	(-EOPNOTSUPP)

/* the BARs of a PCI device, mapped for as long as the driver has it */
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);

/* Pages that are contiguous in physical memory and freed with the device's
 * resources. (Memory of the kernel heap, which kmalloc() returns, is only
 * contiguous within a page on Haiku.) */
unsigned long devm_get_free_pages(struct device *dev, gfp_t gfp,
	unsigned int order);


/* #pragma mark - MSI-X */

/*	As many vectors as asked for (Haiku: 32 at most), their interrupts
	numbered from pci_irq_vector(dev, 0) on, for a driver that asks for
	PCI_IRQ_MSIX only. They are configured when the driver first asks for a
	vector's number, and the table is checked whenever it asks again:
	compat/src/pci.c says why. (Declared in lx/pci.h.) */


/* #pragma mark - no IOMMU */

struct iommu_group;
struct iommu_sva;
struct iova {
	unsigned long pfn_hi;
	unsigned long pfn_lo;
};
struct iova_domain {
	unsigned long granule;
};
/* struct iommu_domain is lx/mm.h's: nothing here ever has one */
#define IOMMU_PASID_INVALID		(-1U)
#define IOMMU_READ				(1 << 0)
#define IOMMU_WRITE				(1 << 1)
#define IOMMU_HWPT_ALLOC_PASID	(1 << 3)

#define iommu_group_get(dev)			((struct iommu_group *)NULL)
#define iommu_group_put(group)			do { (void)(group); } while (0)
#define iommu_sva_bind_device(dev, mm)	((struct iommu_sva *)ERR_PTR(-ENODEV))
#define iommu_sva_unbind_device(sva)	do { (void)(sva); } while (0)
#define iommu_sva_get_pasid(sva)		IOMMU_PASID_INVALID
#define iommu_paging_domain_alloc_flags(dev, flags) \
	((struct iommu_domain *)ERR_PTR(-ENODEV))
#define iommu_domain_free(domain)		do { (void)(domain); } while (0)
#define iommu_attach_group(domain, group)	(-ENODEV)
#define iommu_detach_group(domain, group)	do { } while (0)
#define iommu_map(domain, iova, paddr, size, prot, gfp)	(-ENODEV)
#define iommu_unmap(domain, iova, size)	((size_t)0)
#define iommu_map_sgtable(domain, iova, sgt, prot)	((ssize_t)-ENODEV)

#define iova_cache_get()				0
#define iova_cache_put()				do { } while (0)
#define init_iova_domain(iovad, granule_, start) \
	do { (iovad)->granule = (granule_); } while (0)
#define put_iova_domain(iovad)			do { } while (0)
#define alloc_iova(iovad, size, limit, aligned)	((struct iova *)NULL)
#define free_iova(iovad, pfn)			do { } while (0)
#define __free_iova(iovad, iova)		do { } while (0)
#define iova_shift(iovad)				PAGE_SHIFT
#define iova_align(iovad, size)			PAGE_ALIGN(size)
#define iova_pfn(iovad, addr)			((unsigned long)((addr) >> PAGE_SHIFT))
#define iova_dma_addr(iovad, iova) \
	((dma_addr_t)(iova)->pfn_lo << PAGE_SHIFT)


/* #pragma mark - no HMM, no MMU notifiers */

/*	A driver registers these for buffers its device reaches through the
	process's own page tables (PASID). Without that, registering fails; the
	NPU driver does not get that far, as its buffers have DMA addresses. */

enum mmu_notifier_event {
	MMU_NOTIFY_UNMAP = 0,
	MMU_NOTIFY_CLEAR,
};
struct mmu_notifier_range {
	struct mm_struct *mm;
	unsigned long start;
	unsigned long end;
	unsigned int flags;
	enum mmu_notifier_event event;
};
struct mmu_interval_notifier;
struct mmu_interval_notifier_ops {
	bool (*invalidate)(struct mmu_interval_notifier *interval_sub,
		const struct mmu_notifier_range *range, unsigned long cur_seq);
};
struct mmu_interval_notifier {
	const struct mmu_interval_notifier_ops *ops;
	struct mm_struct *mm;
	unsigned long invalidate_seq;
};
#define mmu_notifier_range_blockable(range)	true
#define mmu_interval_set_seq(sub, seq) \
	do { (sub)->invalidate_seq = (seq); } while (0)
#define mmu_interval_read_begin(sub)		((sub)->invalidate_seq)
#define mmu_interval_read_retry(sub, seq)	((sub)->invalidate_seq != (seq))
#define mmu_interval_notifier_insert(sub, mm, start, length, ops) \
	(-EOPNOTSUPP)
#define mmu_interval_notifier_insert_locked(sub, mm, start, length, ops) \
	(-EOPNOTSUPP)
#define mmu_interval_notifier_remove(sub)	do { (void)(sub); } while (0)

#define HMM_PFN_REQ_FAULT	(1UL << 63)
#define HMM_PFN_REQ_WRITE	(1UL << 62)
#define HMM_RANGE_DEFAULT_TIMEOUT	1000
#define hmm_range_fault(range)	(-EOPNOTSUPP)
#define hmm_range_fault_unlocked_timeout(range, timeout)	(-EOPNOTSUPP)


/* #pragma mark - files of a pseudo file system (dma-buf) */

/*	dma-buf keeps each buffer in a file of its own file system, whose
	directory entries end the buffer's life. Here the file is all there is:
	the last fput() calls the entry's release. Without descriptors for such
	files, a buffer is only ever passed around inside the driver. */
#ifdef LX_ADDON_AMDXDNA
struct super_block {
	const struct dentry_operations *s_d_op;
};
struct vfsmount *lx_kern_mount(struct file_system_type *type);
void lx_kern_unmount(struct vfsmount *mount);
struct pseudo_fs_context *lx_init_pseudo(struct fs_context *fc,
	unsigned long magic);
struct file *lx_alloc_file_pseudo(struct inode *inode, struct vfsmount *mount,
	const char *name, int flags, const struct file_operations *fops);
#undef kern_mount
#undef kern_unmount
#undef init_pseudo
#undef alloc_file_pseudo
#define kern_mount(type)		lx_kern_mount(type)
#define kern_unmount(mount)		lx_kern_unmount(mount)
#define init_pseudo(fc, magic)	lx_init_pseudo(fc, magic)
#define alloc_file_pseudo(inode, mount, name, flags, fops) \
	lx_alloc_file_pseudo(inode, mount, name, flags, fops)
#endif


/* #pragma mark - SRCU */

/*	The NPU driver sleeps in SRCU read sides (it waits for a command there),
	which the reader count all of RCU shares (lx/lock.h) must not see: every
	grace period would wait for the command. Here each srcu_struct counts its
	own readers. */
#ifdef LX_ADDON_AMDXDNA
#undef srcu_read_lock
#undef srcu_read_unlock
#undef synchronize_srcu
int lx_srcu_read_lock(struct srcu_struct *srcu);
void lx_srcu_read_unlock(struct srcu_struct *srcu);
void lx_synchronize_srcu(struct srcu_struct *srcu);
#define srcu_read_lock(s)			lx_srcu_read_lock(s)
#define srcu_read_unlock(s, idx)	((void)(idx), lx_srcu_read_unlock(s))
#define synchronize_srcu(s)			lx_synchronize_srcu(s)
#endif

#endif /* _LX_ACCEL_H */
