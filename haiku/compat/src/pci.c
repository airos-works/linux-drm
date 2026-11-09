/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Linux's PCI API on Haiku's PCI bus manager. A pci_dev is made on first
	use for any device the drivers ask about (the GPU, its audio function,
	the bridges above it) and kept. A bus knows the bridge above it, found
	by its secondary bus number. */

#include <lx/haiku.h>


static LIST_HEAD(lx_pci_devices);
static LIST_HEAD(lx_pci_buses);
static DEFINE_MUTEX(lx_pci_lock);

struct lx_pci_device {
	struct list_head entry;
	struct pci_dev pdev;
};

struct lx_pci_bus {
	struct list_head entry;
	struct pci_bus bus;
};

static struct pci_driver *lx_pci_driver;


static inline int
lx_bus(const struct pci_dev *dev)
{
	return dev->bus->number;
}


int
pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val)
{
	*val = lx_haiku_pci_read(lx_bus(dev), PCI_SLOT(dev->devfn),
		PCI_FUNC(dev->devfn), where, 4);
	return 0;
}


int
pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
	*val = lx_haiku_pci_read(lx_bus(dev), PCI_SLOT(dev->devfn),
		PCI_FUNC(dev->devfn), where, 2);
	return 0;
}


int
pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
	*val = lx_haiku_pci_read(lx_bus(dev), PCI_SLOT(dev->devfn),
		PCI_FUNC(dev->devfn), where, 1);
	return 0;
}


int
pci_write_config_dword(const struct pci_dev *dev, int where, u32 val)
{
	lx_haiku_pci_write(lx_bus(dev), PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn),
		where, 4, val);
	return 0;
}


int
pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
	lx_haiku_pci_write(lx_bus(dev), PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn),
		where, 2, val);
	return 0;
}


int
pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
	lx_haiku_pci_write(lx_bus(dev), PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn),
		where, 1, val);
	return 0;
}


u8
pci_find_capability(struct pci_dev *dev, int cap)
{
	u16 status;
	u8 pos;
	int ttl = 48;
	pci_read_config_word(dev, PCI_STATUS, &status);
	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;
	pci_read_config_byte(dev, PCI_CAPABILITY_LIST, &pos);
	while (ttl-- > 0 && pos >= 0x40) {
		u16 header;
		pos &= ~3;
		pci_read_config_word(dev, pos, &header);
		if ((header & 0xff) == 0xff)
			break;
		if ((header & 0xff) == cap)
			return pos;
		pos = header >> 8;
	}
	return 0;
}


u16
pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	int pos = PCI_CFG_SPACE_SIZE;
	int ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;
	u32 header;
	if (!pci_is_pcie(dev))
		return 0;
	pci_read_config_dword(dev, pos, &header);
	if (header == 0 || header == 0xffffffff)
		return 0;
	while (ttl-- > 0) {
		if (PCI_EXT_CAP_ID(header) == cap && pos != 0)
			return pos;
		pos = PCI_EXT_CAP_NEXT(header);
		if (pos < PCI_CFG_SPACE_SIZE)
			break;
		pci_read_config_dword(dev, pos, &header);
	}
	return 0;
}


/* #pragma mark - devices and buses */


static struct pci_bus *lx_pci_get_bus(int number);

