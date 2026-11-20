/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_FS_H
#define _LX_FS_H

/*	Files. Opening /dev/dri/cardN or renderDN on Haiku makes a struct file
	that the DRM core sees as its file (haiku/amdgpu/DrmDevice.cpp). Files
	the drivers make themselves (dma-buf, sync files) exist, but cannot be
	handed to a team as a file descriptor yet. */

struct inode;
struct file;
struct file_system_type;
struct dentry;
struct vfsmount;
struct super_block;
struct poll_table_struct;
struct kiocb;
struct iov_iter;
struct seq_file;
struct path {
	struct vfsmount *mnt;
	struct dentry *dentry;
};

typedef unsigned int __poll_t;
#define EPOLLIN		((__force __poll_t)0x00000001)
#define EPOLLPRI	((__force __poll_t)0x00000002)
#define EPOLLOUT	((__force __poll_t)0x00000004)
#define EPOLLERR	((__force __poll_t)0x00000008)
#define EPOLLHUP	((__force __poll_t)0x00000010)
#define EPOLLNVAL	((__force __poll_t)0x00000020)
#define EPOLLRDNORM	((__force __poll_t)0x00000040)
#define EPOLLRDBAND	((__force __poll_t)0x00000080)
#define EPOLLWRNORM	((__force __poll_t)0x00000100)
#define EPOLLWRBAND	((__force __poll_t)0x00000200)
#define EPOLLMSG	((__force __poll_t)0x00000400)
#define EPOLLRDHUP	((__force __poll_t)0x00002000)
#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020
#define POLLRDNORM	0x0040

typedef struct poll_table_struct {
	void (*_qproc)(struct file *, struct wait_queue_head *,
		struct poll_table_struct *);
	__poll_t _key;
	void *lx_select;		/* Haiku select info */
} poll_table;
void lx_poll_wait(struct file *filp, struct wait_queue_head *wait_address,
	poll_table *p);
static inline void poll_wait(struct file *filp,
	struct wait_queue_head *wait_address, poll_table *p)
{
	lx_poll_wait(filp, wait_address, p);
}
#define poll_requested_events(p)	((p) ? (p)->_key : ~(__poll_t)0)
#define poll_does_not_wait(p)	((p) == NULL)

typedef unsigned int fl_owner_t_lx;
typedef void *fl_owner_t;

struct file_operations {
	struct module *owner;
	unsigned int fop_flags;
	loff_t (*llseek)(struct file *, loff_t, int);
	ssize_t (*read)(struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write)(struct file *, const char __user *, size_t, loff_t *);
	ssize_t (*read_iter)(struct kiocb *, struct iov_iter *);
	ssize_t (*write_iter)(struct kiocb *, struct iov_iter *);
	__poll_t (*poll)(struct file *, struct poll_table_struct *);
	long (*unlocked_ioctl)(struct file *, unsigned int, unsigned long);
	long (*compat_ioctl)(struct file *, unsigned int, unsigned long);
	int (*mmap)(struct file *, struct vm_area_struct *);
	int (*open)(struct inode *, struct file *);
	int (*flush)(struct file *, fl_owner_t id);
	int (*release)(struct inode *, struct file *);
	int (*fsync)(struct file *, loff_t, loff_t, int datasync);
	int (*fasync)(int, struct file *, int);
	unsigned long (*get_unmapped_area)(struct file *, unsigned long,
		unsigned long, unsigned long, unsigned long);
	void (*show_fdinfo)(struct seq_file *m, struct file *f);
};
#define FOP_UNSIGNED_OFFSET	((unsigned int)(1 << 5))

struct inode {
	dev_t i_rdev;
	umode_t i_mode;
	loff_t i_size;
	unsigned long i_ino;
	struct address_space *i_mapping;
	struct address_space i_data;
	const struct file_operations *i_fop;
	void *i_private;
	atomic_t i_count;
	struct super_block *i_sb;
};

struct file {
	atomic_long_t f_count;
	spinlock_t f_lock;
	unsigned int f_flags;
	fmode_t f_mode;
	loff_t f_pos;
	struct inode *f_inode;
	const struct file_operations *f_op;
	struct address_space *f_mapping;
	void *private_data;
	struct path f_path;
	struct mutex f_pos_lock;
	struct task_struct *lx_owner;
};

#define FMODE_READ		((__force fmode_t)(1 << 0))
#define FMODE_WRITE		((__force fmode_t)(1 << 1))
#define FMODE_LSEEK		((__force fmode_t)(1 << 2))
#define FMODE_PREAD		((__force fmode_t)(1 << 3))
#define FMODE_PWRITE	((__force fmode_t)(1 << 4))
#define FMODE_EXEC		((__force fmode_t)(1 << 5))
#define FMODE_UNSIGNED_OFFSET	((__force fmode_t)(1 << 13))
#define O_ACCMODE	00000003
#define O_RDONLY	00000000
#define O_WRONLY	00000001
#define O_RDWR		00000002
#define O_CREAT		00000100
#define O_NONBLOCK	00004000
#define O_CLOEXEC	02000000
#define O_LARGEFILE	00100000
#define FASYNC		00020000
#define SEEK_SET	0
#define SEEK_CUR	1
#define SEEK_END	2
#define MAY_READ	0x00000004

