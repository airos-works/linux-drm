/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The driver model: devices and their managed resources (devres, with
	groups), kobjects and ksets (reference counts and release, no sysfs),
	firmware files from Haiku's firmware directories, module parameters. */

#include <lx/haiku.h>


struct module lx_this_module = { .name = KBUILD_MODNAME };


/* #pragma mark - devres */

struct devres {
	struct list_head entry;
	dr_release_t release;
	void *group;			/* a group marker when release is NULL */
	bool group_open;
	unsigned long long data[];	/* aligned for any type */
};


static void
devres_ensure(struct device *dev)
{
	if (dev->devres_head.next == NULL) {
		spin_lock_init(&dev->devres_lock);
		INIT_LIST_HEAD(&dev->devres_head);
	}
}


void *
devres_alloc_node(dr_release_t release, size_t size, gfp_t gfp, int nid)
{
	struct devres *dr = kmalloc(sizeof(*dr) + size, gfp | __GFP_ZERO);
	if (dr == NULL)
		return NULL;
	INIT_LIST_HEAD(&dr->entry);
	dr->release = release;
	return dr->data;
}


static struct devres *
to_devres(void *res)
{
	return container_of(res, struct devres, data);
}


void
devres_free(void *res)
{
	if (res != NULL)
		kfree(to_devres(res));
}


void
devres_add(struct device *dev, void *res)
{
	struct devres *dr = to_devres(res);
	devres_ensure(dev);
	spin_lock(&dev->devres_lock);
	list_add_tail(&dr->entry, &dev->devres_head);
	spin_unlock(&dev->devres_lock);
}


static struct devres *
find_dr(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data)
{
	struct devres *dr;
	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release != release)
			continue;
		if (match && !match(dev, dr->data, match_data))
			continue;
		return dr;
	}
	return NULL;
}


void *
devres_find(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data)
{
	struct devres *dr;
	devres_ensure(dev);
	spin_lock(&dev->devres_lock);
	dr = find_dr(dev, release, match, match_data);
	spin_unlock(&dev->devres_lock);
	return dr != NULL ? dr->data : NULL;
}


int
devres_destroy(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data)
{
	struct devres *dr;
	devres_ensure(dev);
	spin_lock(&dev->devres_lock);
	dr = find_dr(dev, release, match, match_data);
	if (dr != NULL)
		list_del_init(&dr->entry);
	spin_unlock(&dev->devres_lock);
	if (dr == NULL)
		return -ENOENT;
	kfree(dr);
	return 0;
}


int
devres_release(struct device *dev, dr_release_t release, dr_match_t match,
	void *match_data)
{
	struct devres *dr;
	devres_ensure(dev);
	spin_lock(&dev->devres_lock);
	dr = find_dr(dev, release, match, match_data);
	if (dr != NULL)
		list_del_init(&dr->entry);
	spin_unlock(&dev->devres_lock);
	if (dr == NULL)
		return -ENOENT;
	dr->release(dev, dr->data);
	kfree(dr);
	return 0;
}


/* releases everything after "from" (all if NULL), last added first */
static void
release_from(struct device *dev, struct devres *from)
{
	for (;;) {
		struct devres *dr = NULL;
		spin_lock(&dev->devres_lock);
		if (!list_empty(&dev->devres_head)) {
			dr = list_last_entry(&dev->devres_head, struct devres, entry);
			if (dr == from)
				dr = NULL;
			else
				list_del_init(&dr->entry);
		}
		spin_unlock(&dev->devres_lock);
		if (dr == NULL)
			break;
		if (dr->release != NULL)
			dr->release(dev, dr->data);
		kfree(dr);
	}
}


void
devres_release_all(struct device *dev)
{
	if (dev->devres_head.next == NULL)
		return;
	release_from(dev, NULL);
}