static struct pci_dev *
lx_pci_dev_create(const struct lx_haiku_pci_info *info)
{
	struct lx_pci_device *entry;
	struct pci_dev *pdev;
	int bar;
	u16 flags;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL)
		return NULL;
	pdev = &entry->pdev;
	pdev->devfn = PCI_DEVFN(info->device, info->function);
	pdev->vendor = info->vendor_id;
	pdev->device = info->device_id;
	pdev->subsystem_vendor = info->subsystem_vendor_id;
	pdev->subsystem_device = info->subsystem_id;
	pdev->class = (info->class_base << 16) | (info->class_sub << 8)
		| info->class_api;
	pdev->revision = info->revision;
	pdev->hdr_type = info->header_type & 0x7f;
	pdev->irq = info->interrupt_line;
	pdev->current_state = PCI_D0;
	device_initialize(&pdev->dev);
	dev_set_name(&pdev->dev, "0000:%02x:%02x.%d", info->bus, info->device,
		info->function);
	pdev->dev.lx_dma_mask = DMA_BIT_MASK(32);
	pdev->dev.dma_mask = &pdev->dev.lx_dma_mask;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);

	for (bar = 0; bar < 6; bar++) {
		struct resource *res = &pdev->resource[bar];
		if (info->bar_size[bar] == 0)
			continue;
		res->start = info->bar_address[bar];
		res->end = info->bar_address[bar] + info->bar_size[bar] - 1;
		res->flags = (info->bar_flags[bar] & 1) ? IORESOURCE_IO
			: IORESOURCE_MEM;
		if (info->bar_flags[bar] & 2)
			res->flags |= IORESOURCE_MEM_64;
		if (info->bar_flags[bar] & 4)
			res->flags |= IORESOURCE_PREFETCH;
	}

	/* the bus before capabilities: config access needs it */
	pdev->bus = lx_pci_get_bus(info->bus);
	pdev->pcie_cap = pci_find_capability(pdev, PCI_CAP_ID_EXP);
	pdev->msi_cap = pci_find_capability(pdev, PCI_CAP_ID_MSI);
	pdev->msix_cap = pci_find_capability(pdev, PCI_CAP_ID_MSIX);
	if (pdev->pcie_cap) {
		pci_read_config_word(pdev, pdev->pcie_cap + PCI_EXP_FLAGS, &flags);
		pdev->pcie_flags_reg_lx = flags;
	}

	list_add_tail(&entry->entry, &lx_pci_devices);
	return pdev;
}


static struct pci_dev *
lx_pci_find_locked(int bus, unsigned int devfn)
{
	struct lx_pci_device *entry;
	struct lx_haiku_pci_info info;
	int index;

	list_for_each_entry(entry, &lx_pci_devices, entry) {
		if (entry->pdev.bus->number == bus && entry->pdev.devfn == devfn)
			return &entry->pdev;
	}
	for (index = 0; lx_haiku_pci_get_nth(index, &info) == 0; index++) {
		if (info.bus == bus && PCI_DEVFN(info.device, info.function) == devfn)
			return lx_pci_dev_create(&info);
	}
	return NULL;
}


/* the bridge whose secondary bus is "number", or NULL for a root bus */
static struct pci_dev *
lx_pci_find_bridge_locked(int number)
{
	struct lx_haiku_pci_info info;
	int index;
	for (index = 0; lx_haiku_pci_get_nth(index, &info) == 0; index++) {
		unsigned int secondary;
		if ((info.header_type & 0x7f) != PCI_HEADER_TYPE_BRIDGE)
			continue;
		secondary = lx_haiku_pci_read(info.bus, info.device, info.function,
			PCI_SECONDARY_BUS, 1);
		if ((int)secondary == number && info.bus != number)
			return lx_pci_find_locked(info.bus,
				PCI_DEVFN(info.device, info.function));
	}
	return NULL;
}


static struct pci_bus *
lx_pci_get_bus(int number)
{
	struct lx_pci_bus *entry;
	struct pci_dev *bridge;

	list_for_each_entry(entry, &lx_pci_buses, entry) {
		if (entry->bus.number == number)
			return &entry->bus;
	}
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL)
		return NULL;
	entry->bus.number = number;
	entry->bus.domain_nr = 0;
	entry->bus.max_bus_speed = PCI_SPEED_UNKNOWN;
	entry->bus.cur_bus_speed = PCI_SPEED_UNKNOWN;
	INIT_LIST_HEAD(&entry->bus.devices);
	list_add_tail(&entry->entry, &lx_pci_buses);

	bridge = lx_pci_find_bridge_locked(number);
	if (bridge != NULL) {
		entry->bus.self = bridge;
		entry->bus.parent = bridge->bus;
		entry->bus.primary = bridge->bus->number;
		bridge->subordinate = &entry->bus;
	}
	return &entry->bus;
}


/* the pci_dev of a device the Haiku driver found */
struct pci_dev *lx_pci_dev_get(int bus, int device, int function);

