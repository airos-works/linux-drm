/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_IO_H
#define _LX_IO_H

/*	Memory mapped and port I/O. ioremap() maps physical memory into the
	kernel with Haiku's map_physical_memory() (haiku/compat/haiku/Io.cpp). */

#define __iomem

#define __LX_MMIO_READ(name, type) \
static __always_inline type name(const volatile void __iomem *addr) \
{ \
	type value = *(const volatile type __force *)addr; \
	barrier(); \
	return value; \
}
#define __LX_MMIO_WRITE(name, type) \
static __always_inline void name(type value, volatile void __iomem *addr) \
{ \
	barrier(); \
	*(volatile type __force *)addr = value; \
}

__LX_MMIO_READ(readb, u8)
__LX_MMIO_READ(readw, u16)
__LX_MMIO_READ(readl, u32)
__LX_MMIO_READ(readq, u64)
__LX_MMIO_WRITE(writeb, u8)
__LX_MMIO_WRITE(writew, u16)
__LX_MMIO_WRITE(writel, u32)
__LX_MMIO_WRITE(writeq, u64)
#define readb_relaxed(a)	readb(a)
#define readw_relaxed(a)	readw(a)
#define readl_relaxed(a)	readl(a)
#define readq_relaxed(a)	readq(a)
#define writeb_relaxed(v, a)	writeb(v, a)
#define writew_relaxed(v, a)	writew(v, a)
#define writel_relaxed(v, a)	writel(v, a)
#define writeq_relaxed(v, a)	writeq(v, a)
#define __raw_readl(a)		readl(a)
#define __raw_writel(v, a)	writel(v, a)
#define __raw_readq(a)		readq(a)
#define __raw_writeq(v, a)	writeq(v, a)
#define ioread8(a)			readb(a)
#define ioread16(a)			readw(a)
#define ioread32(a)			readl(a)
#define ioread64(a)			readq(a)
#define iowrite8(v, a)		writeb(v, a)
#define iowrite16(v, a)		writew(v, a)
#define iowrite32(v, a)		writel(v, a)
#define iowrite64(v, a)		writeq(v, a)
#define ioread32be(a)		be32_to_cpu(readl(a))
#define iowrite32be(v, a)	writel(cpu_to_be32(v), a)
#define mmiowb()			barrier()

static inline u64 lo_hi_readq(const volatile void __iomem *addr)
{
	const volatile u32 __iomem *p = addr;
	u32 low = readl(p);
	u32 high = readl(p + 1);
	return low + ((u64)high << 32);
}
static inline void lo_hi_writeq(u64 val, volatile void __iomem *addr)
{
	writel(val, addr);
	writel(val >> 32, (volatile u8 __iomem *)addr + 4);
}

static inline void memcpy_fromio(void *dst, const volatile void __iomem *src,
	size_t count)
{
	memcpy(dst, (const void __force *)src, count);
}
static inline void memcpy_toio(volatile void __iomem *dst, const void *src,
	size_t count)
{
	memcpy((void __force *)dst, src, count);
}
static inline void memset_io(volatile void __iomem *addr, int value, size_t size)
{
	memset((void __force *)addr, value, size);
}
#define __iowrite32_copy(to, from, count) \
	do { \
		u32 __iomem *__to = (to); \
		const u32 *__from = (from); \
		size_t __i; \
		for (__i = 0; __i < (count); __i++) \
			writel(__from[__i], __to + __i); \
	} while (0)
#define __iowrite64_copy(to, from, count) \
	do { \
		u64 __iomem *__to = (to); \
		const u64 *__from = (from); \
		size_t __i; \
		for (__i = 0; __i < (count); __i++) \
			writeq(__from[__i], __to + __i); \
	} while (0)

/* port I/O */
static inline u8 inb(unsigned long port)
{
	u8 value;
	asm volatile("inb %w1, %b0" : "=a" (value) : "Nd" (port));
	return value;
}
static inline u16 inw(unsigned long port)
{
	u16 value;
	asm volatile("inw %w1, %w0" : "=a" (value) : "Nd" (port));
	return value;
}
static inline u32 inl(unsigned long port)
{
	u32 value;
	asm volatile("inl %w1, %0" : "=a" (value) : "Nd" (port));
	return value;
}
static inline void outb(u8 value, unsigned long port)
{
	asm volatile("outb %b0, %w1" : : "a" (value), "Nd" (port));
}
static inline void outw(u16 value, unsigned long port)
{
	asm volatile("outw %w0, %w1" : : "a" (value), "Nd" (port));
}
static inline void outl(u32 value, unsigned long port)
{
	asm volatile("outl %0, %w1" : : "a" (value), "Nd" (port));
}

