/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_I915_H
#define _LX_I915_H

/*	The parts of the Linux kernel API that only i915 (haiku/i915) uses;
	the other add-ons do not see them. */

#ifdef LX_ADDON_I915

/* #pragma mark - CPU model tables */

#define X86_CPU_ID_FLAG_ENTRY_VALID	BIT(0)
#define X86_MATCH_VFM(vfm, data) { \
	.vendor = VFM_VENDOR(vfm), .family = VFM_FAMILY(vfm), \
	.model = VFM_MODEL(vfm), .steppings = X86_STEPPING_ANY, \
	.feature = X86_FEATURE_ANY, .flags = X86_CPU_ID_FLAG_ENTRY_VALID, \
	.type = X86_CPU_TYPE_ANY, .driver_data = (kernel_ulong_t)(data) }

static inline const struct x86_cpu_id *
x86_match_cpu(const struct x86_cpu_id *match)
{
	for (; match->flags & X86_CPU_ID_FLAG_ENTRY_VALID; match++) {
		if (match->vendor != X86_VENDOR_ANY
			&& match->vendor != boot_cpu_data.x86_vendor)
			continue;
		if (match->family && match->family != boot_cpu_data.x86)
			continue;
		if (match->model && match->model != boot_cpu_data.x86_model)
			continue;
		return match;
	}
	return NULL;
}


/* #pragma mark - PCI */

#ifndef PCIBIOS_DEVICE_NOT_FOUND
#define PCIBIOS_DEVICE_NOT_FOUND	0x86
#endif

static inline struct pci_dev *lx_pci_bus_dev(struct pci_bus *bus,
	unsigned int devfn)
{
	return pci_get_domain_bus_and_slot(bus->domain_nr, bus->number, devfn);
}
#define LX_PCI_BUS_CONFIG(name, type) \
static inline int pci_bus_##name(struct pci_bus *bus, unsigned int devfn, \
	int where, type val) \
{ \
	struct pci_dev *dev = lx_pci_bus_dev(bus, devfn); \
	if (dev == NULL) \
		return PCIBIOS_DEVICE_NOT_FOUND; \
	return pci_##name(dev, where, val); \
}
LX_PCI_BUS_CONFIG(read_config_byte, u8 *)
LX_PCI_BUS_CONFIG(read_config_word, u16 *)
LX_PCI_BUS_CONFIG(read_config_dword, u32 *)
LX_PCI_BUS_CONFIG(write_config_byte, u8)
LX_PCI_BUS_CONFIG(write_config_word, u16)
LX_PCI_BUS_CONFIG(write_config_dword, u32)