struct pci_dev *
lx_pci_dev_get(int bus, int device, int function)
{
	struct pci_dev *pdev;
	mutex_lock(&lx_pci_lock);
	pdev = lx_pci_find_locked(bus, PCI_DEVFN(device, function));
	mutex_unlock(&lx_pci_lock);
	return pdev;
}


struct pci_dev *
pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn)
{
	if (domain != 0)
		return NULL;
	return lx_pci_dev_get(bus, PCI_SLOT(devfn), PCI_FUNC(devfn));
}


static struct pci_dev *
lx_pci_get_next(struct pci_dev *from, bool (*match)(
	const struct lx_haiku_pci_info *, unsigned int, unsigned int),
	unsigned int a, unsigned int b)
{
	struct lx_haiku_pci_info info;
	int index;
	bool past_from = from == NULL;
	for (index = 0; lx_haiku_pci_get_nth(index, &info) == 0; index++) {
		if (!past_from) {
			if (from->bus->number == info.bus
				&& from->devfn == PCI_DEVFN(info.device, info.function))
				past_from = true;
			continue;
		}
		if (match(&info, a, b))
			return lx_pci_dev_get(info.bus, info.device, info.function);
	}
	return NULL;
}


static bool
match_class(const struct lx_haiku_pci_info *info, unsigned int class,
	unsigned int unused)
{
	return ((info->class_base << 16) | (info->class_sub << 8)
		| info->class_api) == class;
}


static bool
match_base_class(const struct lx_haiku_pci_info *info, unsigned int class,
	unsigned int unused)
{
	return info->class_base == (class >> 8);
}


static bool
match_ids(const struct lx_haiku_pci_info *info, unsigned int vendor,
	unsigned int device)
{
	return (vendor == (unsigned int)PCI_ANY_ID || info->vendor_id == vendor)
		&& (device == (unsigned int)PCI_ANY_ID || info->device_id == device);
}


struct pci_dev *
pci_get_class(unsigned int class, struct pci_dev *from)
{
	return lx_pci_get_next(from, match_class, class, 0);
}


struct pci_dev *
pci_get_base_class(unsigned int class, struct pci_dev *from)
{
	return lx_pci_get_next(from, match_base_class, class, 0);
}


struct pci_dev *
pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from)
{
	return lx_pci_get_next(from, match_ids, vendor, device);
}


struct pci_dev *
pci_upstream_bridge(struct pci_dev *dev)
{
	if (dev->bus == NULL)
		return NULL;
	return dev->bus->self;
}


struct pci_dev *
pcie_find_root_port(struct pci_dev *dev)
{
	struct pci_dev *bridge = pci_upstream_bridge(dev);
	while (bridge != NULL) {
		if (pci_is_pcie(bridge)
			&& pci_pcie_type(bridge) == PCI_EXP_TYPE_ROOT_PORT)
			return bridge;
		bridge = pci_upstream_bridge(bridge);
	}
	return NULL;
}


/* #pragma mark - PCI Express */


int
pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	*val = 0;
	if (!pci_is_pcie(dev))
		return -EINVAL;
	return pci_read_config_word(dev, dev->pcie_cap + pos, val);
}


int
pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	*val = 0;
	if (!pci_is_pcie(dev))
		return -EINVAL;
	return pci_read_config_dword(dev, dev->pcie_cap + pos, val);
}


int
pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	if (!pci_is_pcie(dev))
		return -EINVAL;
	return pci_write_config_word(dev, dev->pcie_cap + pos, val);
}


int
pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val)
{
	if (!pci_is_pcie(dev))
		return -EINVAL;
	return pci_write_config_dword(dev, dev->pcie_cap + pos, val);
}


int
pcie_capability_clear_and_set_word(struct pci_dev *dev, int pos, u16 clear,
	u16 set)
{
	u16 val;
	int error = pcie_capability_read_word(dev, pos, &val);
	if (error != 0)
		return error;
	return pcie_capability_write_word(dev, pos, (val & ~clear) | set);
}


int
pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos, u32 clear,
	u32 set)
{
	u32 val;
	int error = pcie_capability_read_dword(dev, pos, &val);
	if (error != 0)
		return error;
	return pcie_capability_write_dword(dev, pos, (val & ~clear) | set);
}


