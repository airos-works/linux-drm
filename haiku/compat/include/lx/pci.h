/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_PCI_H
#define _LX_PCI_H

/*	PCI devices on Haiku's PCI bus manager (haiku/compat/haiku/Pci.cpp).
	The Haiku driver makes the pci_dev of the GPU and of the bridges above
	it; configuration space goes through the bus manager. */

#include <linux/mod_devicetable.h>
#include <linux/pci_ids.h>
#include <uapi/linux/pci_regs.h>

#define PCI_ANY_ID	(~0)
#define PCI_ERROR_RESPONSE	(~0ULL)
#define PCI_SET_ERROR_RESPONSE(val)	(*(val) = ((typeof(*(val)))PCI_ERROR_RESPONSE))
#define PCI_POSSIBLE_ERROR(val)	((val) == ((typeof(val))PCI_ERROR_RESPONSE))
#define PCI_DEVFN(slot, func)	((((slot) & 0x1f) << 3) | ((func) & 0x07))
#define PCI_SLOT(devfn)		(((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn)		((devfn) & 0x07)
#define PCI_NUM_RESOURCES	17
#define PCI_ROM_RESOURCE	6
#define PCI_STD_NUM_BARS	6
#define PCI_IRQ_INTX		(1 << 0)
#define PCI_IRQ_LEGACY		PCI_IRQ_INTX
#define PCI_IRQ_MSI			(1 << 1)
#define PCI_IRQ_MSIX		(1 << 2)
#define PCI_IRQ_AFFINITY	(1 << 3)
#define PCI_IRQ_ALL_TYPES	(PCI_IRQ_INTX | PCI_IRQ_MSI | PCI_IRQ_MSIX)

typedef int pci_power_t;
#define PCI_D0		((pci_power_t)0)
#define PCI_D1		((pci_power_t)1)
#define PCI_D2		((pci_power_t)2)
#define PCI_D3hot	((pci_power_t)3)
#define PCI_D3cold	((pci_power_t)4)
#define PCI_UNKNOWN	((pci_power_t)5)
typedef unsigned int pci_channel_state_t;
#define pci_channel_io_normal	((pci_channel_state_t)1)
#define pci_channel_io_frozen	((pci_channel_state_t)2)
#define pci_channel_io_perm_failure	((pci_channel_state_t)3)
typedef unsigned int pci_ers_result_t;
#define PCI_ERS_RESULT_NONE		((pci_ers_result_t)1)
#define PCI_ERS_RESULT_CAN_RECOVER	((pci_ers_result_t)2)
#define PCI_ERS_RESULT_NEED_RESET	((pci_ers_result_t)3)
#define PCI_ERS_RESULT_DISCONNECT	((pci_ers_result_t)4)
#define PCI_ERS_RESULT_RECOVERED	((pci_ers_result_t)5)

enum pci_bus_speed {
	PCIE_SPEED_2_5GT = 0x14,
	PCIE_SPEED_5_0GT = 0x15,
	PCIE_SPEED_8_0GT = 0x16,
	PCIE_SPEED_16_0GT = 0x17,
	PCIE_SPEED_32_0GT = 0x18,
	PCIE_SPEED_64_0GT = 0x19,
	PCI_SPEED_UNKNOWN = 0xff,
};
enum pcie_link_width {
	PCIE_LNK_WIDTH_RESRV = 0x00,
	PCIE_LNK_X1 = 0x01,
	PCIE_LNK_X2 = 0x02,
	PCIE_LNK_X4 = 0x04,
	PCIE_LNK_X8 = 0x08,
	PCIE_LNK_X12 = 0x0c,
	PCIE_LNK_X16 = 0x10,
	PCIE_LNK_X32 = 0x20,
	PCIE_LNK_WIDTH_UNKNOWN = 0xff,
};

struct pci_bus;
struct pci_driver;
struct pci_saved_state;

struct pci_dev {
	struct device dev;
	struct pci_bus *bus;
	struct pci_bus *subordinate;
	unsigned int devfn;
	unsigned short vendor;
	unsigned short device;
	unsigned short subsystem_vendor;
	unsigned short subsystem_device;
	unsigned int class;
	u8 revision;
	u8 hdr_type;
	u8 pcie_cap;
	u8 msi_cap;
	u8 msix_cap;
	u8 pcie_flags_reg_lx;
	u8 pin;
	int pcie_mpss;
	unsigned int irq;
	struct resource resource[PCI_NUM_RESOURCES];
	pci_power_t current_state;
	unsigned int bridge_d3:1;
	unsigned int d3cold_allowed:1;
	unsigned int no_d3cold:1;
	unsigned int is_physfn:1;
	unsigned int is_virtfn:1;
	unsigned int msi_enabled:1;
	unsigned int msix_enabled:1;
	unsigned int ltr_path:1;
	unsigned int is_hotplug_bridge:1;
	unsigned int is_thunderbolt:1;
	unsigned int untrusted:1;
	unsigned int enable_cnt_lx;
	u32 saved_config_space[16];
	phys_addr_t rom;
	size_t romlen;
	struct mfd_cell *mfd_cell;
	struct pci_driver *driver;
	void *lx_haiku;			/* Haiku's pci_info and module */
	unsigned int lx_irq_count;
	/* MSI-X: the table, and what its entries were set to */
	void __iomem *lx_msix_table;
	u32 *lx_msix_saved;
};

