/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_DEVICE_H
#define _LX_DEVICE_H

/*	The driver model as far as DRM uses it: struct device with managed
	resources (devres), printing, firmware files, modules and parameters.
	There is no sysfs, so attributes are accepted and never shown. */

/* #pragma mark - modules */

struct module { const char *name; };
extern struct module lx_this_module;
#define THIS_MODULE		(&lx_this_module)
#define KBUILD_MODNAME_LX	KBUILD_MODNAME
#define MODULE_LICENSE(x)
#define MODULE_AUTHOR(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_VERSION(x)
#define MODULE_FIRMWARE(x)
#define MODULE_ALIAS(x)
#define MODULE_SOFTDEP(x)
#define MODULE_INFO(tag, info)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_IMPORT_NS(ns)
#define MODULE_PARM_DESC(var, desc)
#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_NS(sym, ns)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)
#define EXPORT_SYMBOL_IF_KUNIT(sym)
#define VISIBLE_IF_KUNIT		static
#define try_module_get(m)		true
#define module_put(m)			do { } while (0)
#define __module_get(m)			do { } while (0)
#define module_init(fn)			int lx_module_init_##fn(void) { return fn(); }
#define module_exit(fn)			void lx_module_exit_##fn(void) { fn(); }
#define subsys_initcall(fn)		module_init(fn)
#define late_initcall(fn)		module_init(fn)
#define device_initcall(fn)		module_init(fn)
#define module_driver(drv, reg, unreg, ...)
#define __init
#define __exitcall(fn)
#define __initcall(fn)
#define __exit
#define __initdata
#define __initconst
#define __exitdata
#define __devinit
#define __ro_after_init
#define __read_mostly
#define __refdata
#define __init_or_module
#define __exit_p(x)		x
#define symbol_get(x)	NULL
#define symbol_put(x)	do { } while (0)
#define symbol_request(x)	NULL
#define request_module(...)	(-ENOENT)
#define request_module_nowait(...)	(-ENOENT)

/* module parameters: plain variables (settings are applied by the driver) */
struct kernel_param;
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};
struct kernel_param {
	const char *name;
	struct module *mod;
	const struct kernel_param_ops *ops;
	void *arg;
};
/*	Parameters are listed in the lx_params section, so the Haiku driver can
	set them from its settings file before the drivers start. */
struct lx_param {
	const char *name;
	void *value;
	const char *type;
};
#define __lx_module_param(pname, var, ptype) \
	static const struct lx_param __UNIQUE_ID(lx_param) __used \
		__section("lx_params") __aligned(8) = { #pname, &(var), #ptype }
#define module_param(name, type, perm)	__lx_module_param(name, name, type)
#define module_param_named(name, value, type, perm) \
	__lx_module_param(name, value, type)
#define module_param_unsafe(name, type, perm)	__lx_module_param(name, name, type)
#define module_param_named_unsafe(name, value, type, perm) \
	__lx_module_param(name, value, type)
#define module_param_string(name, string, len, perm)
#define module_param_array(name, type, nump, perm)
#define module_param_array_named(name, array, type, nump, perm)
#define module_param_cb_unsafe(name, ops, arg, perm)
#define module_param_cb(name, ops, arg, perm)
#define module_param_call(name, set, get, arg, perm)
#define core_param(name, var, type, perm)
#define __setup(str, fn)
extern const struct kernel_param_ops param_ops_int;
extern const struct kernel_param_ops param_ops_uint;
extern const struct kernel_param_ops param_ops_bool;
int param_set_int(const char *val, const struct kernel_param *kp);
int param_get_int(char *buffer, const struct kernel_param *kp);
int param_set_uint(const char *val, const struct kernel_param *kp);
int param_get_uint(char *buffer, const struct kernel_param *kp);
int param_set_bool(const char *val, const struct kernel_param *kp);
int param_get_bool(char *buffer, const struct kernel_param *kp);
#define S_IRUGO		00444
#define S_IRGRP		00040
#define S_IROTH		00004
#define S_IWGRP		00020
#define S_IWOTH		00002
#define S_IXUSR		00100
#define S_IRWXU		00700
#define S_IWUSR		00200
#define S_IRUSR		00400
#define S_IWUGO		00222
#define S_IXUGO		00111
#define S_IRWXUGO	00777
#define S_IALLUGO	07777
#define S_IFMT		00170000
#define S_IFREG		0100000
#define S_IFCHR		0020000
#define S_IFDIR		0040000
#define S_ISREG(m)	(((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m)	(((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m)	(((m) & S_IFMT) == S_IFDIR)

/* #pragma mark - kobjects and sysfs (accepted, never shown) */

struct attribute {
	const char *name;
	umode_t mode;
};
struct kset;
struct kobj_type;
struct kobject {
	const char *name;
	struct list_head entry;
	struct kobject *parent;
	struct kset *kset;
	const struct kobj_type *ktype;
	void *sd;
	struct kref kref;
	unsigned int state_initialized:1;
	unsigned int state_in_sysfs:1;
};
struct sysfs_ops {
	ssize_t (*show)(struct kobject *, struct attribute *, char *);
	ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t);
};
struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	const struct attribute_group **default_groups;
};
struct kset {
	struct list_head list;
	spinlock_t list_lock;
	struct kobject kobj;
};
extern const struct sysfs_ops kobj_sysfs_ops;
void kobject_init(struct kobject *kobj, const struct kobj_type *ktype);
int kobject_add(struct kobject *kobj, struct kobject *parent,
	const char *fmt, ...) __printf(3, 4);
int kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
	struct kobject *parent, const char *fmt, ...) __printf(4, 5);
void kobject_put(struct kobject *kobj);
struct kobject *kobject_get(struct kobject *kobj);
void kobject_del(struct kobject *kobj);
int kset_register(struct kset *kset);
void kset_unregister(struct kset *kset);
#define kset_create_and_add(name, uevent_ops, parent)	((struct kset *)NULL)
#define to_kset(k)	container_of(k, struct kset, kobj)
struct bin_attribute;
struct attribute_group {
	const char *name;
	umode_t (*is_visible)(struct kobject *, struct attribute *, int);
	umode_t (*is_bin_visible)(struct kobject *, const struct bin_attribute *, int);
	size_t (*bin_size)(struct kobject *, const struct bin_attribute *, int);
	struct attribute **attrs;
	const struct bin_attribute *const *bin_attrs;
};
struct file;
struct bin_attribute {
	struct attribute attr;
	size_t size;
	void *private;
	ssize_t (*read)(struct file *, struct kobject *, const struct bin_attribute *,
		char *, loff_t, size_t);
	ssize_t (*write)(struct file *, struct kobject *, const struct bin_attribute *,
		char *, loff_t, size_t);
	int (*mmap)(struct file *, struct kobject *, const struct bin_attribute *,
		struct vm_area_struct *);
};
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr, char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count);
};
#define sysfs_create_group(kobj, grp)		0
#define sysfs_remove_group(kobj, grp)		do { } while (0)
#define sysfs_create_groups(kobj, grp)		0
#define sysfs_remove_groups(kobj, grp)		do { } while (0)
#define sysfs_create_files(kobj, attr)		0
#define sysfs_remove_files(kobj, attr)		do { } while (0)
#define sysfs_create_file(kobj, attr)		0
#define sysfs_remove_file(kobj, attr)		do { } while (0)
#define sysfs_create_bin_file(kobj, attr)	0
#define sysfs_remove_bin_file(kobj, attr)	do { } while (0)
#define sysfs_create_link(kobj, target, name)	0
#define sysfs_remove_link(kobj, name)		do { } while (0)
#define sysfs_notify(kobj, dir, attr)		do { } while (0)
#define sysfs_update_group(kobj, grp)		0
#define sysfs_merge_group(kobj, grp)		0
#define sysfs_unmerge_group(kobj, grp)		do { } while (0)
int sysfs_emit(char *buf, const char *fmt, ...) __printf(2, 3);
int sysfs_emit_at(char *buf, int at, const char *fmt, ...) __printf(3, 4);
#define sysfs_attr_init(attr)	do { } while (0)
#define sysfs_bin_attr_init(attr)	do { } while (0)
#define sysfs_add_file_to_group(kobj, attr, group)	0
#define sysfs_remove_file_from_group(kobj, attr, group)	do { } while (0)
#define kobj_to_dev(k)	container_of(k, struct device, kobj)
#define kobject_set_name(kobj, fmt, ...)	0
#define kobject_uevent(kobj, action)	0
#define kobject_uevent_env(kobj, action, envp)	0
#define kobject_create_and_add(name, parent)	((struct kobject *)NULL)
#define kobject_name(kobj)		((kobj)->name)
#define KOBJ_CHANGE		2
#define KOBJ_ADD		0
#define KOBJ_REMOVE		1
#define __ATTR(_name, _mode, _show, _store) { \
	.attr = { .name = __stringify(_name), .mode = (_mode) }, \
	.show = _show, .store = _store }