void *
devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
	struct devres *dr = kzalloc(sizeof(*dr), gfp);
	if (dr == NULL)
		return NULL;
	INIT_LIST_HEAD(&dr->entry);
	dr->group = id != NULL ? id : dr;
	dr->group_open = true;
	devres_ensure(dev);
	spin_lock(&dev->devres_lock);
	list_add_tail(&dr->entry, &dev->devres_head);
	spin_unlock(&dev->devres_lock);
	return dr->group;
}


static struct devres *
find_group(struct device *dev, void *id)
{
	struct devres *dr;
	list_for_each_entry_reverse(dr, &dev->devres_head, entry) {
		if (dr->release == NULL && dr->group != NULL
			&& (id == NULL ? dr->group_open : dr->group == id))
			return dr;
	}
	return NULL;
}


void
devres_close_group(struct device *dev, void *id)
{
	struct devres *dr;
	spin_lock(&dev->devres_lock);
	dr = find_group(dev, id);
	if (dr != NULL)
		dr->group_open = false;
	spin_unlock(&dev->devres_lock);
}


void
devres_remove_group(struct device *dev, void *id)
{
	struct devres *dr;
	spin_lock(&dev->devres_lock);
	dr = find_group(dev, id);
	if (dr != NULL)
		list_del_init(&dr->entry);
	spin_unlock(&dev->devres_lock);
	kfree(dr);
}


int
devres_release_group(struct device *dev, void *id)
{
	struct devres *marker;
	spin_lock(&dev->devres_lock);
	marker = find_group(dev, id);
	spin_unlock(&dev->devres_lock);
	if (marker == NULL)
		return -ENOENT;
	release_from(dev, marker);
	devres_remove_group(dev, marker->group);
	return 1;
}


struct action_devres {
	void (*action)(void *);
	void *data;
};


static void
devm_action_release(struct device *dev, void *res)
{
	struct action_devres *devres = res;
	devres->action(devres->data);
}


static int
devm_action_match(struct device *dev, void *res, void *p)
{
	struct action_devres *devres = res, *target = p;
	return devres->action == target->action && devres->data == target->data;
}


int
devm_add_action(struct device *dev, void (*action)(void *), void *data)
{
	struct action_devres *devres = devres_alloc(devm_action_release,
		sizeof(*devres), GFP_KERNEL);
	if (devres == NULL)
		return -ENOMEM;
	devres->action = action;
	devres->data = data;
	devres_add(dev, devres);
	return 0;
}


int
devm_add_action_or_reset(struct device *dev, void (*action)(void *),
	void *data)
{
	int error = devm_add_action(dev, action, data);
	if (error != 0)
		action(data);
	return error;
}


void
devm_remove_action(struct device *dev, void (*action)(void *), void *data)
{
	struct action_devres devres = { .action = action, .data = data };
	devres_destroy(dev, devm_action_release, devm_action_match, &devres);
}


void
devm_release_action(struct device *dev, void (*action)(void *), void *data)
{
	struct action_devres devres = { .action = action, .data = data };
	devres_release(dev, devm_action_release, devm_action_match, &devres);
}


static void
devm_kmalloc_release(struct device *dev, void *res)
{
}


static int
devm_kmalloc_match(struct device *dev, void *res, void *data)
{
	return res == data;
}


void *
devm_kmalloc(struct device *dev, size_t size, gfp_t gfp)
{
	void *res = devres_alloc(devm_kmalloc_release, size, gfp);
	if (res == NULL)
		return NULL;
	devres_add(dev, res);
	return res;
}


void
devm_kfree(struct device *dev, const void *p)
{
	devres_destroy(dev, devm_kmalloc_release, devm_kmalloc_match, (void *)p);
}


char *
devm_kstrdup(struct device *dev, const char *s, gfp_t gfp)
{
	size_t size;
	char *buf;
	if (s == NULL)
		return NULL;
	size = strlen(s) + 1;
	buf = devm_kmalloc(dev, size, gfp);
	if (buf != NULL)
		memcpy(buf, s, size);
	return buf;
}