enum pci_bus_speed
pcie_get_speed_cap(struct pci_dev *dev)
{
	u32 lnkcap2, lnkcap;
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP2, &lnkcap2);
	if (lnkcap2) {
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_64_0GB)
			return PCIE_SPEED_64_0GT;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_32_0GB)
			return PCIE_SPEED_32_0GT;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_16_0GB)
			return PCIE_SPEED_16_0GT;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_8_0GB)
			return PCIE_SPEED_8_0GT;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_5_0GB)
			return PCIE_SPEED_5_0GT;
		if (lnkcap2 & PCI_EXP_LNKCAP2_SLS_2_5GB)
			return PCIE_SPEED_2_5GT;
		return PCI_SPEED_UNKNOWN;
	}
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	switch (lnkcap & PCI_EXP_LNKCAP_SLS) {
		case PCI_EXP_LNKCAP_SLS_5_0GB:
			return PCIE_SPEED_5_0GT;
		case PCI_EXP_LNKCAP_SLS_2_5GB:
			return PCIE_SPEED_2_5GT;
	}
	return PCI_SPEED_UNKNOWN;
}


/* Mb/s per lane after the encoding overhead, as Linux's PCIE_SPEED2MBS_ENC */
static u32
link_mbps(enum pci_bus_speed speed)
{
	switch (speed) {
		case PCIE_SPEED_64_0GT: return 64000 * 242 / 256;
		case PCIE_SPEED_32_0GT: return 32000 * 128 / 130;
		case PCIE_SPEED_16_0GT: return 16000 * 128 / 130;
		case PCIE_SPEED_8_0GT: return 8000 * 128 / 130;
		case PCIE_SPEED_5_0GT: return 5000 * 8 / 10;
		case PCIE_SPEED_2_5GT: return 2500 * 8 / 10;
		default: return 0;
	}
}


static enum pci_bus_speed
link_speed(u16 status)
{
	switch (status & PCI_EXP_LNKSTA_CLS) {
		case 1: return PCIE_SPEED_2_5GT;
		case 2: return PCIE_SPEED_5_0GT;
		case 3: return PCIE_SPEED_8_0GT;
		case 4: return PCIE_SPEED_16_0GT;
		case 5: return PCIE_SPEED_32_0GT;
		case 6: return PCIE_SPEED_64_0GT;
		default: return PCI_SPEED_UNKNOWN;
	}
}


/*	Linux's: the bandwidth of the slowest link between the device and the
	root, with that link's current speed and width. */
u32
pcie_bandwidth_available(struct pci_dev *dev, struct pci_dev **limiting_dev,
	enum pci_bus_speed *speed, enum pcie_link_width *width)
{
	u32 bandwidth = 0;
	if (speed != NULL)
		*speed = PCI_SPEED_UNKNOWN;
	if (width != NULL)
		*width = PCIE_LNK_WIDTH_UNKNOWN;
	for (; dev != NULL; dev = pci_upstream_bridge(dev)) {
		u16 status = 0;
		enum pci_bus_speed next_speed;
		enum pcie_link_width next_width;
		u32 next;
		if (!pci_is_pcie(dev)
			|| pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &status) != 0)
			continue;
		next_speed = link_speed(status);
		next_width = (status & PCI_EXP_LNKSTA_NLW) >> PCI_EXP_LNKSTA_NLW_SHIFT;
		next = next_width * link_mbps(next_speed);
		if (next == 0)
			continue;
		if (bandwidth == 0 || next < bandwidth) {
			bandwidth = next;
			if (limiting_dev != NULL)
				*limiting_dev = dev;
			if (speed != NULL)
				*speed = next_speed;
			if (width != NULL)
				*width = next_width;
		}
	}
	return bandwidth;
}


enum pcie_link_width
pcie_get_width_cap(struct pci_dev *dev)
{
	u32 lnkcap;
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	if (lnkcap)
		return (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4;
	return PCIE_LNK_WIDTH_UNKNOWN;
}


int
pcie_get_mps(struct pci_dev *dev)
{
	u16 ctl;
	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &ctl);
	return 128 << ((ctl & PCI_EXP_DEVCTL_PAYLOAD) >> 5);
}