static inline struct inode *file_inode(const struct file *f)
{
	return f->f_inode;
}
#define iminor(inode)	MINOR((inode)->i_rdev)
#define imajor(inode)	MAJOR((inode)->i_rdev)
#define get_file(f)		({ atomic_long_inc(&(f)->f_count); (f); })
#define get_file_active(f) \
	({ struct file *__f = *(f); __f && atomic_long_inc_not_zero(&__f->f_count) ? __f : NULL; })
void fput(struct file *file);
#define file_count(f)	atomic_long_read(&(f)->f_count)
struct file *fget(unsigned int fd);
#define fget_raw(fd)	fget(fd)
int get_unused_fd_flags(unsigned flags);
void put_unused_fd(unsigned int fd);
void fd_install(unsigned int fd, struct file *file);
int close_fd(unsigned int fd);
struct file *anon_inode_getfile(const char *name,
	const struct file_operations *fops, void *priv, int flags);
#define anon_inode_create_getfile(name, fops, priv, flags, ctx) \
	anon_inode_getfile(name, fops, priv, flags)
int anon_inode_getfd(const char *name, const struct file_operations *fops,
	void *priv, int flags);
#define noop_llseek		lx_noop_llseek
loff_t lx_noop_llseek(struct file *file, loff_t offset, int whence);
#define no_llseek		NULL
loff_t default_llseek(struct file *file, loff_t offset, int whence);
loff_t fixed_size_llseek(struct file *file, loff_t offset, int whence,
	loff_t size);
int nonseekable_open(struct inode *inode, struct file *filp);
#define stream_open(inode, filp)	nonseekable_open(inode, filp)
int simple_open(struct inode *inode, struct file *file);
ssize_t simple_read_from_buffer(void __user *to, size_t count, loff_t *ppos,
	const void *from, size_t available);
ssize_t simple_write_to_buffer(void *to, size_t available, loff_t *ppos,
	const void __user *from, size_t count);
#define vfs_fsync(f, datasync)	0
#define file_path(f, buf, len)	"(file)"
#define fops_get(fops)			(fops)
#define fops_put(fops)			do { } while (0)
#define replace_fops(f, fops)	((f)->f_op = (fops))
#define fasync_helper(fd, filp, on, fa)	0
struct fasync_struct { int dummy; };
#define kill_fasync(fa, sig, band)	do { } while (0)
#define SIGIO	29
#define POLL_IN	1
struct vfsmount {
	struct super_block *mnt_sb;
};
void iput(struct inode *inode);
#define igrab(inode)	(inode)
struct inode *alloc_anon_inode(struct super_block *sb);
int simple_pin_fs(struct file_system_type *type, struct vfsmount **mount,
	int *count);
void simple_release_fs(struct vfsmount **mount, int *count);
#define kern_mount(type)	ERR_PTR(-ENOSYS)
#define kern_unmount(mnt)	do { } while (0)
#define alloc_file_pseudo(inode, mnt, name, flags, fops)	ERR_PTR(-ENOSYS)
#define file_ref_get(r)		true
struct dentry_operations {
	char *(*d_dname)(struct dentry *, char *, int);
	void (*d_release)(struct dentry *);
};
struct dentry {
	struct inode *d_inode;
	const struct dentry_operations *d_op;
	void *d_fsdata;
	struct super_block *d_sb;
};
struct fs_context;
struct pseudo_fs_context {
	const struct super_operations *ops;
	const struct xattr_handler *const *xattr;
	const struct dentry_operations *dops;
	unsigned long magic;
};
struct file_system_type {
	const char *name;
	int fs_flags;
	int (*init_fs_context)(struct fs_context *);
	void (*kill_sb)(struct super_block *);
	struct module *owner;
};
#define init_pseudo(fc, magic)	((struct pseudo_fs_context *)NULL)
#define kill_anon_super		NULL
#define dynamic_dname(buf, buflen, fmt, ...)	"dmabuf"
#define d_inode(d)			((d)->d_inode)
#define file_dentry(f)		((f)->f_path.dentry)
#define simple_dname		NULL
#define old_encode_dev(dev)	((u16)(dev))
#define new_encode_dev(dev)	((u32)(dev))
#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)
#define O_EXCL		00000200
#define SIGKILL		9
#define SIGTERM		15
#define overflowuid	65534
#define overflowgid	65534
#define compat_ptr_ioctl	NULL
#define compat_ptr(p)		((void __user *)(uintptr_t)(p))
/* struct fd and CLASS(fd, f)(fd) */
struct fd {
	struct file *file;
};
struct fd lx_fdget(unsigned int fd);
#define fdget(fd)		lx_fdget(fd)
#define fdput(f)		do { if ((f).file) fput((f).file); } while (0)
#define fd_file(f)		((f).file)
#define fd_empty(f)		(!(f).file)
#define EMPTY_FD		((struct fd){ NULL })
DEFINE_CLASS(fd, struct fd, fdput(_T), fdget(fd), int fd)
DEFINE_CLASS(get_unused_fd, int, if (_T >= 0) put_unused_fd(_T),
	get_unused_fd_flags(flags), unsigned flags)