struct pci_bus {
	struct pci_bus *parent;
	struct pci_dev *self;
	unsigned char number;
	unsigned char primary;
	int domain_nr;
	enum pci_bus_speed max_bus_speed;
	enum pci_bus_speed cur_bus_speed;
	struct list_head devices;
};

struct pci_error_handlers {
	pci_ers_result_t (*error_detected)(struct pci_dev *dev, pci_channel_state_t error);
	pci_ers_result_t (*mmio_enabled)(struct pci_dev *dev);
	pci_ers_result_t (*slot_reset)(struct pci_dev *dev);
	void (*reset_prepare)(struct pci_dev *dev);
	void (*reset_done)(struct pci_dev *dev);
	void (*resume)(struct pci_dev *dev);
	void (*cor_error_detected)(struct pci_dev *dev);
};

struct pci_driver {
	const char *name;
	const struct pci_device_id *id_table;
	int (*probe)(struct pci_dev *dev, const struct pci_device_id *id);
	void (*remove)(struct pci_dev *dev);
	void (*shutdown)(struct pci_dev *dev);
	const struct pci_error_handlers *err_handler;
	struct device_driver driver;
	int (*sriov_configure)(struct pci_dev *dev, int num_vfs);
	const struct attribute_group **dev_groups;
	const struct attribute_group **groups;
};

#define to_pci_dev(n)		container_of(n, struct pci_dev, dev)
#define for_each_pci_dev(d)	while ((d = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, d)) != NULL)
#define PCI_DEVICE(vend, dev) \
	.vendor = (vend), .device = (dev), \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define PCI_DEVICE_DATA(vend, dev, data) \
	.vendor = PCI_VENDOR_ID_##vend, .device = PCI_DEVICE_ID_##vend##_##dev, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, .class = 0, \
	.class_mask = 0, .driver_data = (kernel_ulong_t)(data)
#define PCI_VDEVICE(vend, dev) \
	.vendor = PCI_VENDOR_ID_##vend, .device = (dev), \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, 0, 0
#define PCI_DEVICE_CLASS(dev_class, dev_class_mask) \
	.class = (dev_class), .class_mask = (dev_class_mask), \
	.vendor = PCI_ANY_ID, .device = PCI_ANY_ID, \
	.subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID
#define module_pci_driver(drv)
#define builtin_pci_driver(drv)

static inline void *pci_get_drvdata(struct pci_dev *pdev)
{
	return dev_get_drvdata(&pdev->dev);
}
static inline void pci_set_drvdata(struct pci_dev *pdev, void *data)
{
	dev_set_drvdata(&pdev->dev, data);
}
static inline const char *pci_name(const struct pci_dev *pdev)
{
	return dev_name(&pdev->dev);
}
#define pci_resource_start(dev, bar)	((dev)->resource[(bar)].start)
#define pci_resource_end(dev, bar)		((dev)->resource[(bar)].end)
#define pci_resource_flags(dev, bar)	((dev)->resource[(bar)].flags)
#define pci_resource_len(dev, bar) \
	((pci_resource_end((dev), (bar)) == 0) ? 0 : \
	 (pci_resource_end((dev), (bar)) - pci_resource_start((dev), (bar)) + 1))
#define pci_resource_n(dev, bar)		(&(dev)->resource[(bar)])
#define pci_bus_for_each_resource(bus, res, ...)	for ((res) = NULL; (res) != NULL; )
#define pci_domain_nr(bus)		((bus)->domain_nr)
#define pci_is_root_bus(pbus)	((pbus)->parent == NULL)
#define pci_dev_id(dev)			(((u16)(dev)->bus->number << 8) | (dev)->devfn)
#define pci_dev_get(dev)		(dev)
#define pci_dev_put(dev)		do { } while (0)
#define pci_is_pcie(dev)		((dev)->pcie_cap != 0)
#define pci_pcie_cap(dev)		((dev)->pcie_cap)
#define pci_pcie_type(dev)		(((dev)->pcie_flags_reg_lx & PCI_EXP_FLAGS_TYPE) >> 4)
#define pci_is_bridge(dev)		((dev)->hdr_type == PCI_HEADER_TYPE_BRIDGE)
#define pci_is_thunderbolt_attached(dev)	false
#define pci_dev_is_disconnected(dev)	false
#define pci_device_is_present(dev)		true
#define pci_is_enabled(dev)			((dev)->enable_cnt_lx > 0)
#define pci_channel_offline(dev)	false
#define pci_dev_is_added(dev)		true
#define pci_is_dev_assigned(dev)	false
#define pci_physfn(dev)				(dev)
#define pci_sriov_get_totalvfs(dev)	0
#define pci_num_vf(dev)				0
#define pci_iov_virtfn_devfn(dev, id)	(-ENOSYS)
#define pci_enable_sriov(dev, n)	(-ENOSYS)
#define pci_disable_sriov(dev)		do { } while (0)
#define pci_vfs_assigned(dev)		0

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val);
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val);
int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val);
int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val);
int pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear,
	u16 set);
