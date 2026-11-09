/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Files, polling, seq_file and sysfs output, and the page insertion a
	fault handler does while the Haiku driver fills a mapping. */

#include <lx/haiku.h>
#include <lx/vma.h>


void
fput(struct file *file)
{
	if (file == NULL)
		return;
	if (atomic_long_dec_and_test(&file->f_count)) {
		if (file->f_op != NULL && file->f_op->release != NULL)
			file->f_op->release(file->f_inode, file);
		/* a file of a pseudo file system (alloc_file_pseudo()): its
		 * directory entry goes with it */
		if (file->f_path.dentry != NULL) {
			struct dentry *dentry = file->f_path.dentry;
			if (dentry->d_op != NULL && dentry->d_op->d_release != NULL)
				dentry->d_op->d_release(dentry);
			kfree(dentry);
		}
		kfree(file->f_inode);
		kfree(file);
	}
}


struct file *
anon_inode_getfile(const char *name, const struct file_operations *fops,
	void *priv, int flags)
{
	struct file *file = kzalloc(sizeof(*file), GFP_KERNEL);
	struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);
	if (file == NULL || inode == NULL) {
		kfree(file);
		kfree(inode);
		return ERR_PTR(-ENOMEM);
	}
	atomic_long_set(&file->f_count, 1);
	spin_lock_init(&file->f_lock);
	mutex_init(&file->f_pos_lock);
	file->f_flags = flags & (O_ACCMODE | O_NONBLOCK);
	file->f_mode = FMODE_READ | FMODE_WRITE;
	file->f_op = fops;
	file->private_data = priv;
	file->f_inode = inode;
	inode->i_mapping = &inode->i_data;
	file->f_mapping = inode->i_mapping;
	return file;
}


int
anon_inode_getfd(const char *name, const struct file_operations *fops,
	void *priv, int flags)
{
	/* no file descriptors for driver-made files on Haiku (yet) */
	return -ENOSYS;
}


int
get_unused_fd_flags(unsigned flags)
{
	return -ENOSYS;
}


void
put_unused_fd(unsigned int fd)
{
}


void
fd_install(unsigned int fd, struct file *file)
{
}


int
close_fd(unsigned int fd)
{
	return -EBADF;
}


struct file *
fget(unsigned int fd)
{
	return NULL;
}


struct fd
lx_fdget(unsigned int fd)
{
	return (struct fd){ NULL };
}


loff_t
lx_noop_llseek(struct file *file, loff_t offset, int whence)
{
	return file->f_pos;
}


loff_t
default_llseek(struct file *file, loff_t offset, int whence)
{
	switch (whence) {
		case SEEK_SET:
			break;
		case SEEK_CUR:
			offset += file->f_pos;
			break;
		default:
			return -EINVAL;
	}
	if (offset < 0)
		return -EINVAL;
	file->f_pos = offset;
	return offset;
}


loff_t
fixed_size_llseek(struct file *file, loff_t offset, int whence, loff_t size)
{
	if (whence == SEEK_END)
		offset += size;
	else if (whence == SEEK_CUR)
		offset += file->f_pos;
	if (offset < 0 || offset > size)
		return -EINVAL;
	file->f_pos = offset;
	return offset;
}


struct inode *
alloc_anon_inode(struct super_block *sb)
{
	struct inode *inode = kzalloc(sizeof(*inode), GFP_KERNEL);
	if (inode == NULL)
		return ERR_PTR(-ENOMEM);
	inode->i_mapping = &inode->i_data;
	inode->i_data.host = inode;
	inode->i_sb = sb;
	atomic_set(&inode->i_count, 1);
	return inode;
}


void
iput(struct inode *inode)
{
	if (inode != NULL && atomic_dec_and_test(&inode->i_count))
		kfree(inode);
}


int
simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
	int *count)
{
	static struct vfsmount pseudo_mount;
	*mount = &pseudo_mount;
	(*count)++;
	return 0;
}


void
simple_release_fs(struct vfsmount **mount, int *count)
{
	(*count)--;
}


int
nonseekable_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return 0;
}