/* #pragma mark - enabling, interrupts */


int
pci_enable_device(struct pci_dev *dev)
{
	u16 command;
	pci_read_config_word(dev, PCI_COMMAND, &command);
	command |= PCI_COMMAND_MEMORY | PCI_COMMAND_IO;
	pci_write_config_word(dev, PCI_COMMAND, command);
	dev->enable_cnt_lx++;
	return 0;
}


void
pci_disable_device(struct pci_dev *dev)
{
	/* Haiku's other drivers and the firmware display keep using it */
	if (dev->enable_cnt_lx > 0)
		dev->enable_cnt_lx--;
}


void
pci_set_master(struct pci_dev *dev)
{
	u16 command;
	pci_read_config_word(dev, PCI_COMMAND, &command);
	pci_write_config_word(dev, PCI_COMMAND, command | PCI_COMMAND_MASTER);
}


void
pci_clear_master(struct pci_dev *dev)
{
	u16 command;
	pci_read_config_word(dev, PCI_COMMAND, &command);
	pci_write_config_word(dev, PCI_COMMAND, command & ~PCI_COMMAND_MASTER);
}


/*	MSI-X, for drivers that ask for nothing else (amdxdna: one vector per
	mailbox channel).

	Linux writes an interrupt's message into the device's MSI-X table when
	the interrupt is requested, and again after every sleep. Haiku's PCI bus
	manager writes the whole table once, when the vectors are configured.
	For a device whose table cannot be relied on while it is powered down
	(the NPU is powered up by its driver, after the vectors were allocated),
	the vectors are therefore configured when the driver first asks for one
	of their numbers, which it does right before it requests the interrupt;
	and whenever it asks again, the table is looked at and written again if
	it no longer holds what Haiku put there. */

int
lx_pci_msix_vec_count(struct pci_dev *dev)
{
	/* Haiku's PCI bus manager sets up 32 vectors at most */
	return min(lx_haiku_pci_msix_count(lx_bus(dev), PCI_SLOT(dev->devfn),
		PCI_FUNC(dev->devfn)), 32);
}


/* an x86 MSI message for the vector after the previous entry's, unmasked */
static bool
msix_entry_plausible(const u32 *entry, const u32 *previous)
{
	if ((entry[0] & 0xfff00000) != 0xfee00000 || entry[1] != 0
		|| (entry[3] & PCI_MSIX_ENTRY_CTRL_MASKBIT) != 0)
		return false;
	return previous == NULL
		|| (entry[0] == previous[0] && entry[2] == previous[2] + 1);
}


static int
msix_configure(struct pci_dev *dev)
{
	unsigned int count = dev->lx_irq_count, i;
	bool plausible = true;
	int vector;
	u32 table;

	vector = lx_haiku_pci_enable_msix(lx_bus(dev), PCI_SLOT(dev->devfn),
		PCI_FUNC(dev->devfn), count);
	if (vector < 0) {
		pr_err("%s: MSI-X: Haiku configured no %u vectors\n", pci_name(dev),
			count);
		return -ENOSPC;
	}
	dev->irq = vector;
	dev->msix_enabled = 1;

	/* the table: in the BAR and at the offset the capability names */
	pci_read_config_dword(dev, dev->msix_cap + PCI_MSIX_TABLE, &table);
	dev->lx_msix_saved = kcalloc(count, 4 * sizeof(u32), GFP_KERNEL);
	dev->lx_msix_table = ioremap(pci_resource_start(dev,
			table & PCI_MSIX_TABLE_BIR) + (table & PCI_MSIX_TABLE_OFFSET),
		count * PCI_MSIX_ENTRY_SIZE);
	if (dev->lx_msix_saved == NULL || dev->lx_msix_table == NULL)
		return 0;
	for (i = 0; i < count * 4; i++)
		dev->lx_msix_saved[i] = readl(dev->lx_msix_table + i * 4);
	for (i = 0; i < count && plausible; i++) {
		plausible = msix_entry_plausible(&dev->lx_msix_saved[i * 4],
			i > 0 ? &dev->lx_msix_saved[(i - 1) * 4] : NULL);
	}
	if (!plausible) {
		/* never written back: a wrong message address would have the
		 * device write its interrupts somewhere into memory */
		pr_warn("%s: MSI-X table does not read back as configured (entry 0: "
			"%#x %#x %#x %#x); it will not be restored\n", pci_name(dev),
			dev->lx_msix_saved[0], dev->lx_msix_saved[1],
			dev->lx_msix_saved[2], dev->lx_msix_saved[3]);
		kfree(dev->lx_msix_saved);
		dev->lx_msix_saved = NULL;
	}
	return 0;
}


