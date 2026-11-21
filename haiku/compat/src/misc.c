/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Processors, random numbers, notifier chains, the I2C core, and what
	the DRM core and amdgpu expect from subsystems Haiku does not have
	(sysfs, in-kernel DRM clients, panel quirks, compute partitions). */

#include <lx/haiku.h>
#include <drm/drm_connector.h>
#include <drm/drm_client.h>
#include <drm/drm_utils.h>
#include <drm/drm_file.h>


int
lx_num_online_cpus(void)
{
	return lx_haiku_cpu_count();
}


int
lx_smp_processor_id(void)
{
	return lx_haiku_current_cpu();
}


void
get_random_bytes(void *buf, size_t length)
{
	lx_haiku_random(buf, length);
}


u32
get_random_u32(void)
{
	u32 value;
	lx_haiku_random(&value, sizeof(value));
	return value;
}


u64
get_random_u64(void)
{
	u64 value;
	lx_haiku_random(&value, sizeof(value));
	return value;
}


/* #pragma mark - notifier chains */


int
blocking_notifier_chain_register(struct blocking_notifier_head *nh,
	struct notifier_block *n)
{
	struct notifier_block **nl;
	down_write(&nh->rwsem);
	nl = &nh->head;
	while (*nl != NULL && (*nl)->priority >= n->priority)
		nl = &(*nl)->next;
	n->next = *nl;
	*nl = n;
	up_write(&nh->rwsem);
	return 0;
}


int
blocking_notifier_chain_unregister(struct blocking_notifier_head *nh,
	struct notifier_block *n)
{
	struct notifier_block **nl;
	int result = -ENOENT;
	down_write(&nh->rwsem);
	for (nl = &nh->head; *nl != NULL; nl = &(*nl)->next) {
		if (*nl == n) {
			*nl = n->next;
			result = 0;
			break;
		}
	}
	up_write(&nh->rwsem);
	return result;
}


int
blocking_notifier_call_chain(struct blocking_notifier_head *nh,
	unsigned long val, void *v)
{
	struct notifier_block *nb;
	int ret = NOTIFY_DONE;
	if (nh->head == NULL)
		return ret;
	down_read(&nh->rwsem);
	for (nb = nh->head; nb != NULL; nb = nb->next) {
		ret = nb->notifier_call(nb, val, v);
		if (ret & NOTIFY_STOP_MASK)
			break;
	}
	up_read(&nh->rwsem);
	return ret;
}


/* #pragma mark - I2C core */


int
i2c_add_adapter(struct i2c_adapter *adap)
{
	mutex_init(&adap->bus_lock.m);
	if (adap->timeout == 0)
		adap->timeout = HZ;
	return 0;
}


int
i2c_add_numbered_adapter(struct i2c_adapter *adap)
{
	return i2c_add_adapter(adap);
}


void
i2c_del_adapter(struct i2c_adapter *adap)
{
	mutex_destroy(&adap->bus_lock.m);
}


int
__i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	unsigned long start = jiffies;
	int ret, attempt;
	if (adap->algo == NULL || adap->algo->master_xfer == NULL)
		return -EOPNOTSUPP;
	for (ret = 0, attempt = 0; attempt <= adap->retries; attempt++) {
		ret = adap->algo->master_xfer(adap, msgs, num);
		if (ret != -EAGAIN)
			break;
		if (time_after(jiffies, start + adap->timeout))
			break;
	}
	return ret;
}


int
i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	int ret;
	mutex_lock(&adap->bus_lock.m);
	ret = __i2c_transfer(adap, msgs, num);
	mutex_unlock(&adap->bus_lock.m);
	return ret;
}


s32
i2c_smbus_read_byte_data(const struct i2c_client *client, u8 command)
{
	u8 value;
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &command },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = 1, .buf = &value },
	};
	int ret = i2c_transfer(client->adapter, msgs, 2);
	return ret == 2 ? value : (ret < 0 ? ret : -EIO);
}


s32
i2c_smbus_write_byte_data(const struct i2c_client *client, u8 command, u8 value)
{
	u8 buffer[2] = { command, value };
	struct i2c_msg msg = { .addr = client->addr, .flags = 0, .len = 2,
		.buf = buffer };
	int ret = i2c_transfer(client->adapter, &msg, 1);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}


/* #pragma mark - DRM without sysfs, clients and quirk tables */


int
drm_sysfs_init(void)
{
	return 0;
}


void
drm_sysfs_destroy(void)
{
}


static void
lx_minor_kdev_release(struct device *kdev)
{
	kfree(kdev);
}