/* mappings */
void __iomem *lx_ioremap(phys_addr_t offset, size_t size, int type);
void lx_iounmap(volatile void __iomem *addr);
#define LX_MAP_UC	0
#define LX_MAP_WC	1
#define LX_MAP_WB	2
#define ioremap(offset, size)		lx_ioremap(offset, size, LX_MAP_UC)
#define ioremap_uc(offset, size)	lx_ioremap(offset, size, LX_MAP_UC)
#define ioremap_np(offset, size)	lx_ioremap(offset, size, LX_MAP_UC)
#define ioremap_wc(offset, size)	lx_ioremap(offset, size, LX_MAP_WC)
#define ioremap_wt(offset, size)	lx_ioremap(offset, size, LX_MAP_WC)
#define ioremap_cache(offset, size)	lx_ioremap(offset, size, LX_MAP_WB)
#define ioremap_prot(offset, size, prot)	lx_ioremap(offset, size, LX_MAP_UC)
#define iounmap(addr)				lx_iounmap(addr)
enum {
	MEMREMAP_WB = 1 << 0,
	MEMREMAP_WT = 1 << 1,
	MEMREMAP_WC = 1 << 2,
	MEMREMAP_ENC = 1 << 3,
	MEMREMAP_DEC = 1 << 4,
};
/* A range Haiku has mapped already with another caching (ACPI's mapping of
 * firmware memory, the i915 OpRegion on the Yoga 11e) refuses a second
 * mapping of another type: then it is mapped uncached, as ACPI's is. */
static inline void *memremap(resource_size_t offset, size_t size,
	unsigned long flags)
{
	void *address = (void *)lx_ioremap(offset, size,
		(flags & MEMREMAP_WB) ? LX_MAP_WB : LX_MAP_WC);
	if (address == NULL)
		address = (void *)lx_ioremap(offset, size, LX_MAP_UC);
	return address;
}
#define memunmap(addr)	lx_iounmap((void __iomem *)(addr))

/* io-mapping: a mapped aperture, mapped once in full */
struct io_mapping {
	resource_size_t base;
	unsigned long size;
	pgprot_t prot;
	void __iomem *iomem;
};
static inline struct io_mapping *io_mapping_init_wc(struct io_mapping *iomap,
	resource_size_t base, unsigned long size)
{
	iomap->iomem = ioremap_wc(base, size);
	if (!iomap->iomem)
		return NULL;
	iomap->base = base;
	iomap->size = size;
	iomap->prot = pgprot_writecombine(PAGE_KERNEL);
	return iomap;
}
static inline void io_mapping_fini(struct io_mapping *mapping)
{
	iounmap(mapping->iomem);
}
static inline struct io_mapping *io_mapping_create_wc(resource_size_t base,
	unsigned long size)
{
	struct io_mapping *iomap = kmalloc(sizeof(*iomap), GFP_KERNEL);
	if (!iomap)
		return NULL;
	if (!io_mapping_init_wc(iomap, base, size)) {
		kfree(iomap);
		return NULL;
	}
	return iomap;
}
static inline void io_mapping_free(struct io_mapping *iomap)
{
	io_mapping_fini(iomap);
	kfree(iomap);
}
#define io_mapping_map_wc(mapping, offset, size) \
	((void __iomem *)((char __force *)(mapping)->iomem + (offset)))
#define io_mapping_map_atomic_wc(mapping, offset)	io_mapping_map_wc(mapping, offset, PAGE_SIZE)
#define io_mapping_map_local_wc(mapping, offset)	io_mapping_map_wc(mapping, offset, PAGE_SIZE)
#define io_mapping_unmap(vaddr)			do { (void)(vaddr); } while (0)
#define io_mapping_unmap_atomic(vaddr)	do { (void)(vaddr); } while (0)
#define io_mapping_unmap_local(vaddr)	do { (void)(vaddr); } while (0)