char *
devm_kasprintf(struct device *dev, gfp_t gfp, const char *fmt, ...)
{
	va_list args;
	int length;
	char *p;
	va_start(args, fmt);
	length = vsnprintf(NULL, 0, fmt, args);
	va_end(args);
	p = devm_kmalloc(dev, length + 1, gfp);
	if (p == NULL)
		return NULL;
	va_start(args, fmt);
	vsnprintf(p, length + 1, fmt, args);
	va_end(args);
	return p;
}


/* #pragma mark - devices */


int
dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list args;
	char *name;
	va_start(args, fmt);
	name = kvasprintf(GFP_KERNEL, fmt, args);
	va_end(args);
	if (name == NULL)
		return -ENOMEM;
	kfree(dev->lx_name);
	dev->lx_name = name;
	dev->kobj.name = name;
	return 0;
}


static void
device_release(struct kobject *kobj)
{
	struct device *dev = kobj_to_dev(kobj);
	devres_release_all(dev);
	kfree(dev->lx_name);
	dev->lx_name = NULL;
	if (dev->release != NULL)
		dev->release(dev);
	else if (dev->type != NULL && dev->type->release != NULL)
		dev->type->release(dev);
}


static const struct kobj_type device_ktype = {
	.release = device_release,
};


void
device_initialize(struct device *dev)
{
	kobject_init(&dev->kobj, &device_ktype);
	devres_ensure(dev);
}


int
device_add(struct device *dev)
{
	if (dev->init_name != NULL && dev->lx_name == NULL)
		dev_set_name(dev, "%s", dev->init_name);
	dev->lx_registered = true;
	return 0;
}


void
device_del(struct device *dev)
{
	dev->lx_registered = false;
}


struct device *
get_device(struct device *dev)
{
	if (dev != NULL)
		kobject_get(&dev->kobj);
	return dev;
}


void
put_device(struct device *dev)
{
	if (dev != NULL)
		kobject_put(&dev->kobj);
}


/* #pragma mark - kobjects */


void
kobject_init(struct kobject *kobj, const struct kobj_type *ktype)
{
	kref_init(&kobj->kref);
	INIT_LIST_HEAD(&kobj->entry);
	kobj->ktype = ktype;
	kobj->state_initialized = 1;
}


int
kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
	va_list args;
	char *name;
	va_start(args, fmt);
	name = kvasprintf(GFP_KERNEL, fmt, args);
	va_end(args);
	kobj->name = name;
	kobj->parent = parent;
	if (kobj->kset != NULL) {
		spin_lock(&kobj->kset->list_lock);
		list_add_tail(&kobj->entry, &kobj->kset->list);
		spin_unlock(&kobj->kset->list_lock);
	}
	kobj->state_in_sysfs = 1;
	return 0;
}


int
kobject_init_and_add(struct kobject *kobj, const struct kobj_type *ktype,
	struct kobject *parent, const char *fmt, ...)
{
	va_list args;
	char *name;
	kobject_init(kobj, ktype);
	va_start(args, fmt);
	name = kvasprintf(GFP_KERNEL, fmt, args);
	va_end(args);
	return kobject_add(kobj, parent, "%s", name != NULL ? name : "");
}


struct kobject *
kobject_get(struct kobject *kobj)
{
	if (kobj != NULL && kobj->state_initialized)
		kref_get(&kobj->kref);
	return kobj;
}


static void
kobject_release(struct kref *kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);
	const struct kobj_type *ktype = kobj->ktype;
	const char *name = kobj->name;
	if (kobj->state_in_sysfs)
		kobject_del(kobj);
	if (ktype != NULL && ktype->release != NULL)
		ktype->release(kobj);
	/* device names belong to the device */
	(void)name;
}


void
kobject_put(struct kobject *kobj)
{
	if (kobj != NULL && kobj->state_initialized)
		kref_put(&kobj->kref, kobject_release);
}


void
kobject_del(struct kobject *kobj)
{
	if (kobj->kset != NULL && !list_empty(&kobj->entry)) {
		spin_lock(&kobj->kset->list_lock);
		list_del_init(&kobj->entry);
		spin_unlock(&kobj->kset->list_lock);
	}
	kobj->state_in_sysfs = 0;
}