/*	Writes the MSI-X table entries again where they are no longer what they
	were set to; returns how many it wrote, or a negative error. */
int
lx_pci_msix_restore(struct pci_dev *dev)
{
	unsigned int i, entry;
	int written = 0;
	u16 control;

	if (!dev->msix_enabled)
		return 0;
	if (dev->lx_msix_saved == NULL || dev->lx_msix_table == NULL)
		return -ENODEV;
	for (entry = 0; entry < dev->lx_irq_count; entry++) {
		void __iomem *base = dev->lx_msix_table + entry * PCI_MSIX_ENTRY_SIZE;
		const u32 *saved = &dev->lx_msix_saved[entry * 4];
		bool same = true;
		for (i = 0; i < 4; i++)
			same = same && readl(base + i * 4) == saved[i];
		if (same)
			continue;
		writel(saved[3] | PCI_MSIX_ENTRY_CTRL_MASKBIT,
			base + PCI_MSIX_ENTRY_VECTOR_CTRL);
		writel(saved[0], base + PCI_MSIX_ENTRY_LOWER_ADDR);
		writel(saved[1], base + PCI_MSIX_ENTRY_UPPER_ADDR);
		writel(saved[2], base + PCI_MSIX_ENTRY_DATA);
		writel(saved[3], base + PCI_MSIX_ENTRY_VECTOR_CTRL);
		written++;
	}
	/* and the capability's enable bit */
	pci_read_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS, &control);
	if ((control & PCI_MSIX_FLAGS_ENABLE) == 0
		|| (control & PCI_MSIX_FLAGS_MASKALL) != 0) {
		pci_write_config_word(dev, dev->msix_cap + PCI_MSIX_FLAGS,
			(control | PCI_MSIX_FLAGS_ENABLE) & ~PCI_MSIX_FLAGS_MASKALL);
		written++;
	}
	if (written > 0) {
		pr_warn("%s: %d MSI-X settings were lost (the device was without "
			"power?); written again\n", pci_name(dev), written);
	}
	return written;
}


int
pci_alloc_irq_vectors(struct pci_dev *dev, unsigned int min_vecs,
	unsigned int max_vecs, unsigned int flags)
{
	if ((flags & (PCI_IRQ_MSI | PCI_IRQ_MSIX | PCI_IRQ_INTX)) == PCI_IRQ_MSIX) {
		/* only counted here; see msix_configure() */
		int available = lx_pci_msix_vec_count(dev);
		unsigned int count;
		if (available <= 0 || dev->msix_cap == 0)
			return -ENOSPC;
		count = min_t(unsigned int, max_vecs, available);
		if (count < min_vecs)
			return -ENOSPC;
		dev->lx_irq_count = count;
		dev->lx_msix_wanted = 1;
		return count;
	}
	if (flags & (PCI_IRQ_MSI | PCI_IRQ_MSIX)) {
		int vector = lx_haiku_pci_enable_msi(lx_bus(dev),
			PCI_SLOT(dev->devfn), PCI_FUNC(dev->devfn));
		if (vector >= 0) {
			dev->irq = vector;
			dev->msi_enabled = 1;
			dev->lx_irq_count = 1;
			return 1;
		}
	}
	if ((flags & PCI_IRQ_INTX) && dev->irq != 0 && dev->irq != 0xff) {
		dev->lx_irq_count = 1;
		return 1;
	}
	return -ENOSPC;
}