#define __ATTR_RO(_name) { \
	.attr = { .name = __stringify(_name), .mode = 0444 }, .show = _name##_show }
#define __ATTR_RO_MODE(_name, _mode) { \
	.attr = { .name = __stringify(_name), .mode = (_mode) }, .show = _name##_show }
#define __ATTR_RW(_name)	__ATTR(_name, 0644, _name##_show, _name##_store)
#define __ATTR_RW_MODE(_name, _mode)	__ATTR(_name, _mode, _name##_show, _name##_store)
#define __ATTR_WO(_name) { \
	.attr = { .name = __stringify(_name), .mode = 0200 }, .store = _name##_store }
#define __ATTR_NULL			{ .attr = { .name = NULL } }
#define ATTRIBUTE_GROUPS(_name) \
	static const struct attribute_group _name##_group = { .attrs = _name##_attrs }; \
	static const struct attribute_group *_name##_groups[] = { &_name##_group, NULL }
#define BIN_ATTR(_name, _mode, _read, _write, _size) \
	struct bin_attribute bin_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = (_mode) }, \
		.read = _read, .write = _write, .size = _size }
#define BIN_ATTR_RO(_name, _size) \
	struct bin_attribute bin_attr_##_name = { \
		.attr = { .name = __stringify(_name), .mode = 0444 }, \
		.read = _name##_read, .size = _size }
#define BIN_ATTR_ADMIN_RO(_name, _size)	BIN_ATTR_RO(_name, _size)
#define __BIN_ATTR(_name, _mode, _read, _write, _size) { \
	.attr = { .name = __stringify(_name), .mode = (_mode) }, \
	.read = _read, .write = _write, .size = _size }
#define __BIN_ATTR_RO(_name, _size) { \
	.attr = { .name = __stringify(_name), .mode = 0444 }, \
	.read = _name##_read, .size = _size }

/* #pragma mark - devices */

struct device;
struct device_driver;
struct device_type;
struct class;
struct bus_type;
struct fwnode_handle;
struct device_node;
struct dev_pm_ops;
struct iommu_group;

struct device_dma_parameters {
	unsigned int max_segment_size;
	unsigned int min_align_mask;
	unsigned long segment_boundary_mask;
};

struct dev_pm_info {
	unsigned int can_wakeup:1;
	unsigned int async_suspend:1;
	unsigned int runtime_auto:1;
	int usage_count_lx;
	bool is_suspended;
};

struct device {
	struct kobject kobj;
	struct device *parent;
	const char *init_name;
	const char *lx_name;
	const struct device_type *type;
	const struct bus_type *bus;
	struct device_driver *driver;
	void *platform_data;
	void *driver_data;
	struct dev_pm_info power;
	int numa_node;
	u64 *dma_mask;
	u64 coherent_dma_mask;
	u64 bus_dma_limit;
	struct device_dma_parameters *dma_parms;
	struct device_node *of_node;
	struct fwnode_handle *fwnode;
	dev_t devt;
	u32 id;
	spinlock_t devres_lock;
	struct list_head devres_head;
	const struct class *class;
	const struct attribute_group **groups;
	void (*release)(struct device *dev);
	struct iommu_group *iommu_group;
	bool dma_coherent:1;
	bool lx_registered:1;
	u64 lx_dma_mask;
};