int
kset_register(struct kset *kset)
{
	INIT_LIST_HEAD(&kset->list);
	spin_lock_init(&kset->list_lock);
	if (!kset->kobj.state_initialized)
		kobject_init(&kset->kobj, kset->kobj.ktype);
	kset->kobj.state_in_sysfs = 1;
	return 0;
}


void
kset_unregister(struct kset *kset)
{
	kobject_del(&kset->kobj);
	kobject_put(&kset->kobj);
}


static ssize_t
kobj_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct kobj_attribute *kattr = container_of(attr, struct kobj_attribute,
		attr);
	return kattr->show != NULL ? kattr->show(kobj, kattr, buf) : -EIO;
}


static ssize_t
kobj_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf,
	size_t count)
{
	struct kobj_attribute *kattr = container_of(attr, struct kobj_attribute,
		attr);
	return kattr->store != NULL ? kattr->store(kobj, kattr, buf, count) : -EIO;
}


const struct sysfs_ops kobj_sysfs_ops = {
	.show = kobj_attr_show,
	.store = kobj_attr_store,
};


/* #pragma mark - firmware */

static const char *const firmware_directories[] = {
	"/boot/home/config/non-packaged/data/firmware",
	"/boot/system/non-packaged/data/firmware",
	"/boot/system/data/firmware",
};


static int
load_firmware(const struct firmware **_fw, const char *name, bool warn)
{
	struct firmware *fw;
	char path[256];
	size_t i;

	*_fw = NULL;
	fw = kzalloc(sizeof(*fw), GFP_KERNEL);
	if (fw == NULL)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(firmware_directories); i++) {
		void *data;
		unsigned long size;
		snprintf(path, sizeof(path), "%s/%s", firmware_directories[i], name);
		if (lx_haiku_read_file(path, &data, &size) == 0) {
			fw->data = data;
			fw->size = size;
			*_fw = fw;
			return 0;
		}
	}
	kfree(fw);
	if (warn)
		pr_warn(KBUILD_MODNAME ": firmware %s not found\n", name);
	return -ENOENT;
}


int
request_firmware(const struct firmware **fw, const char *name,
	struct device *device)
{
	return load_firmware(fw, name, true);
}


int
firmware_request_nowarn(const struct firmware **fw, const char *name,
	struct device *device)
{
	return load_firmware(fw, name, false);
}


int
request_firmware_nowait(struct module *module, bool uevent, const char *name,
	struct device *device, gfp_t gfp, void *context,
	void (*cont)(const struct firmware *fw, void *context))
{
	const struct firmware *fw;
	load_firmware(&fw, name, true);
	cont(fw, context);
	return 0;
}


void
release_firmware(const struct firmware *fw)
{
	if (fw == NULL)
		return;
	lx_haiku_free((void *)fw->data);
	kfree(fw);
}


/* #pragma mark - module parameters */


int
param_set_int(const char *val, const struct kernel_param *kp)
{
	return kstrtoint(val, 0, (int *)kp->arg);
}


int
param_get_int(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%i\n", *(int *)kp->arg);
}


int
param_set_uint(const char *val, const struct kernel_param *kp)
{
	return kstrtouint(val, 0, (unsigned int *)kp->arg);
}


int
param_get_uint(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%u\n", *(unsigned int *)kp->arg);
}


int
param_set_bool(const char *val, const struct kernel_param *kp)
{
	return kstrtobool(val, (bool *)kp->arg);
}


int
param_get_bool(char *buffer, const struct kernel_param *kp)
{
	return sprintf(buffer, "%c\n", *(bool *)kp->arg ? 'Y' : 'N');
}


const struct kernel_param_ops param_ops_int = {
	.set = param_set_int,
	.get = param_get_int,
};
const struct kernel_param_ops param_ops_uint = {
	.set = param_set_uint,
	.get = param_get_uint,
};
const struct kernel_param_ops param_ops_bool = {
	.set = param_set_bool,
	.get = param_get_bool,
};