int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear,
	u32 set);
#define pcie_capability_set_word(dev, pos, set) \
	pcie_capability_clear_and_set_word(dev, pos, 0, set)
#define pcie_capability_clear_word(dev, pos, clear) \
	pcie_capability_clear_and_set_word(dev, pos, clear, 0)
#define pcie_capability_set_dword(dev, pos, set) \
	pcie_capability_clear_and_set_dword(dev, pos, 0, set)
#define pcie_capability_clear_dword(dev, pos, clear) \
	pcie_capability_clear_and_set_dword(dev, pos, clear, 0)
u8 pci_find_capability(struct pci_dev *dev, int cap);
u16 pci_find_ext_capability(struct pci_dev *dev, int cap);
int pci_enable_device(struct pci_dev *dev);
#define pci_enable_device_mem(dev)	pci_enable_device(dev)
#define pcim_enable_device(dev)		pci_enable_device(dev)
void pci_disable_device(struct pci_dev *dev);
void pci_set_master(struct pci_dev *dev);
void pci_clear_master(struct pci_dev *dev);
#define pci_save_state(dev)			0
#define pci_restore_state(dev)		do { } while (0)
#define pci_store_saved_state(dev)	NULL
#define pci_load_saved_state(dev, state)	0
#define pci_load_and_free_saved_state(dev, state)	0
#define pci_restore_msi_state(dev)	do { } while (0)
#define pci_restore_config_space(dev)	do { } while (0)
#define pci_set_power_state(dev, state)	0
#define pci_wake_from_d3(dev, enable)	0
#define pci_prepare_to_sleep(dev)	0
#define pci_back_from_sleep(dev)	0
#define pci_d3cold_enable(dev)		do { } while (0)
#define pci_d3cold_disable(dev)		do { } while (0)
#define pci_ignore_hotplug(dev)		do { } while (0)
#define pci_pr3_present(dev)		false
#define pci_reset_function(dev)		(-ENOTTY)
#define pci_reset_function_locked(dev)	(-ENOTTY)
#define pci_wait_for_pending_transaction(dev)	1
#define pci_request_regions(dev, name)	0
#define pci_release_regions(dev)	do { } while (0)
#define pci_request_region(dev, bar, name)	0
#define pci_release_region(dev, bar)	do { } while (0)
#define pci_release_resource(dev, bar)	do { } while (0)
#define pci_resize_resource(dev, bar, size, ...)	(-ENOTSUPP)
#define pci_rebar_get_possible_sizes(dev, bar)	0
#define pci_rebar_bytes_to_size(bytes)	\
	(max(ilog2((u64)(bytes)), 20) - 20)
#define pci_rebar_size_to_bytes(size)	(1ULL << ((size) + 20))
#define pci_assign_unassigned_bus_resources(bus)	do { } while (0)
#define pci_enable_atomic_ops_to_root(dev, mask)	(-EINVAL)
#define pci_p2pdma_distance(provider, client, verbose)	(-1)
#define pci_p2pdma_distance_many(provider, clients, n, verbose)	(-1)
void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size);
void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom);
#define pci_platform_rom(pdev, size)	NULL
struct pci_dev *pci_upstream_bridge(struct pci_dev *dev);
struct pci_dev *pcie_find_root_port(struct pci_dev *dev);
struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus,
	unsigned int devfn);
struct pci_dev *pci_get_class(unsigned int class, struct pci_dev *from);
struct pci_dev *pci_get_base_class(unsigned int class, struct pci_dev *from);
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
	struct pci_dev *from);
#define pci_get_slot(bus, devfn) \
	pci_get_domain_bus_and_slot((bus)->domain_nr, (bus)->number, devfn)
int lx_pci_register_driver(struct pci_driver *driver);
#define pci_register_driver(drv)	lx_pci_register_driver(drv)
#define pci_unregister_driver(drv)	do { } while (0)
int pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
	unsigned int max_vecs, unsigned int flags);