struct device_type {
	const char *name;
	const struct attribute_group **groups;
	void (*release)(struct device *dev);
};

struct device_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count);
};
#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO(_name)
#define DEVICE_ATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW(_name)
#define DEVICE_ATTR_WO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_WO(_name)
#define DEVICE_ATTR_ADMIN_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, 0400, _name##_show, NULL)
#define DEVICE_ATTR_ADMIN_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, 0600, _name##_show, _name##_store)
#define device_create_file(dev, attr)	0
#define device_remove_file(dev, attr)	do { } while (0)
#define device_add_groups(dev, groups)	0
#define device_remove_groups(dev, groups)	do { } while (0)
#define device_add_group(dev, group)	0
#define device_remove_group(dev, group)	do { } while (0)
#define devm_device_add_group(dev, group)	0
#define devm_device_add_groups(dev, groups)	0

struct device_driver {
	const char *name;
	const struct bus_type *bus;
	struct module *owner;
	const struct dev_pm_ops *pm;
	int (*probe)(struct device *dev);
	int (*remove)(struct device *dev);
	void (*shutdown)(struct device *dev);
};

struct class {
	const char *name;
	char *(*devnode)(const struct device *dev, umode_t *mode);
};

struct kobj_uevent_env;
struct dev_pm_ops;
struct bus_type {
	const char *name;
	int (*match)(struct device *dev, const struct device_driver *drv);
	int (*uevent)(const struct device *dev, struct kobj_uevent_env *env);
	const struct dev_pm_ops *pm;
};

static inline const char *dev_name(const struct device *dev)
{
	if (dev->init_name)
		return dev->init_name;
	return dev->lx_name ? dev->lx_name : "amdgpu";
}
int dev_set_name(struct device *dev, const char *name, ...) __printf(2, 3);
static inline void *dev_get_drvdata(const struct device *dev)
{
	return dev->driver_data;
}
static inline void dev_set_drvdata(struct device *dev, void *data)
{
	dev->driver_data = data;
}
#define dev_driver_string(dev)	"amdgpu"
#define dev_to_node(dev)		NUMA_NO_NODE
#define set_dev_node(dev, node)	do { } while (0)
#define dev_is_pci(dev)			1
#define dev_fwnode(dev)			((dev)->fwnode)
#define dev_of_node(dev)		((dev)->of_node)
#define device_is_registered(dev)	((dev)->lx_registered)
struct device *get_device(struct device *dev);
void put_device(struct device *dev);
void device_initialize(struct device *dev);
int device_add(struct device *dev);
void device_del(struct device *dev);
#define device_register(dev)	({ device_initialize(dev); device_add(dev); })
#define device_unregister(dev)	do { device_del(dev); put_device(dev); } while (0)
#define device_set_wakeup_capable(dev, val)	do { } while (0)
#define device_set_wakeup_enable(dev, val)	0
#define device_wakeup_enable(dev)	0
#define device_init_wakeup(dev, val)	0
#define device_can_wakeup(dev)		false
#define device_may_wakeup(dev)		false
#define device_link_add(consumer, supplier, flags)	NULL
#define device_link_del(link)	do { } while (0)
#define device_link_remove(consumer, supplier)	do { } while (0)
#define DL_FLAG_STATELESS		(1 << 0)
#define DL_FLAG_AUTOREMOVE_CONSUMER	(1 << 2)
#define DL_FLAG_PM_RUNTIME		(1 << 3)
#define device_property_read_u32(dev, name, val)	(-EINVAL)
#define device_property_present(dev, name)	false
#define device_property_read_string(dev, name, val)	(-EINVAL)
#define device_get_match_data(dev)	NULL
#define device_lock(dev)		do { } while (0)
#define dev_is_removable(dev)	false
typedef int suspend_state_t;
#define device_unlock(dev)		do { } while (0)
#define device_trylock(dev)		1
#define device_lock_assert(dev)	do { } while (0)
#define device_create(cls, parent, devt, data, fmt, ...)	ERR_PTR(-ENOSYS)
#define device_destroy(cls, devt)	do { } while (0)
#define class_create(name)		ERR_PTR(-ENOSYS)
#define class_destroy(cls)		do { } while (0)
#define class_register(cls)		0
#define class_unregister(cls)	do { } while (0)
#define MKDEV(ma, mi)			(((ma) << 20) | (mi))
#define MAJOR(dev)				((unsigned int)((dev) >> 20))
#define MINOR(dev)				((unsigned int)((dev) & ((1U << 20) - 1)))
#define register_chrdev(major, name, fops)	0
#define unregister_chrdev(major, name)	do { } while (0)