struct device *
drm_sysfs_minor_alloc(struct drm_minor *minor)
{
	struct device *kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (kdev == NULL)
		return ERR_PTR(-ENOMEM);
	device_initialize(kdev);
	kdev->release = lx_minor_kdev_release;
	/* named as Linux's sysfs names it (i915 compares kobject names) */
	if (minor->type == DRM_MINOR_RENDER)
		dev_set_name(kdev, "renderD%d", minor->index);
	else if (minor->type == DRM_MINOR_ACCEL)
		dev_set_name(kdev, "accel%d", minor->index);
	else
		dev_set_name(kdev, "card%d", minor->index);
	return kdev;
}


#ifdef LX_ADDON_I915

/* i915 uses the connector's device (drm_connector::kdev), as Linux's sysfs
 * makes one: its name in messages, the parent of the DP AUX channel's I2C
 * adapter, the eDP panel's devres owner. A plain device here. */
static void
lx_connector_kdev_release(struct device *kdev)
{
	kfree(kdev);
}


int
drm_sysfs_connector_add(struct drm_connector *connector)
{
	struct device *kdev;

	if (connector->kdev != NULL)
		return 0;
	kdev = kzalloc(sizeof(*kdev), GFP_KERNEL);
	if (kdev == NULL)
		return -ENOMEM;
	device_initialize(kdev);
	kdev->parent = connector->dev->dev;
	kdev->release = lx_connector_kdev_release;
	dev_set_name(kdev, "card%d-%s", connector->dev->primary->index,
		connector->name);
	kdev->kobj.name = dev_name(kdev);
	connector->kdev = kdev;
	return 0;
}
#else
int
drm_sysfs_connector_add(struct drm_connector *connector)
{
	return 0;
}
#endif


int
drm_sysfs_connector_add_late(struct drm_connector *connector)
{
	return 0;
}


void
drm_sysfs_connector_remove_early(struct drm_connector *connector)
{
}


void
drm_sysfs_connector_remove(struct drm_connector *connector)
{
#ifdef LX_ADDON_I915
	struct device *kdev = connector->kdev;
	if (kdev == NULL)
		return;
	connector->kdev = NULL;
	devres_release_all(kdev);
	put_device(kdev);
#endif
}


void
drm_sysfs_lease_event(struct drm_device *dev)
{
}


/* the Haiku driver wants hot plug notifications */
void (*lx_drm_hotplug_hook)(struct drm_device *dev);

void
drm_sysfs_hotplug_event(struct drm_device *dev)
{
	if (lx_drm_hotplug_hook != NULL)
		lx_drm_hotplug_hook(dev);
}


void
drm_sysfs_connector_hotplug_event(struct drm_connector *connector)
{
	drm_sysfs_hotplug_event(connector->dev);
}


void
drm_sysfs_connector_property_event(struct drm_connector *connector,
	struct drm_property *property)
{
}


int
drm_client_init(struct drm_device *dev, struct drm_client_dev *client,
	const char *name, const struct drm_client_funcs *funcs)
{
	return -ENODEV;
}


void
drm_client_register(struct drm_client_dev *client)
{
}


void
drm_client_release(struct drm_client_dev *client)
{
}


int
drm_get_panel_orientation_quirk(int width, int height)
{
	return -ENODEV;
}


const struct drm_panel_backlight_quirk *
drm_get_panel_backlight_quirk(const struct drm_edid *edid)
{
	return ERR_PTR(-ENODEV);
}


/* compute partitions (MI300) */

int amdgpu_xcp_drm_dev_alloc(struct drm_device **ddev);
void amdgpu_xcp_drm_dev_free(struct drm_device *ddev);
void amdgpu_xcp_drv_release(void);

int
amdgpu_xcp_drm_dev_alloc(struct drm_device **ddev)
{
	return -ENODEV;
}


void
amdgpu_xcp_drm_dev_free(struct drm_device *ddev)
{
}


void
amdgpu_xcp_drv_release(void)
{
}


/* #pragma mark - backlight */


struct backlight_device *
backlight_device_register(const char *name, struct device *parent,
	void *devdata, const struct backlight_ops *ops,
	const struct backlight_properties *props)
{
	struct backlight_device *bd = kzalloc(sizeof(*bd), GFP_KERNEL);
	if (bd == NULL)
		return ERR_PTR(-ENOMEM);

	bd->ops = ops;
	if (props != NULL)
		bd->props = *props;
	bd->dev.parent = parent;
	dev_set_drvdata(&bd->dev, devdata);
	pr_info("backlight %s: brightness %d of %d\n", name,
		bd->props.brightness, bd->props.max_brightness);
	return bd;
}


void
backlight_device_unregister(struct backlight_device *bd)
{
	if (!IS_ERR_OR_NULL(bd))
		kfree(bd);
}


int
backlight_device_set_brightness(struct backlight_device *bd,
	unsigned long brightness)
{
	if (brightness > (unsigned long)bd->props.max_brightness)
		return -EINVAL;
	bd->props.brightness = brightness;
	return backlight_update_status(bd);
}
