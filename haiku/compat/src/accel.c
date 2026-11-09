/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	What the AMD NPU driver needs beyond the GPU drivers (lx/accel.h):
	managed BAR mappings and page runs, the files of dma-buf's pseudo file
	system, and SRCU with a reader count per srcu_struct. */

#include <lx/haiku.h>


/* #pragma mark - managed resources */


struct lx_iomap_devres {
	void __iomem *address;
};


static void
lx_iomap_release(struct device *dev, void *res)
{
	struct lx_iomap_devres *map = res;
	iounmap(map->address);
}


void __iomem *(*lx_pcim_iomap_hook)(struct pci_dev *pdev, int bar);
int lx_emulated_device;


void __iomem *
pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen)
{
	resource_size_t start = pci_resource_start(pdev, bar);
	resource_size_t length = pci_resource_len(pdev, bar);
	struct lx_iomap_devres *map;

	if (lx_pcim_iomap_hook != NULL) {
		void __iomem *address = lx_pcim_iomap_hook(pdev, bar);
		if (address != NULL)
			return address;
	}
	if (start == 0 || length == 0
		|| (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) == 0)
		return NULL;
	if (maxlen != 0 && length > maxlen)
		length = maxlen;
	map = devres_alloc(lx_iomap_release, sizeof(*map), GFP_KERNEL);
	if (map == NULL)
		return NULL;
	map->address = ioremap(start, length);
	if (map->address == NULL) {
		devres_free(map);
		return NULL;
	}
	devres_add(&pdev->dev, map);
	return map->address;
}


struct lx_pages_devres {
	unsigned long address;
	unsigned int order;
};


static void
lx_pages_release(struct device *dev, void *res)
{
	struct lx_pages_devres *pages = res;
	free_pages(pages->address, pages->order);
}


unsigned long
devm_get_free_pages(struct device *dev, gfp_t gfp, unsigned int order)
{
	struct lx_pages_devres *pages = devres_alloc(lx_pages_release,
		sizeof(*pages), GFP_KERNEL);
	if (pages == NULL)
		return 0;
	pages->address = __get_free_pages(gfp, order);
	pages->order = order;
	if (pages->address == 0) {
		devres_free(pages);
		return 0;
	}
	devres_add(dev, pages);
	return pages->address;
}


/* #pragma mark - step log */


int lx_step_log = 1;
module_param_named(step_log, lx_step_log, int, 0644);


void
lx_step_print(const char *format, ...)
{
	char line[200];
	va_list args;

	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	printk(KERN_INFO KBUILD_MODNAME ": step: %s\n", line);
}


/* #pragma mark - files of a pseudo file system */

#ifdef LX_ADDON_AMDXDNA

struct fs_context {
	struct pseudo_fs_context pseudo;
};

struct lx_pseudo_mount {
	struct vfsmount mount;
	struct super_block sb;
};


struct pseudo_fs_context *
lx_init_pseudo(struct fs_context *fc, unsigned long magic)
{
	fc->pseudo.magic = magic;
	return &fc->pseudo;
}


/*	The file system type says what its directory entries do
	(init_fs_context()); a mount is no more than that. */
struct vfsmount *
lx_kern_mount(struct file_system_type *type)
{
	struct lx_pseudo_mount *mount;
	struct fs_context fc = {};
	int error;

	if (type->init_fs_context == NULL)
		return ERR_PTR(-EINVAL);
	error = type->init_fs_context(&fc);
	if (error != 0)
		return ERR_PTR(error);
	mount = kzalloc(sizeof(*mount), GFP_KERNEL);
	if (mount == NULL)
		return ERR_PTR(-ENOMEM);
	mount->sb.s_d_op = fc.pseudo.dops;
	mount->mount.mnt_sb = &mount->sb;
	return &mount->mount;
}


void
lx_kern_unmount(struct vfsmount *mount)
{
	if (!IS_ERR_OR_NULL(mount))
		kfree(container_of(mount, struct lx_pseudo_mount, mount));
}


/*	A file for \a inode, which it owns from now on, with a directory entry
	of its own; fput() releases both (compat/src/fs.c). */
struct file *
lx_alloc_file_pseudo(struct inode *inode, struct vfsmount *mount,
	const char *name, int flags, const struct file_operations *fops)
{
	struct file *file = kzalloc(sizeof(*file), GFP_KERNEL);
	struct dentry *dentry = kzalloc(sizeof(*dentry), GFP_KERNEL);

	if (file == NULL || dentry == NULL) {
		kfree(file);
		kfree(dentry);
		return ERR_PTR(-ENOMEM);
	}
	dentry->d_inode = inode;
	dentry->d_sb = mount->mnt_sb;
	dentry->d_op = mount->mnt_sb->s_d_op;

	atomic_long_set(&file->f_count, 1);
	spin_lock_init(&file->f_lock);
	mutex_init(&file->f_pos_lock);
	file->f_flags = flags;
	file->f_mode = FMODE_READ | FMODE_WRITE;
	file->f_op = fops;
	file->f_inode = inode;
	file->f_mapping = inode->i_mapping;
	file->f_path.mnt = mount;
	file->f_path.dentry = dentry;
	return file;
}


/* #pragma mark - SRCU */


int
lx_srcu_read_lock(struct srcu_struct *srcu)
{
	__atomic_add_fetch(&srcu->readers, 1, __ATOMIC_SEQ_CST);
	return 0;
}


void
lx_srcu_read_unlock(struct srcu_struct *srcu)
{
	__atomic_sub_fetch(&srcu->readers, 1, __ATOMIC_SEQ_CST);
}


/*	Returns once no reader was in a read side: every reader that began
	before the call has ended then. Readers may sleep for long, so this
	sleeps between looks. */
void
lx_synchronize_srcu(struct srcu_struct *srcu)
{
	int spins = 0;
	while (__atomic_load_n(&srcu->readers, __ATOMIC_SEQ_CST) != 0) {
		if (++spins < 100)
			cpu_relax();
		else
			lx_haiku_snooze(spins < 1000 ? 20 : 1000);
	}
}

#endif	/* LX_ADDON_AMDXDNA */