/* printing with the device's name */
void _dev_printk(const char *level, const struct device *dev,
	const char *fmt, ...) __printf(3, 4);
#define dev_printk(level, dev, fmt, ...) \
	_dev_printk(level, dev, fmt, ##__VA_ARGS__)
#define dev_emerg(dev, fmt, ...)	dev_printk(KERN_EMERG, dev, fmt, ##__VA_ARGS__)
#define dev_crit(dev, fmt, ...)		dev_printk(KERN_CRIT, dev, fmt, ##__VA_ARGS__)
#define dev_alert(dev, fmt, ...)	dev_printk(KERN_ALERT, dev, fmt, ##__VA_ARGS__)
#define dev_err(dev, fmt, ...)		dev_printk(KERN_ERR, dev, fmt, ##__VA_ARGS__)
#define dev_warn(dev, fmt, ...)		dev_printk(KERN_WARNING, dev, fmt, ##__VA_ARGS__)
#define dev_notice(dev, fmt, ...)	dev_printk(KERN_NOTICE, dev, fmt, ##__VA_ARGS__)
#define dev_info(dev, fmt, ...)		dev_printk(KERN_INFO, dev, fmt, ##__VA_ARGS__)
#define dev_dbg(dev, fmt, ...) \
	({ if (0) dev_printk(KERN_DEBUG, dev, fmt, ##__VA_ARGS__); })
#define dev_vdbg	dev_dbg
#define dev_err_once(dev, fmt, ...)		DO_ONCE_LITE(dev_err, dev, fmt, ##__VA_ARGS__)
#define dev_warn_once(dev, fmt, ...)	DO_ONCE_LITE(dev_warn, dev, fmt, ##__VA_ARGS__)
#define dev_info_once(dev, fmt, ...)	DO_ONCE_LITE(dev_info, dev, fmt, ##__VA_ARGS__)
#define dev_notice_once(dev, fmt, ...)	DO_ONCE_LITE(dev_notice, dev, fmt, ##__VA_ARGS__)
#define dev_dbg_once(dev, fmt, ...)		dev_dbg(dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited			dev_err
#define dev_warn_ratelimited		dev_warn
#define dev_info_ratelimited		dev_info
#define dev_notice_ratelimited		dev_notice
#define dev_dbg_ratelimited			dev_dbg
#define dev_level_once(level, dev, fmt, ...) \
	DO_ONCE_LITE(dev_printk, level, dev, fmt, ##__VA_ARGS__)
#define dev_WARN(dev, format, arg...)	WARN(1, "%s: " format, dev_name(dev), ## arg)
#define dev_WARN_ONCE(dev, condition, format, arg...) \
	WARN_ONCE(condition, "%s: " format, dev_name(dev), ## arg)
#define dev_err_probe(dev, err, fmt, ...) \
	({ int __e = (err); dev_err(dev, fmt, ##__VA_ARGS__); __e; })
#define dev_warn_probe(dev, err, fmt, ...) \
	({ int __e = (err); dev_warn(dev, fmt, ##__VA_ARGS__); __e; })
#define dynamic_dev_dbg(dev, fmt, ...)	dev_dbg(dev, fmt, ##__VA_ARGS__)
#define DEFINE_DYNAMIC_DEBUG_METADATA(name, fmt)
#define DYNAMIC_DEBUG_BRANCH(descriptor)	false
#define __dynamic_func_call_cls(id, cls, fmt, func, ...)	do { } while (0)
#define _dynamic_func_call_cls(cls, fmt, func, ...)	do { } while (0)
#define _dynamic_func_call_no_desc(fmt, func, ...)	do { } while (0)
#define _dynamic_func_call_cls_no_desc(cls, fmt, func, ...)	do { } while (0)
struct ddebug_class_map { int dummy; };
#define DECLARE_DYNDBG_CLASSMAP(_var, _type, _base, ...)
#define DYNDBG_CLASSMAP_DEFINE(_var, _type, _base, ...)
#define DYNDBG_CLASSMAP_USE(_var)

/* #pragma mark - managed resources */

typedef void (*dr_release_t)(struct device *dev, void *res);
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);
void *devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid);
#define devres_alloc(release, size, gfp)	devres_alloc_node(release, size, gfp, NUMA_NO_NODE)
void devres_free(void *res);
void devres_add(struct device *dev, void *res);
int devres_destroy(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data);
int devres_release(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data);
void *devres_find(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data);
void devres_release_all(struct device *dev);
void *devres_open_group(struct device *dev, void *id, gfp_t gfp);
void devres_close_group(struct device *dev, void *id);
void devres_remove_group(struct device *dev, void *id);
int devres_release_group(struct device *dev, void *id);
int devm_add_action(struct device *dev, void (*action)(void *), void *data);
int devm_add_action_or_reset(struct device *dev, void (*action)(void *),
	void *data);
void devm_remove_action(struct device *dev, void (*action)(void *), void *data);
void devm_release_action(struct device *dev, void (*action)(void *), void *data);
void *devm_kmalloc(struct device *dev, size_t size, gfp_t gfp) __malloc;
#define devm_kzalloc(dev, size, gfp)	devm_kmalloc(dev, size, (gfp) | __GFP_ZERO)
#define devm_kcalloc(dev, n, size, gfp)	devm_kmalloc(dev, array_size(n, size), (gfp) | __GFP_ZERO)
#define devm_kmalloc_array(dev, n, size, gfp)	devm_kmalloc(dev, array_size(n, size), gfp)
void devm_kfree(struct device *dev, const void *p);
char *devm_kstrdup(struct device *dev, const char *s, gfp_t gfp) __malloc;
char *devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...) __printf(3, 4);
#define devm_ioremap(dev, offset, size)		ioremap(offset, size)
#define devm_ioremap_wc(dev, offset, size)	ioremap_wc(offset, size)
#define devm_iounmap(dev, addr)				iounmap(addr)
#define devm_kmemdup(dev, src, len, gfp) \
	({ void *__p = devm_kmalloc(dev, len, gfp); if (__p) memcpy(__p, src, len); __p; })

/* #pragma mark - firmware */

struct firmware {
	size_t size;
	const u8 *data;
	void *priv;
};
int request_firmware(const struct firmware **fw, const char *name,
	struct device *device);
int firmware_request_nowarn(const struct firmware **fw, const char *name,
	struct device *device);
#define request_firmware_direct(fw, name, dev)	firmware_request_nowarn(fw, name, dev)
#define request_firmware_into_buf(fw, name, dev, buf, size)	(-ENOSYS)
int request_firmware_nowait(struct module *module, bool uevent, const char *name,
	struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context));
void release_firmware(const struct firmware *fw);
#define firmware_request_platform(fw, name, dev)	request_firmware(fw, name, dev)

/* #pragma mark - power management (devices stay powered) */

typedef struct pm_message { int event; } pm_message_t;
struct dev_pm_ops {
	int (*prepare)(struct device *dev);
	void (*complete)(struct device *dev);
	int (*suspend)(struct device *dev);
	int (*resume)(struct device *dev);
	int (*freeze)(struct device *dev);
	int (*thaw)(struct device *dev);
	int (*poweroff)(struct device *dev);
	int (*restore)(struct device *dev);
	int (*suspend_late)(struct device *dev);
	int (*resume_early)(struct device *dev);
	int (*freeze_late)(struct device *dev);
	int (*thaw_early)(struct device *dev);
	int (*poweroff_late)(struct device *dev);
	int (*restore_early)(struct device *dev);
	int (*suspend_noirq)(struct device *dev);
	int (*resume_noirq)(struct device *dev);
	int (*freeze_noirq)(struct device *dev);
	int (*thaw_noirq)(struct device *dev);
	int (*poweroff_noirq)(struct device *dev);
	int (*restore_noirq)(struct device *dev);
	int (*runtime_suspend)(struct device *dev);
	int (*runtime_resume)(struct device *dev);
	int (*runtime_idle)(struct device *dev);
};
struct dev_pm_domain {
	struct dev_pm_ops ops;
	int (*start)(struct device *dev);
	void (*detach)(struct device *dev, bool power_off);
	int (*activate)(struct device *dev);
	void (*sync)(struct device *dev);
	void (*dismiss)(struct device *dev);
	int (*set_performance_state)(struct device *dev, unsigned int state);
};
#define dev_pm_domain_set(dev, pd)	do { } while (0)
#define PMSG_SUSPEND	((struct pm_message){ .event = 2 })
#define PMSG_FREEZE		((struct pm_message){ .event = 1 })
#define PMSG_HIBERNATE	((struct pm_message){ .event = 4 })
#define PMSG_ON			((struct pm_message){ .event = 0 })
#define SET_SYSTEM_SLEEP_PM_OPS(s, r)
#define SET_RUNTIME_PM_OPS(s, r, i)
#define SYSTEM_SLEEP_PM_OPS(s, r)
#define RUNTIME_PM_OPS(s, r, i)
#define pm_sleep_ptr(p)		NULL
#define pm_ptr(p)			(p)
#define pm_runtime_get_sync(dev)		0
#define pm_runtime_resume_and_get(dev)	0
#define pm_runtime_get_noresume(dev)	do { } while (0)
#define pm_runtime_get(dev)				0
#define pm_runtime_get_if_active(dev)	1
#define pm_runtime_get_if_in_use(dev)	1
#define pm_runtime_put(dev)				0
#define pm_runtime_put_sync(dev)		0
#define pm_runtime_put_noidle(dev)		do { } while (0)
#define pm_runtime_put_autosuspend(dev)	0
#define pm_runtime_put_sync_autosuspend(dev)	0
#define pm_runtime_mark_last_busy(dev)	do { } while (0)
#define pm_runtime_use_autosuspend(dev)	do { } while (0)
#define pm_runtime_dont_use_autosuspend(dev)	do { } while (0)
#define pm_runtime_set_autosuspend_delay(dev, d)	do { } while (0)
#define pm_runtime_set_active(dev)		0
#define pm_runtime_set_suspended(dev)	0
#define pm_runtime_allow(dev)			do { } while (0)
#define pm_runtime_forbid(dev)			do { } while (0)
#define pm_runtime_enable(dev)			do { } while (0)
#define pm_runtime_disable(dev)			do { } while (0)
#define pm_runtime_suspended(dev)		false
#define pm_runtime_active(dev)			true
#define pm_runtime_status_suspended(dev)	false
#define pm_runtime_enabled(dev)			false
#define pm_runtime_suspend(dev)			0
#define pm_runtime_resume(dev)			0
#define pm_runtime_idle(dev)			0
#define pm_runtime_autosuspend(dev)		0
#define pm_runtime_autosuspend_expiration(dev)	0
#define ksys_sync_helper()	do { } while (0)
#define pm_runtime_get_noresume(dev)	do { } while (0)
#define pm_runtime_irq_safe(dev)		do { } while (0)
#define pm_runtime_no_callbacks(dev)	do { } while (0)
#define pm_suspend_via_firmware()		false
#define pm_resume_via_firmware()		false
#define pm_suspend_target_state			0
#define pm_suspend_default_s2idle()		false
#define pm_hibernate_is_recovering()	false
#define pm_hibernation_mode_is_suspend()	false
#define PM_SUSPEND_ON		0
#define PM_SUSPEND_TO_IDLE	1
#define PM_SUSPEND_MEM		3
#define dev_pm_set_driver_flags(dev, flags)	do { } while (0)
#define DPM_FLAG_NO_DIRECT_COMPLETE	1
#define DPM_FLAG_SMART_SUSPEND		4
#define DPM_FLAG_MAY_SKIP_RESUME	8
#define device_pm_wait_for_dev(a, b)	0

#endif /* _LX_DEVICE_H */