void pci_free_irq_vectors(struct pci_dev *dev);
int pci_irq_vector(struct pci_dev *dev, unsigned int nr);
#define pci_enable_msi(dev)		pci_alloc_irq_vectors(dev, 1, 1, PCI_IRQ_MSI)
#define pci_disable_msi(dev)	pci_free_irq_vectors(dev)
#define pci_dev_msi_enabled(dev)	((dev)->msi_enabled || (dev)->msix_enabled)
#define pci_msix_vec_count(dev)	lx_pci_msix_vec_count(dev)
int lx_pci_msix_vec_count(struct pci_dev *dev);
int lx_pci_msix_restore(struct pci_dev *dev);
enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev);
enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev);
u32 pcie_bandwidth_available(struct pci_dev *dev, struct pci_dev **limiting_dev,
	enum pci_bus_speed *speed, enum pcie_link_width *width);
int pcie_get_mps(struct pci_dev *dev);
#define pcie_set_mps(dev, mps)	0
#define pcie_get_readrq(dev)	512
#define pcie_set_readrq(dev, rq)	0
#define pcie_aspm_enabled(dev)	false
#define pci_disable_link_state(dev, state)	0
#define pci_is_root_port(dev)	(pci_pcie_type(dev) == PCI_EXP_TYPE_ROOT_PORT)
#define pci_vpd_find_ro_info_keyword(buf, len, kw, size)	(-ENOENT)
#define pci_vpd_alloc(dev, size)	ERR_PTR(-ENODEV)
#define pci_dev_trylock(dev)	1
#define pci_dev_unlock(dev)		do { } while (0)
#define pci_dev_lock(dev)		do { } while (0)
#define pci_cfg_access_lock(dev)	do { } while (0)
#define pci_cfg_access_unlock(dev)	do { } while (0)
#define pcie_reset_flr(dev, probe)	(-ENOTTY)
#define pcie_flr(dev)			(-ENOTTY)
#define pci_bridge_secondary_bus_reset(dev)	(-ENOTTY)
#define pci_find_bus(domain, busnr)	NULL
#define pci_rescan_bus(bus)		0
#define pci_stop_and_remove_bus_device_locked(dev)	do { } while (0)
#define pci_lock_rescan_remove()	do { } while (0)
#define pci_unlock_rescan_remove()	do { } while (0)
#define pci_acs_path_enabled(start, end, flags)	false

/* VGA arbitration: one GPU owns the legacy VGA range */
#define VGA_RSRC_NONE			0x00
#define VGA_RSRC_LEGACY_IO		0x01
#define VGA_RSRC_LEGACY_MEM		0x02
#define VGA_RSRC_LEGACY_MASK	(VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM)
#define VGA_RSRC_NORMAL_IO		0x04
#define VGA_RSRC_NORMAL_MEM		0x08
#define vga_client_register(pdev, set_decode)	0
#define vga_client_unregister(pdev)	do { } while (0)
#define vga_get_uninterruptible(pdev, rsrc)	0
#define vga_put(pdev, rsrc)		do { } while (0)
#define vga_set_legacy_decoding(pdev, decodes)	do { } while (0)
#define vga_default_device()	NULL
#define vga_switcheroo_register_client(pdev, ops, driver_power_control)	0
#define vga_switcheroo_unregister_client(pdev)	do { } while (0)
#define vga_switcheroo_client_fb_set(pdev, info)	do { } while (0)
#define vga_switcheroo_init_domain_pm_ops(dev, domain)	(-EINVAL)
#define vga_switcheroo_fini_domain_pm_ops(dev)	do { } while (0)
#define vga_switcheroo_process_delayed_switch()	0
#define vga_switcheroo_handler_flags()	0
#define vga_switcheroo_client_probe_defer(pdev)	false
#define vga_switcheroo_lock_ddc(pdev)	do { } while (0)
#define vga_switcheroo_unlock_ddc(pdev)	0
#define VGA_SWITCHEROO_CAN_SWITCH_DDC	1
#define VGA_SWITCHEROO_ON	1
#define VGA_SWITCHEROO_OFF	0
enum vga_switcheroo_state { VGA_SWITCHEROO_OFF_LX, VGA_SWITCHEROO_ON_LX,
	VGA_SWITCHEROO_NOT_FOUND };
struct vga_switcheroo_client_ops {
	void (*set_gpu_state)(struct pci_dev *dev, enum vga_switcheroo_state);
	void (*reprobe)(struct pci_dev *dev);
	bool (*can_switch)(struct pci_dev *dev);
	void (*gpu_bound)(struct pci_dev *dev, int id);
};
#define apple_gmux_detect(pnp, info)	false

#endif /* _LX_PCI_H */