#define capable(cap)		true
#define CAP_SYS_ADMIN		21
#define CAP_SYS_NICE		23
#define CAP_PERFMON			38
#define current_uid()		0
#define current_euid()		0
#define uid_eq(a, b)		((a) == (b))
#define GLOBAL_ROOT_UID		0
#define from_kuid(ns, uid)	(uid)
#define from_kuid_munged(ns, uid)	(uid)
#define current_user_ns()	NULL
#define init_user_ns		(*(int *)NULL)
typedef unsigned int kuid_t;
typedef unsigned int kgid_t;
#define task_euid(task)		0
#define current_cred()		NULL
#define get_current_cred()	NULL
#define put_cred(c)			do { } while (0)

/* seq_file and debugfs: there is no debugfs */
struct seq_file {
	char *buf;
	size_t size;
	size_t count;
	void *private;
};
void seq_printf(struct seq_file *m, const char *fmt, ...) __printf(2, 3);
void seq_vprintf(struct seq_file *m, const char *fmt, va_list args);
void seq_puts(struct seq_file *m, const char *s);
void seq_putc(struct seq_file *m, char c);
int seq_write(struct seq_file *m, const void *data, size_t len);
#define d_path(path, buf, len)	((char *)ERR_PTR(-ENOSYS))
#define mangle_path(s, p, esc)	((char *)NULL)
#define seq_hex_dump(m, prefix, type, rowsize, groupsize, buf, len, ascii) \
	do { } while (0)
#define seq_put_decimal_ull(m, delimiter, num)	seq_printf(m, "%s%llu", delimiter, (unsigned long long)(num))
#define seq_open(f, op)		(-ENOSYS)
#define seq_read			NULL
#define seq_lseek			NULL
#define seq_release			NULL
#define single_open(f, show, data)	(-ENOSYS)
#define single_release		NULL
#define seq_file_path(m, file, esc)	0
#define DEFINE_SHOW_ATTRIBUTE(__name) \
	static const struct file_operations __name ## _fops __maybe_unused = { \
		.owner = THIS_MODULE }
#define DEFINE_SHOW_STORE_ATTRIBUTE(__name)	DEFINE_SHOW_ATTRIBUTE(__name)
#define DEFINE_DEBUGFS_ATTRIBUTE(__fops, __get, __set, __fmt) \
	static const struct file_operations __fops __maybe_unused = { \
		.owner = THIS_MODULE }
#define DEFINE_SIMPLE_ATTRIBUTE	DEFINE_DEBUGFS_ATTRIBUTE
struct seq_operations {
	void *(*start)(struct seq_file *m, loff_t *pos);
	void (*stop)(struct seq_file *m, void *v);
	void *(*next)(struct seq_file *m, void *v, loff_t *pos);
	int (*show)(struct seq_file *m, void *v);
};
#define SEQ_START_TOKEN	((void *)1)
struct debugfs_reg32 {
	char *name;
	unsigned long offset;
};
struct debugfs_regset32 {
	const struct debugfs_reg32 *regs;
	int nregs;
	void __iomem *base;
	struct device *dev;
};
struct debugfs_blob_wrapper { void *data; unsigned long size; };
#define debugfs_create_dir(name, parent)	ERR_PTR(-ENODEV)
#define debugfs_create_file(name, mode, parent, data, fops)	ERR_PTR(-ENODEV)
#define debugfs_create_file_unsafe(name, mode, parent, data, fops)	ERR_PTR(-ENODEV)
#define debugfs_create_file_size(name, mode, parent, data, fops, size) \
	ERR_PTR(-ENODEV)
#define debugfs_create_blob(name, mode, parent, blob)	ERR_PTR(-ENODEV)
#define debugfs_create_bool(name, mode, parent, value)	do { } while (0)
#define debugfs_create_u8(name, mode, parent, value)	do { } while (0)
#define debugfs_create_u32(name, mode, parent, value)	do { } while (0)
#define debugfs_create_x32(name, mode, parent, value)	do { } while (0)
#define debugfs_create_u64(name, mode, parent, value)	do { } while (0)
#define debugfs_create_x64(name, mode, parent, value)	do { } while (0)
#define debugfs_create_ulong(name, mode, parent, value)	do { } while (0)
#define debugfs_create_atomic_t(name, mode, parent, value)	do { } while (0)
#define debugfs_create_symlink(name, parent, dest)	ERR_PTR(-ENODEV)
#define debugfs_remove(dentry)			do { } while (0)
#define debugfs_remove_recursive(dentry)	do { } while (0)
#define debugfs_lookup(name, parent)	NULL
#define debugfs_lookup_and_remove(name, parent)	do { } while (0)
#define debugfs_initialized()	false
#define debugfs_attr_read		NULL
#define debugfs_attr_write		NULL

#endif /* _LX_FS_H */