static inline const struct pci_device_id *pci_match_id(
	const struct pci_device_id *ids, struct pci_dev *dev)
{
	for (; ids->vendor || ids->subvendor || ids->class_mask; ids++) {
		if ((ids->vendor == PCI_ANY_ID || ids->vendor == dev->vendor)
			&& (ids->device == PCI_ANY_ID || ids->device == dev->device)
			&& (ids->subvendor == PCI_ANY_ID
				|| ids->subvendor == dev->subsystem_vendor)
			&& (ids->subdevice == PCI_ANY_ID
				|| ids->subdevice == dev->subsystem_device)
			&& !((ids->class ^ dev->class) & ids->class_mask))
			return ids;
	}
	return NULL;
}
static inline int pci_dev_present(const struct pci_device_id *ids)
{
	for (; ids->vendor || ids->subvendor || ids->class_mask; ids++) {
		struct pci_dev *dev = NULL;
		while ((dev = pci_get_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
			if (pci_match_id(ids, dev) == ids)
				return 1;
		}
	}
	return 0;
}
static inline void __iomem *pci_iomap_range(struct pci_dev *dev, int bar,
	unsigned long offset, unsigned long maxlen)
{
	resource_size_t start = pci_resource_start(dev, bar);
	resource_size_t len = pci_resource_len(dev, bar);
	if (len <= offset || !start)
		return NULL;
	len -= offset;
	if (maxlen && len > maxlen)
		len = maxlen;
	return ioremap(start + offset, len);
}
#define pci_iounmap(dev, addr)	iounmap(addr)
#ifndef PCI_STD_RESOURCES
#define PCI_STD_RESOURCES	0
#define PCI_STD_RESOURCE_END	5
#endif
#define devm_request_mem_region(dev, start, n, name) \
	request_mem_region(start, n, name)
static inline bool resource_contains(const struct resource *r1,
	const struct resource *r2)
{
	if (!r1->start && !r1->end)
		return false;
	return r1->start <= r2->start && r1->end >= r2->end;
}

/* #pragma mark - scheduling, waiting, RCU */

#define might_alloc(gfp)	do { } while (0)
#define init_rcu_head(head)	do { } while (0)
#define destroy_rcu_head(head)	do { } while (0)
#define __init_waitqueue_head(wq, name, key)	init_waitqueue_head(wq)
#define __var_waitqueue(var)	lx_var_waitqueue((void *)(var))
#define ___wait_var_event(var, condition, state, exclusive, ret, cmd) \
	___wait_event(*__var_waitqueue(var), condition, state, exclusive, ret, cmd)
#define synchronize_hardirq(irq)	({ synchronize_irq(irq); true; })
#define synchronize_srcu_expedited(s)	synchronize_srcu(s)
#define time_after32(a, b)	((s32)((u32)(b) - (u32)(a)) < 0)
#define time_before32(b, a)	time_after32(a, b)
#define seqprop_sequence(s)	READ_ONCE((s)->sequence)
static inline int atomic_dec_and_lock_irqsave_lx(atomic_t *cnt,
	spinlock_t *lock, unsigned long *flags)
{
	if (atomic_add_unless(cnt, -1, 1))
		return 0;
	spin_lock_irqsave(lock, *flags);
	if (atomic_dec_and_test(cnt))
		return 1;
	spin_unlock_irqrestore(lock, *flags);
	return 0;
}
#define atomic_dec_and_lock_irqsave(cnt, lock, flags) \
	atomic_dec_and_lock_irqsave_lx(cnt, lock, &(flags))
static inline int atomic_notifier_call_chain(struct atomic_notifier_head *nh,
	unsigned long val, void *v)
{
	struct notifier_block *nb;
	unsigned long flags;
	int ret = NOTIFY_DONE;
	spin_lock_irqsave(&nh->lock, flags);
	for (nb = nh->head; nb != NULL; nb = nb->next) {
		ret = nb->notifier_call(nb, val, v);
		if (ret & NOTIFY_STOP_MASK)
			break;
	}
	spin_unlock_irqrestore(&nh->lock, flags);
	return ret;
}
#define register_vmap_purge_notifier(nb)	0
#define unregister_vmap_purge_notifier(nb)	0
/* stop_machine(): i915 uses it only for GGTT updates on Broxton and
 * Cherryview with VT-d (a workaround); here it runs the function with
 * interrupts off */
static inline int stop_machine(int (*fn)(void *), void *data,
	const void *cpus)
{
	unsigned long flags;
	int ret;
	local_irq_save(flags);
	ret = fn(data);
	local_irq_restore(flags);
	return ret;
}
#define get_task_pid(task, type)	get_pid((task)->thread_pid)

/* #pragma mark - user memory */

#define user_write_access_begin(ptr, len)	user_access_begin(ptr, len)
#define user_write_access_end()	user_access_end()
#define user_read_access_begin(ptr, len)	user_access_begin(ptr, len)
#define user_read_access_end()	user_access_end()
#define __copy_to_user_inatomic(to, from, n)	copy_to_user(to, from, n)
#define copy_from_user_inatomic_nontemporal(to, from, n) \
	copy_from_user_inatomic(to, from, n)
#ifndef PROT_READ
#define PROT_READ	0x1
#define PROT_WRITE	0x2
#define MAP_SHARED	0x01
#endif
/* vm_mmap(): i915's legacy GEM_MMAP ioctl maps a buffer into the caller
 * here; the Haiku driver maps buffers its own way (DRM_HAIKU_IOCTL_MAP) */
#define vm_mmap(file, addr, len, prot, flag, offset)	((unsigned long)-ENODEV)
#define mmap_write_lock_killable(mm)	0
#define vfs_mmap(file, vma)	(-ENODEV)

/* #pragma mark - misc */

#define WARN_RATELIMIT(condition, format, ...)	WARN_ONCE(condition, format, ##__VA_ARGS__)
#ifndef TAINT_USER
#define TAINT_USER	6
#endif
#define PM_EVENT_SUSPEND	0x0002
#define PM_EVENT_FREEZE		0x0001
#define PM_EVENT_HIBERNATE	0x0004
#define PM_EVENT_PRETHAW	0x0008
#define perfmon_capable()	true
#define ratelimit_state_get_miss(rs)	0
#define seq_has_overflowed(m)	false
#define single_open_size(f, show, data, size)	single_open(f, show, data)
#define poll_timeout_us_atomic(op, cond, delay_us, timeout_us, delay_before_op) \
	poll_timeout_us(op, cond, delay_us, timeout_us, delay_before_op)
#define power_group_name	"power"
static inline unsigned int intlog10(u32 value)
{
	/* log10(value) in 8.24 fixed point (Linux's int_log.h), from log2 */
	u64 log2 = 0;
	unsigned int bits;
	if (!value)
		return 0;
	bits = 31 - __builtin_clz(value);
	/* integer part of log2, plus 24 fraction bits by squaring */
	{
		u64 x = (u64)value << (32 - bits);	/* 1.32 in [1, 2) */
		int i;
		log2 = (u64)bits << 24;
		for (i = 23; i >= 0; i--) {
			x = (x * x) >> 32;
			if (x >= (2ULL << 32)) {
				x >>= 1;
				log2 |= 1ULL << i;
			}
		}
	}
	/* log10(x) = log2(x) * log10(2); 646456993 = log10(2) << 31 */
	return (unsigned int)((log2 * 646456993ULL) >> 31);
}

/* sysctl tables (i915 perf's paranoid switch): not registered */
struct ctl_table {
	const char *procname;
	void *data;
	int maxlen;
	umode_t mode;
	void *proc_handler;
	void *extra1;
	void *extra2;
};
#define proc_dointvec_minmax	NULL
#define SYSCTL_ZERO	NULL
#define SYSCTL_ONE	NULL
#define register_sysctl(path, table)	((struct ctl_table_header *)NULL)
#define unregister_sysctl_table(header)	do { } while (0)
struct ctl_table_header;

/* VGA registers (the legacy VGA plane is turned off at start) */
#define VGA_MIS_R	0x3CC
#define VGA_MIS_W	0x3C2
#define VGA_SEQ_I	0x3C4
#define VGA_SEQ_D	0x3C5
#define VGA_SR01_SCREEN_OFF	0x20
#define VGA_MIS_COLOR	0x01
#define VGA_RSRC_LEGACY_IO	0x01
#define VGA_RSRC_LEGACY_MEM	0x02
#define VGA_RSRC_NORMAL_IO	0x04
#define VGA_RSRC_NORMAL_MEM	0x08

/* binary sysfs files: none */
#define device_create_bin_file(dev, attr)	0
#define device_remove_bin_file(dev, attr)	do { } while (0)

/* page table bits (i915 builds GPU PTEs with the CPU's PAT bits) */
#ifndef _PAGE_PWT
#define _PAGE_PWT	(1ULL << 3)
#define _PAGE_PCD	(1ULL << 4)
#define _PAGE_PAT	(1ULL << 7)
#endif

/* #pragma mark - PM QoS (no CPU latency requests on Haiku) */

#define PM_QOS_DEFAULT_VALUE	(-1)
struct pm_qos_request { int lx_active; };
struct dev_pm_qos_request { int lx_active; };
static inline void cpu_latency_qos_add_request(struct pm_qos_request *r, s32 v)
{
	r->lx_active = 1;
}
static inline void cpu_latency_qos_update_request(struct pm_qos_request *r,
	s32 v) { }
static inline void cpu_latency_qos_remove_request(struct pm_qos_request *r)
{
	r->lx_active = 0;
}
static inline bool cpu_latency_qos_request_active(struct pm_qos_request *r)
{
	return r->lx_active;
}

/* #pragma mark - PWM (SoC backlight controllers; none here) */

enum pwm_polarity { PWM_POLARITY_NORMAL, PWM_POLARITY_INVERSED };
struct pwm_state {
	u64 period;
	u64 duty_cycle;
	enum pwm_polarity polarity;
	bool enabled;
	bool usage_power;
};
struct pwm_device;
#define pwm_get(dev, con_id)	((struct pwm_device *)ERR_PTR(-ENODEV))
#define pwm_put(pwm)	do { } while (0)
#define pwm_apply_might_sleep(pwm, state)	(-ENODEV)
#define pwm_init_state(pwm, state)	memset((state), 0, sizeof(*(state)))
#define pwm_get_state(pwm, state)	memset((state), 0, sizeof(*(state)))
#define pwm_is_enabled(pwm)	false
static inline unsigned int pwm_get_relative_duty_cycle(
	const struct pwm_state *state, unsigned int scale)
{
	if (!state->period)
		return 0;
	return DIV_ROUND_CLOSEST_ULL((u64)state->duty_cycle * scale,
		state->period);
}
static inline int pwm_set_relative_duty_cycle(struct pwm_state *state,
	unsigned int duty_cycle, unsigned int scale)
{
	if (!scale || duty_cycle > scale)
		return -EINVAL;
	state->duty_cycle = DIV_ROUND_CLOSEST_ULL((u64)duty_cycle * state->period,
		scale);
	return 0;
}

/* #pragma mark - relay channels (GuC log; never created here) */

struct rchan_buf { void *start; size_t subbufs_produced; };
struct rchan { size_t subbuf_size; size_t n_subbufs; };
struct rchan_callbacks {
	int (*subbuf_start)(struct rchan_buf *buf, void *subbuf, void *prev_subbuf);
	struct dentry *(*create_buf_file)(const char *filename,
		struct dentry *parent, umode_t mode, struct rchan_buf *buf,
		int *is_global);
	int (*remove_buf_file)(struct dentry *dentry);
};
#define relay_open(base, parent, subbuf_size, n_subbufs, cb, priv)	\
	((struct rchan *)NULL)
#define relay_close(chan)	do { } while (0)
#define relay_flush(chan)	do { } while (0)
#define relay_reserve(chan, length)	((void *)NULL)
#define relay_write(chan, data, length)	do { } while (0)
#define relay_buf_full(buf)	1
extern const struct file_operations relay_file_operations;

/* #pragma mark - ACPI events, typed components */

#define ACPI_VIDEO_CLASS	"video"
struct acpi_bus_event {
	struct list_head node;
	char device_class[20];
	char bus_id[15];
	u32 type;
	u32 data;
};
#define component_add_typed(dev, ops, subcomponent)	0

/* #pragma mark - files */

#define MAX_LFS_FILESIZE	((loff_t)LLONG_MAX)
#define force_o_largefile()	true
#ifndef O_LARGEFILE
#define O_LARGEFILE	00100000
#endif
/* writes into a shmem file's pages (i915 fills firmware copies that way) */
ssize_t kernel_write(struct file *file, const void *buf, size_t count,
	loff_t *pos);

/* #pragma mark - perf events (i915's PMU is not registered) */

#ifndef LX_HAVE_STRUCT_PMU
struct pmu { int lx_unused; };
#endif

/* #pragma mark - platform glue for SoCs and laptops' power management */

/* the CPU frequency (i915 sets the ring/CPU frequency ratios from it) */
struct cpufreq_policy {
	struct {
		unsigned int max_freq;
	} cpuinfo;
};
#define cpufreq_cpu_get(cpu)	((struct cpufreq_policy *)NULL)
#define cpufreq_cpu_put(policy)	do { } while (0)
extern unsigned int tsc_khz;
/* Cherryview/Broxton PMIC MIPI sequences, the IOSF sideband (Valleyview) */
#define intel_soc_pmic_exec_mipi_pmic_seq_element(i2c, reg, value, mask) \
	(-ENODEV)
#define acpi_video_unregister()	do { } while (0)
#define acpi_video_register()	0
#define on_each_cpu(func, info, wait)	((func)(info))
/* the MCHBAR (Gen4-Gen5 bridges whose BIOS leaves it unset) */
#define pnp_range_reserved(start, end)	false
#define pci_bus_alloc_resource(bus, res, size, align, min, type, alignf, data) \
	(-ENODEV)
#define PCIBIOS_MIN_MEM	0x10000000
#define pcibios_align_resource	NULL
#define release_resource(res)	0
/* auxiliary devices (the GSC's MEI interface, Meteor Lake and discrete) */
struct auxiliary_device {
	struct device dev;
	const char *name;
	u32 id;
};
struct mei_aux_device {
	struct auxiliary_device aux_dev;
	int irq;
	struct resource bar;
	struct resource ext_op_mem;
	bool slow_firmware;
};
/* GPIOs and pin control of SoC display panels (Atom MIPI DSI panels): no
 * SoC GPIO controllers here */
struct gpio_desc;
enum gpiod_flags {
	GPIOD_ASIS = 0,
	GPIOD_IN = 1,
	GPIOD_OUT_LOW = 3,
	GPIOD_OUT_HIGH = 7,
};
enum gpio_lookup_flags {
	GPIO_ACTIVE_HIGH = 0,
	GPIO_ACTIVE_LOW = 1,
};
struct gpiod_lookup {
	const char *key;
	u16 chip_hwnum;
	const char *con_id;
	unsigned int idx;
	unsigned long flags;
};
struct gpiod_lookup_table {
	struct list_head list;
	const char *dev_id;
	struct gpiod_lookup table[];
};
#define GPIO_LOOKUP_IDX(_key, _chip_hwnum, _con_id, _idx, _flags) \
	(struct gpiod_lookup) { .key = _key, .chip_hwnum = _chip_hwnum, \
		.con_id = _con_id, .idx = _idx, .flags = _flags }
#define GPIO_LOOKUP(_key, _chip_hwnum, _con_id, _flags) \
	GPIO_LOOKUP_IDX(_key, _chip_hwnum, _con_id, 0, _flags)
#define gpiod_add_lookup_table(table)	do { } while (0)
#define gpiod_remove_lookup_table(table)	do { } while (0)
#define devm_gpiod_get(dev, con_id, flags)	((struct gpio_desc *)ERR_PTR(-ENODEV))
#define devm_gpiod_get_index(dev, con_id, idx, flags) \
	((struct gpio_desc *)ERR_PTR(-ENODEV))
#define gpiod_get(dev, con_id, flags)	((struct gpio_desc *)ERR_PTR(-ENODEV))
#define gpiod_put(desc)	do { } while (0)
#define gpiod_set_value(desc, value)	do { } while (0)
#define gpiod_set_value_cansleep(desc, value)	do { } while (0)
struct pinctrl;
struct pinctrl_map {
	const char *dev_name;
	const char *name;
	int type;
	const char *ctrl_dev_name;
	const char *group;
	const char *function;
};
#define PIN_MAP_MUX_GROUP(_dev, _state, _ctrl, _grp, _func) \
	{ .dev_name = _dev, .name = _state, .ctrl_dev_name = _ctrl, \
		.group = _grp, .function = _func }
#define pinctrl_register_mappings(maps, num)	0
#define pinctrl_unregister_mappings(maps)	do { } while (0)
#define devm_pinctrl_get_select(dev, name)	((struct pinctrl *)ERR_PTR(-ENODEV))
/* ACPI I2C resources (the DSI panel's I2C bus) */
struct acpi_resource;
struct acpi_resource_source { u8 index; u16 string_length; char *string_ptr; };
struct acpi_resource_i2c_serialbus {
	u16 slave_address;
	struct acpi_resource_source resource_source;
};
#define i2c_acpi_get_i2c_resource(ares, sb)	false
#define i2c_acpi_find_adapter_by_handle(handle)	((struct i2c_adapter *)NULL)
#define acpi_get_handle(parent, path, handle)	((acpi_status)1)
#define acpi_device_handle(adev)	((acpi_handle)NULL)
#define acpi_dev_get_resources(adev, list, fn, data)	(-ENODEV)
#define acpi_dev_free_resource_list(list)	do { } while (0)
#define i2c_get_adapter(nr)	((struct i2c_adapter *)NULL)
#define i2c_put_adapter(adapter)	do { } while (0)
/* the MIPI DSI bus (drm_mipi_dsi.c): its devices are made by the encoders
 * directly, the bus itself is not registered */
#define pm_generic_runtime_suspend	NULL
#define pm_generic_runtime_resume	NULL
#define pm_generic_suspend	NULL
#define pm_generic_resume	NULL
#define pm_generic_freeze	NULL
#define pm_generic_thaw	NULL
#define pm_generic_poweroff	NULL
#define pm_generic_restore	NULL
#define add_uevent_var(env, format, ...)	0
#define of_device_uevent_modalias(dev, env)	(-ENODEV)
#define of_driver_match_device(dev, drv)	0
#define device_for_each_child(dev, data, fn)	0
#define for_each_available_child_of_node(parent, child) \
	for ((child) = NULL; (child) != NULL; )
#define device_set_node(dev, node)	do { (dev)->fwnode = (node); } while (0)
#define postcore_initcall(fn) \
	static int (* const __maybe_unused lx_postcore_##fn)(void) = fn
#define backlight_enable(bd)	0
#define backlight_disable(bd)	0
#define fwnode_find_reference(fwnode, name, index) \
	((struct fwnode_handle *)ERR_PTR(-ENOENT))
#define fwnode_device_is_available(fwnode)	false
#define driver_register(drv)	0
#define driver_unregister(drv)	do { } while (0)
#define bus_register(bus)	0
#define bus_unregister(bus)	do { } while (0)
#define bus_find_device_by_of_node(bus, np)	((struct device *)NULL)
/* bus notifiers (the HuC waits for the GSC's MEI device on Meteor Lake) */
#define bus_register_notifier(bus, nb)	0
#define bus_unregister_notifier(bus, nb)	0
#define BUS_NOTIFY_ADD_DEVICE		0x00000001
#define BUS_NOTIFY_DEL_DEVICE		0x00000002
#define BUS_NOTIFY_BIND_DRIVER		0x00000004
#define BUS_NOTIFY_BOUND_DRIVER		0x00000005
#define BUS_NOTIFY_DRIVER_NOT_BOUND	0x00000007
#define BUS_NOTIFY_UNBIND_DRIVER	0x00000008
#define BUS_NOTIFY_UNBOUND_DRIVER	0x00000009

#endif /* LX_ADDON_I915 */

#endif /* _LX_I915_H */