void
pci_free_irq_vectors(struct pci_dev *dev)
{
	if (dev->msi_enabled || dev->msix_enabled) {
		lx_haiku_pci_disable_msi(lx_bus(dev), PCI_SLOT(dev->devfn),
			PCI_FUNC(dev->devfn));
		dev->msi_enabled = 0;
		dev->msix_enabled = 0;
	}
	if (dev->lx_msix_table != NULL) {
		iounmap(dev->lx_msix_table);
		dev->lx_msix_table = NULL;
	}
	kfree(dev->lx_msix_saved);
	dev->lx_msix_saved = NULL;
	dev->lx_msix_wanted = 0;
	dev->lx_irq_count = 0;
}


int
pci_irq_vector(struct pci_dev *dev, unsigned int nr)
{
	if (nr >= dev->lx_irq_count)
		return -EINVAL;
	if (dev->lx_msix_wanted) {
		static DEFINE_MUTEX(lock);
		int error = 0;
		mutex_lock(&lock);
		if (!dev->msix_enabled)
			error = msix_configure(dev);
		else if (dev->lx_msix_saved != NULL)
			lx_pci_msix_restore(dev);
		mutex_unlock(&lock);
		if (error != 0)
			return error;
		/* MSI-X vectors follow one another */
		return dev->irq + nr;
	}
	return dev->irq;
}


/* #pragma mark - expansion ROM */


void __iomem *
pci_map_rom(struct pci_dev *pdev, size_t *size)
{
	u32 rom, mask;
	void __iomem *mapping;
	u8 signature[2];

	pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &rom);
	if ((rom & PCI_ROM_ADDRESS_MASK) == 0)
		return NULL;

	pci_write_config_dword(pdev, PCI_ROM_ADDRESS, PCI_ROM_ADDRESS_MASK);
	pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &mask);
	pci_write_config_dword(pdev, PCI_ROM_ADDRESS, rom | PCI_ROM_ADDRESS_ENABLE);
	mask &= PCI_ROM_ADDRESS_MASK;
	if (mask == 0)
		return NULL;
	*size = ~mask + 1;

	mapping = ioremap(rom & PCI_ROM_ADDRESS_MASK, *size);
	if (mapping == NULL) {
		pci_write_config_dword(pdev, PCI_ROM_ADDRESS, rom);
		return NULL;
	}
	memcpy_fromio(signature, mapping, 2);
	if (signature[0] != 0x55 || signature[1] != 0xaa) {
		iounmap(mapping);
		pci_write_config_dword(pdev, PCI_ROM_ADDRESS, rom);
		return NULL;
	}
	return mapping;
}


void
pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom)
{
	u32 value;
	iounmap(rom);
	pci_read_config_dword(pdev, PCI_ROM_ADDRESS, &value);
	pci_write_config_dword(pdev, PCI_ROM_ADDRESS,
		value & ~PCI_ROM_ADDRESS_ENABLE);
}


/* #pragma mark - drivers */


int lx_pci_register_driver(struct pci_driver *driver);

int
lx_pci_register_driver(struct pci_driver *driver)
{
	lx_pci_driver = driver;
	return 0;
}


struct pci_driver *lx_pci_registered_driver(void);

struct pci_driver *
lx_pci_registered_driver(void)
{
	return lx_pci_driver;
}


const struct pci_device_id *lx_pci_match(struct pci_driver *driver,
	struct pci_dev *dev);

const struct pci_device_id *
lx_pci_match(struct pci_driver *driver, struct pci_dev *dev)
{
	const struct pci_device_id *id;
	for (id = driver->id_table; id->vendor != 0 || id->class_mask != 0; id++) {
		if (id->vendor != (u32)PCI_ANY_ID && id->vendor != dev->vendor)
			continue;
		if (id->device != (u32)PCI_ANY_ID && id->device != dev->device)
			continue;
		if (id->subvendor != (u32)PCI_ANY_ID
			&& id->subvendor != dev->subsystem_vendor)
			continue;
		if (id->subdevice != (u32)PCI_ANY_ID
			&& id->subdevice != dev->subsystem_device)
			continue;
		if (((id->class ^ dev->class) & id->class_mask) != 0)
			continue;
		return id;
	}
	return NULL;
}