/* linux/iopoll.h */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, \
		sleep_before_read, args...) ({ \
	u64 __timeout_us = (timeout_us); \
	unsigned long __sleep_us = (sleep_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	if ((sleep_before_read) && __sleep_us) \
		usleep_range((__sleep_us >> 2) + 1, __sleep_us); \
	for (;;) { \
		(val) = op(args); \
		if (cond) \
			break; \
		if (__timeout_us && ktime_compare(ktime_get(), __timeout) > 0) { \
			(val) = op(args); \
			break; \
		} \
		if (__sleep_us) \
			usleep_range((__sleep_us >> 2) + 1, __sleep_us); \
		cpu_relax(); \
	} \
	(cond) ? 0 : -ETIMEDOUT; \
})
#define read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, \
		delay_before_read, args...) ({ \
	u64 __timeout_us = (timeout_us); \
	unsigned long __delay_us = (delay_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	if ((delay_before_read) && __delay_us) \
		udelay(__delay_us); \
	for (;;) { \
		(val) = op(args); \
		if (cond) \
			break; \
		if (__timeout_us && ktime_compare(ktime_get(), __timeout) > 0) { \
			(val) = op(args); \
			break; \
		} \
		if (__delay_us) \
			udelay(__delay_us); \
		cpu_relax(); \
	} \
	(cond) ? 0 : -ETIMEDOUT; \
})
#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) \
	read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)
#define readx_poll_timeout_atomic(op, addr, val, cond, delay_us, timeout_us) \
	read_poll_timeout_atomic(op, val, cond, delay_us, timeout_us, false, addr)
#define readl_poll_timeout(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout(readl, addr, val, cond, delay_us, timeout_us)
#define readl_poll_timeout_atomic(addr, val, cond, delay_us, timeout_us) \
	readx_poll_timeout_atomic(readl, addr, val, cond, delay_us, timeout_us)
#define poll_timeout_us(op, cond, sleep_us, timeout_us, sleep_before_op) ({ \
	u64 __timeout_us = (timeout_us); \
	unsigned long __sleep_us = (sleep_us); \
	ktime_t __timeout = ktime_add_us(ktime_get(), __timeout_us); \
	int ___ret; \
	if ((sleep_before_op) && __sleep_us) \
		usleep_range((__sleep_us >> 2) + 1, __sleep_us); \
	for (;;) { \
		bool __expired = __timeout_us && \
			ktime_compare(ktime_get(), __timeout) > 0; \
		op; \
		if (cond) { \
			___ret = 0; \
			break; \
		} \
		if (__expired) { \
			___ret = -ETIMEDOUT; \
			break; \
		} \
		if (__sleep_us) \
			usleep_range((__sleep_us >> 2) + 1, __sleep_us); \
		cpu_relax(); \
	} \
	___ret; \
})

/* resources */
struct resource {
	resource_size_t start;
	resource_size_t end;
	const char *name;
	unsigned long flags;
	unsigned long desc;
	struct resource *parent, *sibling, *child;
};
#define IORESOURCE_IO		0x00000100
#define IORESOURCE_MEM		0x00000200
#define IORESOURCE_REG		0x00000300
#define IORESOURCE_IRQ		0x00000400
#define IORESOURCE_DMA		0x00000800
#define IORESOURCE_PREFETCH	0x00002000
#define IORESOURCE_READONLY	0x00004000
#define IORESOURCE_CACHEABLE	0x00004000
#define IORESOURCE_MEM_64	0x00100000
#define IORESOURCE_UNSET	0x20000000
#define IORESOURCE_BUSY		0x80000000
#define IORESOURCE_ROM_ENABLE	(1 << 0)
#define IORESOURCE_ROM_SHADOW	(1 << 1)
static inline resource_size_t resource_size(const struct resource *res)
{
	return res->end - res->start + 1;
}
static inline unsigned long resource_type(const struct resource *res)
{
	return res->flags & 0x00001f00;
}
#define DEFINE_RES_MEM(_start, _size) \
	((struct resource){ .start = (_start), .end = (_start) + (_size) - 1, \
		.flags = IORESOURCE_MEM })
extern struct resource iomem_resource;
#define request_mem_region(start, n, name)	((struct resource *)1)
#define release_mem_region(start, n)	do { } while (0)
#define iomem_is_exclusive(addr)	false
#define page_is_ram(pfn)	1
#define walk_iomem_res_desc(desc, flags, start, end, arg, func)	(-ENODEV)
#define IORES_DESC_NONE	0
#define IORES_DESC_PERSISTENT_MEMORY	4

#endif /* _LX_IO_H */
