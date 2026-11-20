/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_NOUVEAU_H
#define _LX_NOUVEAU_H

/*	The parts of the Linux kernel API that only nouveau (haiku/nouveau) uses:
	a small maple tree, I2C client drivers, and stubs for the SoC (Tegra)
	interfaces its shared code names but never calls on a PCI GPU. */

/* #pragma mark - maple tree */

/*	A range map with the maple tree's interface, as much of it as nouveau's
	VM_BIND code uses (the regions of a VM: a few at a time). The ranges are a
	sorted list; the caller locks (MT_FLAGS_LOCK_EXTERN). */
struct maple_tree {
	struct list_head ma_ranges;
	unsigned int ma_flags;
	void *ma_external_lock;
};

struct ma_state {
	struct maple_tree *tree;
	unsigned long index;
	unsigned long last;
	int started;
};

#define MT_FLAGS_LOCK_EXTERN	0x300
#define MTREE_INIT(name, flags)	{ .ma_ranges = LIST_HEAD_INIT(name.ma_ranges), \
	.ma_flags = (flags) }
#define MA_STATE(name, mt, first, end) \
	struct ma_state name = { .tree = (mt), .index = (first), .last = (end), \
		.started = 0 }

static inline void mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
	INIT_LIST_HEAD(&mt->ma_ranges);
	mt->ma_flags = flags;
	mt->ma_external_lock = NULL;
}
#define mt_init(mt)	mt_init_flags(mt, 0)
#define mt_set_external_lock(mt, lock)	((mt)->ma_external_lock = (lock))
static inline bool mtree_empty(const struct maple_tree *mt)
{
	return list_empty(&mt->ma_ranges);
}

void *mas_walk(struct ma_state *mas);
void *mas_find(struct ma_state *mas, unsigned long max);
void *mas_erase(struct ma_state *mas);
int mas_store_gfp(struct ma_state *mas, void *entry, gfp_t gfp);
void __mt_destroy(struct maple_tree *mt);
#define mtree_destroy(mt)	__mt_destroy(mt)
#define mas_for_each(mas, entry, max) \
	while (((entry) = mas_find((mas), (max))) != NULL)

/* #pragma mark - I2C client drivers */

#include <linux/mod_devicetable.h>

struct i2c_driver {
	int (*probe)(struct i2c_client *client);
	void (*remove)(struct i2c_client *client);
	int (*detect)(struct i2c_client *client, struct i2c_board_info *info);
	const unsigned short *address_list;
	const struct i2c_device_id *id_table;
	struct device_driver driver;
};
#define to_i2c_driver(d)	container_of_const(d, struct i2c_driver, driver)
#define to_i2c_client(d)	container_of(d, struct i2c_client, dev)
#define i2c_client_has_driver(client)	((client) != NULL && !IS_ERR(client) \
	&& (client)->dev.driver != NULL)
#define i2c_add_driver(driver)	0
#define i2c_del_driver(driver)	do { } while (0)
#define i2c_get_clientdata(client)	dev_get_drvdata(&(client)->dev)
#define i2c_set_clientdata(client, data)	dev_set_drvdata(&(client)->dev, data)
int i2c_master_send(const struct i2c_client *client, const char *buf, int count);
int i2c_master_recv(const struct i2c_client *client, char *buf, int count);

/* #pragma mark - SoC interfaces (Tegra), never reached on PCI */

struct clk;
struct regulator;
struct reset_control;
#define clk_get_rate(clk)	0UL
#define clk_set_rate(clk, rate)	(-ENODEV)
#define clk_prepare_enable(clk)	(-ENODEV)
#define clk_disable_unprepare(clk)	do { } while (0)
#define devm_clk_get(dev, id)	ERR_PTR(-ENODEV)
#define regulator_get_voltage(r)	(-ENODEV)
#define regulator_set_voltage(r, min, max)	(-ENODEV)
#define reset_control_assert(r)	0
#define reset_control_deassert(r)	0
#define devm_reset_control_get(dev, id)	ERR_PTR(-ENODEV)
#define devm_reset_control_get_exclusive(dev, id)	ERR_PTR(-ENODEV)
#define tegra_dev_iommu_get_stream_id(dev, id)	false

/* #pragma mark - PCI option ROM */

/* pci_map_rom() turns the ROM's decoding on and off itself */
#define pci_enable_rom(pdev)	0
#define pci_disable_rom(pdev)	do { } while (0)

/* #pragma mark - LEDs (CONFIG_LEDS_CLASS is off) */

enum led_brightness { LED_OFF = 0, LED_ON = 1, LED_HALF = 127, LED_FULL = 255 };
struct led_classdev {
	const char *name;
	unsigned int brightness;
	unsigned int max_brightness;
	void (*brightness_set)(struct led_classdev *led_cdev,
		enum led_brightness brightness);
	enum led_brightness (*brightness_get)(struct led_classdev *led_cdev);
};

/* #pragma mark - MMU notifiers (CONFIG_DRM_NOUVEAU_SVM is off) */

struct mmu_notifier {
	const void *ops;
	struct mm_struct *mm;
};

/* #pragma mark - misc */

/* the GSP firmware images are ELF files */
#include <uapi/linux/elf.h>

/* the top of a team's address space (Haiku's USER_TOP on x86_64) */
#define TASK_SIZE	0x00007fffffffffffUL

u32 crc32_le(u32 crc, const void *data, size_t length);

/* the firmware's console was on this GPU */
#define video_is_primary_device(dev)	false

#endif /* _LX_NOUVEAU_H */