int
simple_open(struct inode *inode, struct file *file)
{
	if (inode->i_private)
		file->private_data = inode->i_private;
	return 0;
}


ssize_t
simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
	const void *from, size_t available)
{
	loff_t pos = *ppos;
	size_t ret;
	if (pos < 0)
		return -EINVAL;
	if (pos >= available || !count)
		return 0;
	if (count > available - pos)
		count = available - pos;
	ret = copy_to_user(to, (const char *)from + pos, count);
	if (ret == count)
		return -EFAULT;
	count -= ret;
	*ppos = pos + count;
	return count;
}


ssize_t
simple_write_to_buffer(void *to, size_t available, loff_t *ppos,
	const void __user *from, size_t count)
{
	loff_t pos = *ppos;
	size_t res;
	if (pos < 0)
		return -EINVAL;
	if (pos >= available || !count)
		return 0;
	if (count > available - pos)
		count = available - pos;
	res = copy_from_user((char *)to + pos, from, count);
	if (res == count)
		return -EFAULT;
	count -= res;
	*ppos = pos + count;
	return count;
}


void
lx_poll_wait(struct file *filp, struct wait_queue_head *wait_address,
	poll_table *p)
{
	if (p != NULL && p->_qproc != NULL && wait_address != NULL)
		p->_qproc(filp, wait_address, p);
}


/* #pragma mark - seq_file, sysfs */


void
seq_vprintf(struct seq_file *m, const char *fmt, va_list args)
{
	int length;
	if (m->count >= m->size)
		return;
	length = vsnprintf(m->buf + m->count, m->size - m->count, fmt, args);
	m->count = min(m->count + length, m->size);
}


void
seq_printf(struct seq_file *m, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	seq_vprintf(m, fmt, args);
	va_end(args);
}


void
seq_puts(struct seq_file *m, const char *s)
{
	seq_printf(m, "%s", s);
}


void
seq_putc(struct seq_file *m, char c)
{
	if (m->count < m->size)
		m->buf[m->count++] = c;
}


int
seq_write(struct seq_file *m, const void *data, size_t length)
{
	if (m->count + length > m->size) {
		m->count = m->size;
		return -1;
	}
	memcpy(m->buf + m->count, data, length);
	m->count += length;
	return 0;
}


int
sysfs_emit(char *buf, const char *fmt, ...)
{
	va_list args;
	int length;
	va_start(args, fmt);
	length = vscnprintf(buf, PAGE_SIZE, fmt, args);
	va_end(args);
	return length;
}


int
sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
	va_list args;
	int length;
	if (at < 0 || at >= (int)PAGE_SIZE)
		return 0;
	va_start(args, fmt);
	length = vscnprintf(buf + at, PAGE_SIZE - at, fmt, args);
	va_end(args);
	return length;
}


/* #pragma mark - mappings into teams */


vm_fault_t
vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
	unsigned long pfn, pgprot_t pgprot)
{
	struct lx_vma_pages *pages = vma->lx_haiku;
	unsigned long index;
	if (pages == NULL)
		return VM_FAULT_SIGBUS;
	if (addr < vma->vm_start || addr >= vma->vm_end)
		return VM_FAULT_SIGBUS;
	index = (addr - vma->vm_start) >> PAGE_SHIFT;
	pages->pfns[index] = pfn;
	pages->prot = pgprot_val(pgprot);
	return VM_FAULT_NOPAGE;
}


int
remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
	unsigned long pfn, unsigned long size, pgprot_t prot)
{
	unsigned long offset;
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		if (vmf_insert_pfn_prot(vma, addr + offset, pfn + offset / PAGE_SIZE,
				prot) != VM_FAULT_NOPAGE)
			return -EFAULT;
	}
	return 0;
}


void (*lx_unmap_mapping_range_hook)(struct address_space *mapping,
	loff_t start, loff_t length);

void
unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
	loff_t const holelen, int even_cows)
{
	if (lx_unmap_mapping_range_hook != NULL)
		lx_unmap_mapping_range_hook(mapping, holebegin, holelen);
}
