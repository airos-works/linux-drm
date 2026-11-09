/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LX_MM_H
#define _LX_MM_H

/*	Memory: allocation flags, kmalloc/vmalloc, pages, DMA mapping,
	scatterlists, user address spaces.

	kmalloc() and kvmalloc() are Haiku's kernel malloc(); vmalloc() is a
	locked area. A struct page describes one physical page of RAM: pages from
	alloc_pages() come as an array (so "page + i" works for higher orders),
	and every page is registered by frame number, so pfn_to_page() and
	virt_to_page() find it. page_address() is the page in Haiku's physical
	memory map. There is no IOMMU: DMA addresses are physical addresses. */

typedef struct { unsigned long pgprot; } pgprot_t;

/* #pragma mark - page size */

#define PAGE_SHIFT		12
#define PAGE_SIZE		(_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK		(~(PAGE_SIZE - 1))
#define PAGE_ALIGN(addr)	ALIGN(addr, PAGE_SIZE)
#define PAGE_ALIGN_DOWN(addr)	ALIGN_DOWN(addr, PAGE_SIZE)
#define PAGE_ALIGNED(addr)	IS_ALIGNED((unsigned long)(addr), PAGE_SIZE)
#define PFN_ALIGN(x)	(((unsigned long)(x) + (PAGE_SIZE - 1)) & PAGE_MASK)
#define PFN_UP(x)		(((x) + PAGE_SIZE - 1) >> PAGE_SHIFT)
#define PFN_DOWN(x)		((x) >> PAGE_SHIFT)
#define PFN_PHYS(x)		((phys_addr_t)(x) << PAGE_SHIFT)
#define PHYS_PFN(x)		((unsigned long)((x) >> PAGE_SHIFT))
#define offset_in_page(p)	((unsigned long)(p) & ~PAGE_MASK)
#define HPAGE_PMD_ORDER	9
#define HPAGE_PMD_SIZE	(PAGE_SIZE << HPAGE_PMD_ORDER)
#define PMD_SHIFT		21
#define PMD_SIZE		(1UL << PMD_SHIFT)
#define PUD_SHIFT		30
#define PUD_SIZE		(1UL << PUD_SHIFT)
#define MAX_PAGE_ORDER	10
#define MAX_ORDER		MAX_PAGE_ORDER
#define NR_PAGE_ORDERS	(MAX_PAGE_ORDER + 1)
#define NUMA_NO_NODE	(-1)
#define MAX_NUMNODES	1
#define numa_node_id()	0
#define cpu_to_node(cpu)	0
#define dev_to_node(dev)	NUMA_NO_NODE
#define nr_node_ids		1
#define for_each_node(node)	for ((node) = 0; (node) < 1; (node)++)
#define for_each_online_node(node)	for_each_node(node)
#define node_online(node)	((node) == 0)
#define first_online_node	0
#define LX_PMAP_BASE	0xffffff0000000000UL

/* #pragma mark - allocation flags */

typedef unsigned int __bitwise lx_gfp_unused_t;

#define ___GFP_DMA				0x01u
#define ___GFP_HIGHMEM			0x02u
#define ___GFP_DMA32			0x04u
#define ___GFP_MOVABLE			0x08u
#define ___GFP_RECLAIMABLE		0x10u
#define ___GFP_HIGH				0x20u
#define ___GFP_IO				0x40u
#define ___GFP_FS				0x80u
#define ___GFP_ZERO				0x100u
#define ___GFP_DIRECT_RECLAIM	0x400u
#define ___GFP_KSWAPD_RECLAIM	0x800u
#define ___GFP_WRITE			0x1000u
#define ___GFP_NOWARN			0x2000u
#define ___GFP_RETRY_MAYFAIL	0x4000u
#define ___GFP_NOFAIL			0x8000u
#define ___GFP_NORETRY			0x10000u
#define ___GFP_MEMALLOC			0x20000u
#define ___GFP_COMP				0x40000u
#define ___GFP_NOMEMALLOC		0x80000u
#define ___GFP_HARDWALL			0x100000u
#define ___GFP_THISNODE			0x200000u
#define ___GFP_ACCOUNT			0x400000u
#define ___GFP_ZEROTAGS			0x800000u
#define ___GFP_NOLOCKDEP		0x1000000u

#define __GFP_DMA			((__force gfp_t)___GFP_DMA)
#define __GFP_HIGHMEM		((__force gfp_t)___GFP_HIGHMEM)
#define __GFP_DMA32			((__force gfp_t)___GFP_DMA32)
#define __GFP_MOVABLE		((__force gfp_t)___GFP_MOVABLE)
#define __GFP_RECLAIMABLE	((__force gfp_t)___GFP_RECLAIMABLE)
#define __GFP_HIGH			((__force gfp_t)___GFP_HIGH)
#define __GFP_IO			((__force gfp_t)___GFP_IO)
#define __GFP_FS			((__force gfp_t)___GFP_FS)
#define __GFP_ZERO			((__force gfp_t)___GFP_ZERO)
#define __GFP_DIRECT_RECLAIM ((__force gfp_t)___GFP_DIRECT_RECLAIM)
#define __GFP_KSWAPD_RECLAIM ((__force gfp_t)___GFP_KSWAPD_RECLAIM)
#define __GFP_RECLAIM		((__force gfp_t)(___GFP_DIRECT_RECLAIM | ___GFP_KSWAPD_RECLAIM))
#define __GFP_WRITE			((__force gfp_t)___GFP_WRITE)
#define __GFP_NOWARN		((__force gfp_t)___GFP_NOWARN)
#define __GFP_RETRY_MAYFAIL	((__force gfp_t)___GFP_RETRY_MAYFAIL)
#define __GFP_NOFAIL		((__force gfp_t)___GFP_NOFAIL)
#define __GFP_NORETRY		((__force gfp_t)___GFP_NORETRY)
#define __GFP_MEMALLOC		((__force gfp_t)___GFP_MEMALLOC)
#define __GFP_COMP			((__force gfp_t)___GFP_COMP)
#define __GFP_NOMEMALLOC	((__force gfp_t)___GFP_NOMEMALLOC)
#define __GFP_HARDWALL		((__force gfp_t)___GFP_HARDWALL)
#define __GFP_THISNODE		((__force gfp_t)___GFP_THISNODE)
#define __GFP_ACCOUNT		((__force gfp_t)___GFP_ACCOUNT)
#define __GFP_ZEROTAGS		((__force gfp_t)___GFP_ZEROTAGS)
#define __GFP_NOLOCKDEP		((__force gfp_t)___GFP_NOLOCKDEP)
#define __GFP_SKIP_KASAN	((__force gfp_t)0)
#define GFP_ATOMIC		(__GFP_HIGH | __GFP_KSWAPD_RECLAIM)
#define GFP_KERNEL		(__GFP_RECLAIM | __GFP_IO | __GFP_FS)
#define GFP_KERNEL_ACCOUNT	(GFP_KERNEL | __GFP_ACCOUNT)
#define GFP_NOWAIT		(__GFP_KSWAPD_RECLAIM | __GFP_NOWARN)
#define GFP_NOIO		(__GFP_RECLAIM)
#define GFP_NOFS		(__GFP_RECLAIM | __GFP_IO)
#define GFP_USER		(__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_HARDWALL)
#define GFP_DMA			__GFP_DMA
#define GFP_DMA32		__GFP_DMA32
#define GFP_HIGHUSER	(GFP_USER | __GFP_HIGHMEM)
#define GFP_HIGHUSER_MOVABLE	(GFP_HIGHUSER | __GFP_MOVABLE)
#define GFP_TRANSHUGE_LIGHT	((GFP_HIGHUSER_MOVABLE | __GFP_COMP | \
	__GFP_NOMEMALLOC | __GFP_NOWARN) & ~__GFP_RECLAIM)
#define GFP_TRANSHUGE	(GFP_TRANSHUGE_LIGHT | __GFP_DIRECT_RECLAIM)
#define gfpflags_allow_blocking(gfp)	(!!((gfp) & __GFP_DIRECT_RECLAIM))
#define current_gfp_context(gfp)	(gfp)
#define gfp_zone(gfp)	0

/* #pragma mark - kmalloc */

#define ARCH_KMALLOC_MINALIGN	8
#define ARCH_DMA_MINALIGN		64
#define KMALLOC_MAX_SIZE		(1UL << 30)
#define ZERO_SIZE_PTR			((void *)16)
#define ZERO_OR_NULL_PTR(x)		((unsigned long)(x) <= (unsigned long)ZERO_SIZE_PTR)

void *lx_kmalloc(size_t size, gfp_t flags) __malloc __alloc_size(1);
void *lx_krealloc(const void *p, size_t new_size, gfp_t flags) __alloc_size(2);
void lx_kfree(const void *p);
size_t ksize(const void *objp);
void *lx_vmalloc(size_t size, gfp_t flags) __malloc __alloc_size(1);
void lx_vfree(const void *addr);
bool is_vmalloc_addr(const void *x);

#define kmalloc(size, flags)		lx_kmalloc(size, flags)
#define kmalloc_noprof				kmalloc
#define kzalloc(size, flags)		lx_kmalloc(size, (flags) | __GFP_ZERO)
#define kzalloc_noprof				kzalloc
#define kmalloc_node(size, flags, node)	kmalloc(size, flags)
#define kmalloc_node_track_caller(size, flags, node)	kmalloc(size, flags)
#define kmalloc_track_caller(size, flags)	kmalloc(size, flags)
#define want_init_on_free()	false
#define want_init_on_alloc(flags)	false
#define kzalloc_node(size, flags, node)	kzalloc(size, flags)
#define kfree(p)					lx_kfree(p)
#define kfree_sensitive(p)			lx_kfree(p)
#define krealloc(p, size, flags)	lx_krealloc(p, size, flags)
static inline __alloc_size(2, 3) void *kmalloc_array(size_t n, size_t size,
	gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(n, size, &bytes)))
		return NULL;
	return kmalloc(bytes, flags);
}
#define kcalloc(n, size, flags)		kmalloc_array(n, size, (flags) | __GFP_ZERO)
#define kmalloc_array_node(n, size, flags, node)	kmalloc_array(n, size, flags)
#define kcalloc_node(n, size, flags, node)	kcalloc(n, size, flags)
static inline __alloc_size(2, 3) void *krealloc_array(void *p, size_t new_n,
	size_t new_size, gfp_t flags)
{
	size_t bytes;
	if (unlikely(check_mul_overflow(new_n, new_size, &bytes)))
		return NULL;
	return krealloc(p, bytes, flags);
}
#define kvmalloc(size, flags)		kmalloc(size, flags)
#define kvzalloc(size, flags)		kzalloc(size, flags)
#define kvmalloc_node(size, flags, node)	kmalloc(size, flags)
#define kvzalloc_node(size, flags, node)	kzalloc(size, flags)
#define kvmalloc_array(n, size, flags)	kmalloc_array(n, size, flags)
#define kvcalloc(n, size, flags)		kcalloc(n, size, flags)
#define kvrealloc(p, size, flags)		krealloc(p, size, flags)
#define kvfree(p)					lx_kfree(p)
#define kvfree_sensitive(p, len)	lx_kfree(p)
#define vmalloc(size)				lx_vmalloc(size, GFP_KERNEL)
#define vzalloc(size)				lx_vmalloc(size, GFP_KERNEL | __GFP_ZERO)
#define __vmalloc(size, flags)		lx_vmalloc(size, flags)
#define vmalloc_node(size, node)	vmalloc(size)
#define vzalloc_node(size, node)	vzalloc(size)
#define vmalloc_32(size)			vmalloc(size)
#define vmalloc_user(size)			vzalloc(size)
#define vmalloc_array(n, size)		vmalloc(array_size(n, size))
#define vcalloc(n, size)			vzalloc(array_size(n, size))
#define vfree(p)					lx_vfree(p)
#define kmalloc_size_roundup(size)	(size)
/* typed allocations; the allocation flags may be left out (newer Linux) */
#define lx_default_gfp(...)			__lx_default_gfp(, ##__VA_ARGS__, GFP_KERNEL)
#define __lx_default_gfp(empty, gfp, ...)	(gfp)
#define kmalloc_obj(p, ...) \
	((typeof(p) *)kmalloc(sizeof(typeof(p)), lx_default_gfp(__VA_ARGS__)))
#define kzalloc_obj(p, ...) \
	((typeof(p) *)kzalloc(sizeof(typeof(p)), lx_default_gfp(__VA_ARGS__)))
#define kmalloc_objs(p, n, ...) \
	((typeof(p) *)kmalloc_array(n, sizeof(typeof(p)), lx_default_gfp(__VA_ARGS__)))
#define kzalloc_objs(p, n, ...) \
	((typeof(p) *)kcalloc(n, sizeof(typeof(p)), lx_default_gfp(__VA_ARGS__)))
#define kvzalloc_objs(p, n, ...) \
	((typeof(p) *)kvcalloc(n, sizeof(typeof(p)), lx_default_gfp(__VA_ARGS__)))
#define kzalloc_flex(p, member, n, ...) \
	((typeof(p) *)kzalloc(struct_size_t(typeof(p), member, n), \
		lx_default_gfp(__VA_ARGS__)))
#define kmemleak_alloc(ptr, size, min, gfp)	do { } while (0)
#define kmemleak_free(ptr)			do { } while (0)
#define kmemleak_ignore(ptr)		do { } while (0)
#define kmemleak_not_leak(ptr)		do { } while (0)
#define kmemleak_update_trace(ptr)	do { } while (0)

DEFINE_FREE(kfree, void *, if (!IS_ERR_OR_NULL(_T)) kfree(_T))
DEFINE_FREE(kvfree, void *, if (!IS_ERR_OR_NULL(_T)) kvfree(_T))
DEFINE_FREE(vfree, void *, if (!IS_ERR_OR_NULL(_T)) vfree(_T))

/* slab caches: fixed size kmalloc */
typedef unsigned int __bitwise slab_flags_lx_unused_t;
struct kmem_cache {
	const char *name;
	unsigned int size;
	unsigned int align;
	slab_flags_t flags;
	void (*ctor)(void *);
	struct list_head entry;		/* in the list of caches, for lx_mm_stats() */
	long live;					/* objects allocated and not freed */
	bool tracked;				/* its objects are kept in a table, for
								   lx_kmem_cache_for_each() */
};

int lx_mm_stats(char *buffer, int size);
int lx_kmalloc_top(char *buffer, int size, int count);
void lx_kmem_cache_for_each(const char *name,
	void (*function)(void *object, s64 age_ns, void *data), void *data);
#define SLAB_HWCACHE_ALIGN	((__force slab_flags_t)0x00002000U)
#define SLAB_RECLAIM_ACCOUNT ((__force slab_flags_t)0x00020000U)
#define SLAB_PANIC			((__force slab_flags_t)0x00040000U)
#define SLAB_TYPESAFE_BY_RCU ((__force slab_flags_t)0x00080000U)
#define SLAB_ACCOUNT		((__force slab_flags_t)0x04000000U)
#define SLAB_TEMPORARY		SLAB_RECLAIM_ACCOUNT
struct kmem_cache *kmem_cache_create(const char *name, unsigned int size,
	unsigned int align, slab_flags_t flags, void (*ctor)(void *));
#define kmem_cache_create_usercopy(name, size, align, flags, uo, us, ctor) \
	kmem_cache_create(name, size, align, flags, ctor)
#define KMEM_CACHE(__struct, __flags) \
	kmem_cache_create(#__struct, sizeof(struct __struct), \
		__alignof__(struct __struct), (__flags), NULL)
void kmem_cache_destroy(struct kmem_cache *s);
void *kmem_cache_alloc(struct kmem_cache *s, gfp_t flags) __malloc;
#define kmem_cache_zalloc(s, flags)	kmem_cache_alloc(s, (flags) | __GFP_ZERO)
#define kmem_cache_alloc_node(s, flags, node)	kmem_cache_alloc(s, flags)
void kmem_cache_free(struct kmem_cache *s, void *objp);
#define kmem_cache_shrink(s)	0

/* #pragma mark - pages */

struct address_space;

struct page {
	unsigned long flags;
	unsigned long private;
	phys_addr_t lx_phys;
	void *lx_haiku_page;		/* Haiku's vm_page, NULL if not RAM we own */
	atomic_t _refcount;
	atomic_t _mapcount;
	unsigned int lx_order;		/* allocation order, in the first page */
	unsigned int lx_count;		/* pages in the allocation, first page */
	struct page *lx_head;		/* first page of the allocation */
	struct address_space *mapping;
	unsigned long index;
	struct list_head lru;
} __aligned(16);

struct folio {
	struct page page;
};
#define page_folio(p)	((struct folio *)(p))
#define folio_page(f, n)	(&(f)->page + (n))
#define folio_pfn(f)		page_to_pfn(&(f)->page)
#define folio_order(f)		((f)->page.lx_order)
#define folio_nr_pages(f)	(1UL << folio_order(f))
#define folio_size(f)		(PAGE_SIZE << folio_order(f))
#define folio_address(f)	page_address(&(f)->page)
#define folio_put(f)		put_page(&(f)->page)
#define folio_get(f)		get_page(&(f)->page)
#define folio_lock(f)		do { } while (0)
#define folio_unlock(f)		do { } while (0)
#define folio_mark_dirty(f)	true
#define folio_mark_accessed(f)	do { } while (0)
#define folio_file_page(f, i)	(&(f)->page)
#define folio_test_dirty(f)	false
#define folio_test_writeback(f)	false
#define folio_set_reclaim(f)	do { } while (0)
#define folio_clear_reclaim(f)	do { } while (0)
#define folio_mapped(f)		false
#define folio_clear_dirty_for_io(f)	false
#define __folio_batch_release(fb)	do { (fb)->nr = 0; } while (0)
#define split_page(page, order)	do { } while (0)
#define shmem_writeout(folio, plug, list)	(-ENOSYS)
#define inode_set_bytes(inode, bytes)	do { } while (0)
#define file_clone_open(f)	((struct file *)ERR_PTR(-ENOSYS))
#define vma_set_file(vma, file)	do { (vma)->vm_file = (file); } while (0)
void *vmap(struct page **pages, unsigned int count, unsigned long flags,
	pgprot_t prot);
void vunmap(const void *addr);
#define VM_MAP		0x00000004
#define VM_IOREMAP	0x00000001
#define VM_USERMAP	0x00000008
#define vmap_pfn(pfns, count, prot)	NULL
#define folio_test_highmem(f)	false
#define folio_set_dirty(f)	do { } while (0)
#define folio_clear_dirty(f) do { } while (0)
struct folio_batch {
	unsigned char nr;
	struct folio *folios[31];
};
#define folio_batch_init(fbatch)	((fbatch)->nr = 0)
#define folio_batch_count(fbatch)	((fbatch)->nr)
#define folio_batch_add(fbatch, folio) \
	((fbatch)->folios[(fbatch)->nr++] = (folio), 31 - (fbatch)->nr)
#define folio_batch_release(fbatch)	do { (fbatch)->nr = 0; } while (0)
#define check_move_unevictable_folios(fbatch)	do { } while (0)

struct page *lx_alloc_pages(gfp_t gfp, unsigned int order);
void lx_free_pages(struct page *page, unsigned int order);
struct page *lx_pfn_to_page(unsigned long pfn);
phys_addr_t lx_virt_to_phys(const volatile void *address);

#define alloc_pages(gfp, order)		lx_alloc_pages(gfp, order)
#define alloc_pages_node(nid, gfp, order)	lx_alloc_pages(gfp, order)
#define alloc_pages_noprof			alloc_pages
#define alloc_page(gfp)				lx_alloc_pages(gfp, 0)
#define __free_pages(page, order)	lx_free_pages(page, order)
#define __free_page(page)			lx_free_pages(page, 0)
#define folio_alloc(gfp, order)		page_folio(lx_alloc_pages(gfp, order))
#define page_to_pfn(page)			((unsigned long)((page)->lx_phys >> PAGE_SHIFT))
#define pfn_to_page(pfn)			lx_pfn_to_page(pfn)
#define page_to_phys(page)			((page)->lx_phys)
#define phys_to_page(phys)			lx_pfn_to_page((phys) >> PAGE_SHIFT)
#define pfn_valid(pfn)				(lx_pfn_to_page(pfn) != NULL)
#define page_address(page)			((void *)(LX_PMAP_BASE + (page)->lx_phys))
#define lowmem_page_address(page)	page_address(page)
#define virt_to_phys(address)		lx_virt_to_phys(address)
#define __pa(address)				lx_virt_to_phys((void *)(address))
#define __pa_symbol(address)		__pa(address)
#define phys_to_virt(phys)			((void *)(LX_PMAP_BASE + (phys_addr_t)(phys)))
#define __va(phys)					phys_to_virt(phys)
#define virt_to_page(address)		lx_pfn_to_page(lx_virt_to_phys(address) >> PAGE_SHIFT)
#define virt_to_pfn(address)		(lx_virt_to_phys(address) >> PAGE_SHIFT)
#define virt_addr_valid(address)	1
#define page_to_virt(page)			page_address(page)
#define vmalloc_to_page(address)	virt_to_page(address)
#define vmalloc_to_pfn(address)		virt_to_pfn(address)
#define page_size(page)				(PAGE_SIZE << (page)->lx_order)
#define compound_order(page)		((page)->lx_order)
#define compound_head(page)			(page)
#define page_to_nid(page)			0
#define pfn_to_nid(pfn)				0
#define page_zone(page)				NULL
#define PageHighMem(page)			0
#define PageReserved(page)			0
#define PageCompound(page)			((page)->lx_order != 0)
#define PageTail(page)				0
#define PageSlab(page)				0
#define PageDirty(page)				0
#define SetPageReserved(page)		do { } while (0)
#define ClearPageReserved(page)		do { } while (0)
#define SetPageDirty(page)			do { } while (0)
#define set_page_dirty(page)		1
#define set_page_dirty_lock(page)	1
#define mark_page_accessed(page)	do { } while (0)
#define is_zone_device_page(page)	false
#define page_pgmap(page)			NULL
#define page_private(page)			((page)->private)
#define set_page_private(page, v)	((page)->private = (v))
#define page_count(page)			atomic_read(&(page)->_refcount)
#define page_ref_count(page)		atomic_read(&(page)->_refcount)
#define set_page_count(page, v)		atomic_set(&(page)->_refcount, v)
#define page_ref_inc(page)			atomic_inc(&(page)->_refcount)
#define get_page(page)				atomic_inc(&(page)->_refcount)
void put_page(struct page *page);
#define unpin_user_page(page)		put_page(page)
#define unpin_user_pages(pages, n) \
	do { unsigned long __i; for (__i = 0; __i < (n); __i++) put_page((pages)[__i]); } while (0)
#define release_pages(pages, n)		unpin_user_pages(pages, n)
#define page_mapped(page)			0
#define page_mapcount(page)			0
unsigned long __get_free_pages(gfp_t gfp_mask, unsigned int order);
#define __get_free_page(gfp)		__get_free_pages(gfp, 0)
#define get_zeroed_page(gfp)		__get_free_pages((gfp) | __GFP_ZERO, 0)
void free_pages(unsigned long addr, unsigned int order);
#define free_page(addr)				free_pages(addr, 0)
void *alloc_pages_exact(size_t size, gfp_t gfp_mask);
void free_pages_exact(void *virt, size_t size);
#define get_order(size)	((size) <= PAGE_SIZE ? 0 : \
	(int)ilog2(roundup_pow_of_two((unsigned long)(size)) >> PAGE_SHIFT))
extern struct page *lx_zero_page;
#define ZERO_PAGE(vaddr)	lx_zero_page
unsigned long totalram_pages(void);
struct sysinfo {
	unsigned long totalram;
	unsigned long freeram;
	unsigned long sharedram;
	unsigned long bufferram;
	unsigned long totalswap;
	unsigned long freeswap;
	unsigned long totalhigh;
	unsigned long freehigh;
	unsigned int mem_unit;
};
void si_meminfo(struct sysinfo *val);
#define si_mem_available()	(totalram_pages() / 2)
#define get_nr_swap_pages()	0L
#define total_swap_pages	0L
#define high_memory			((void *)~0UL)

/* highmem: everything is in the physical map */
#define kmap(page)				page_address(page)
#define kunmap(page)			do { (void)(page); } while (0)
#define kmap_local_page(page)	page_address(page)
#define kmap_local_folio(f, off)	((char *)page_address(&(f)->page) + (off))
#define kmap_local_pfn(pfn)		phys_to_virt(PFN_PHYS(pfn))
#define kunmap_local(addr)		do { (void)(addr); } while (0)
#define kmap_atomic(page)		page_address(page)
#define kunmap_atomic(addr)		do { (void)(addr); } while (0)
#define kmap_local_page_try_from_panic(page)	page_address(page)
#define kmap_atomic_prot(page, prot)	page_address(page)
#define kmap_local_page_prot(page, prot)	page_address(page)
#define kmap_atomic_pfn(pfn)	phys_to_virt(PFN_PHYS(pfn))
#define kmap_to_page(addr)		virt_to_page(addr)
#define clear_page(addr)		memset((addr), 0, PAGE_SIZE)
#define copy_page(to, from)		memcpy((to), (from), PAGE_SIZE)
#define clear_highpage(page)	clear_page(page_address(page))
#define copy_highpage(to, from)	copy_page(page_address(to), page_address(from))
#define memcpy_to_page(page, off, from, len) \
	memcpy((char *)page_address(page) + (off), (from), (len))
#define memcpy_from_page(to, page, off, len) \
	memcpy((to), (char *)page_address(page) + (off), (len))
#define memzero_page(page, off, len) \
	memset((char *)page_address(page) + (off), 0, (len))

/* Caching attributes of the kernel mapping: it stays write-back (Haiku's
   physical map, see lx_pat_enabled). Pages made write-combined or uncached
   for the GPU, which then reads and writes them without snooping, are
   flushed from the processor caches instead, so that nothing written
   through that mapping (clearing them) reaches the GPU late or is written
   back over what the GPU wrote. */
void lx_flush_virt_range(const void *address, unsigned long size);
void lx_flush_page_range(struct page *page, unsigned long count);
void lx_flush_page_array(struct page **pages, unsigned long count);
#define set_pages_wb(page, n)			0
#define set_pages_uc(page, n)			(lx_flush_page_range(page, n), 0)
#define set_pages_wc(page, n)			(lx_flush_page_range(page, n), 0)
#define set_pages_array_wb(pages, n)	0
#define set_pages_array_wc(pages, n)	(lx_flush_page_array(pages, n), 0)
#define set_pages_array_uc(pages, n)	(lx_flush_page_array(pages, n), 0)
#define set_memory_wb(addr, n)			0
#define set_memory_wc(addr, n) \
	(lx_flush_virt_range((const void *)(addr), (n) * PAGE_SIZE), 0)
#define set_memory_uc(addr, n) \
	(lx_flush_virt_range((const void *)(addr), (n) * PAGE_SIZE), 0)
#define set_memory_ro(addr, n)			0
#define set_memory_rw(addr, n)			0
#define set_memory_decrypted(addr, n)	0
#define set_memory_encrypted(addr, n)	0
#define pat_enabled()					false
#define arch_io_reserve_memtype_wc(start, size)	0
#define arch_io_free_memtype_wc(start, size)	do { } while (0)
#define arch_phys_wc_add(base, size)	0
#define arch_phys_wc_del(handle)		do { } while (0)
#define arch_phys_wc_index(handle)		(-1)
void clflush_cache_range(void *addr, unsigned int size);
/* (Linux patches the instruction into clflush on a processor without
 * clflushopt, where it is an invalid opcode) */
#define clflushopt(addr) \
	do { \
		if (likely(lx_cpu_has(X86_FEATURE_CLFLUSHOPT))) { \
			asm volatile("clflushopt %0" \
				: "+m" (*(volatile char *)(addr))); \
		} else { \
			asm volatile("clflush %0" \
				: "+m" (*(volatile char *)(addr))); \
		} \
	} while (0)
#define clflush(addr)		asm volatile("clflush %0" : "+m" (*(volatile char *)(addr)))
#define wbinvd_on_all_cpus()	lx_wbinvd_on_all_cpus()
int lx_wbinvd_on_all_cpus(void);
#define boot_cpu_has(f)			lx_cpu_has(f)
#define static_cpu_has(f)		lx_cpu_has(f)
#define cpu_feature_enabled(f)	lx_cpu_has(f)
#define X86_FEATURE_CLFLUSH		1
#define X86_FEATURE_CLFLUSHOPT	2
#define X86_FEATURE_PAT			3
#define X86_FEATURE_HYPERVISOR	4
#define X86_FEATURE_XMM			5
#define X86_FEATURE_XMM2		6
#define X86_FEATURE_XMM4_1		7
#define X86_FEATURE_AVX			8
bool lx_cpu_has(int feature);
struct cpuinfo_x86 {
	u32 x86_vfm;
	u8 x86;
	u8 x86_vendor;
	u8 x86_model;
	u8 x86_stepping;
	u16 x86_clflush_size;
	int x86_cache_alignment;
};
extern struct cpuinfo_x86 boot_cpu_data;
#define X86_VENDOR_INTEL	0
#define X86_VENDOR_AMD		2
#define X86_VENDOR_HYGON	9
#define cpu_data(cpu)		(*(&boot_cpu_data + 0 * (cpu)))
#define cc_platform_has(attr)	false
#define sme_me_mask			0ULL
#define mem_encrypt_active()	false
#define force_dma_unencrypted(dev)	false
#define hypervisor_is_type(t)	false
#define X86_HYPER_NATIVE	0
#define X86_HYPER_XEN_PV	1
#define X86_HYPER_MS_HYPERV	2
#define X86_HYPER_VMWARE	3
#define X86_HYPER_KVM		4
#define xen_domain()		0
#define xen_pv_domain()		0
#define xen_initial_domain() 0

/* #pragma mark - user address spaces */

#define __pgprot(x)		((pgprot_t){ (x) })
#define pgprot_val(x)	((x).pgprot)
#define PAGE_KERNEL		__pgprot(0x163)
#define PAGE_KERNEL_IO	__pgprot(0x163)
#define PAGE_SHARED		__pgprot(0x67)
#define PAGE_KERNEL_NOCACHE	__pgprot(0x173)
#define pgprot_writecombine(p)	__pgprot(pgprot_val(p) | 0x8)
#define pgprot_noncached(p)		__pgprot(pgprot_val(p) | 0x18)
#define pgprot_decrypted(p)		(p)
#define pgprot_encrypted(p)		(p)
#define pgprot_device(p)		pgprot_noncached(p)
#define pgprot_modify(old, new)	(new)
#define _PAGE_CACHE_MASK	0x18

typedef unsigned long vm_flags_t;
#define VM_NONE		0x00000000
#define VM_READ		0x00000001
#define VM_WRITE	0x00000002
#define VM_EXEC		0x00000004
#define VM_SHARED	0x00000008
#define VM_MAYREAD	0x00000010
#define VM_MAYWRITE	0x00000020
#define VM_MAYEXEC	0x00000040
#define VM_MAYSHARE	0x00000080
#define VM_PFNMAP	0x00000400
#define VM_IO		0x00004000
#define VM_DONTCOPY	0x00020000
#define VM_DONTEXPAND	0x00040000
#define VM_ACCOUNT	0x00100000
#define VM_NORESERVE	0x00200000
#define VM_MIXEDMAP	0x10000000
#define VM_DONTDUMP	0x04000000
#define VM_WIPEONFORK	0x02000000
#define VM_ACCESS_FLAGS	(VM_READ | VM_WRITE | VM_EXEC)

/*	One per team, current->mm of the team's threads (kernel threads have
	none): amdgpu's userptr buffers belong to the team that made them and are
	checked against current->mm. Tasks and mmgrab() hold mm_count; the last
	mmdrop() frees it (compat/src/sched.c). */
struct mm_struct {
	struct rw_semaphore mmap_lock;
	atomic_t mm_users;
	atomic_t mm_count;
	unsigned long total_vm;
	atomic64_t pinned_vm;
	int lx_team;
	struct list_head lx_link;	/* in the table of teams' mms */
};

struct vm_area_struct;
struct vm_fault;
typedef unsigned int vm_fault_t;
#define VM_FAULT_OOM		0x000001
#define VM_FAULT_SIGBUS		0x000002
#define VM_FAULT_MAJOR		0x000004
#define VM_FAULT_HWPOISON	0x000010
#define VM_FAULT_SIGSEGV	0x000040
#define VM_FAULT_NOPAGE		0x000100
#define VM_FAULT_LOCKED		0x000200
#define VM_FAULT_RETRY		0x000400
#define VM_FAULT_FALLBACK	0x000800
#define VM_FAULT_DONE_COW	0x001000
#define VM_FAULT_ERROR		(VM_FAULT_OOM | VM_FAULT_SIGBUS | \
	VM_FAULT_SIGSEGV | VM_FAULT_HWPOISON | VM_FAULT_FALLBACK)
#define FAULT_FLAG_WRITE			0x01
#define FAULT_FLAG_MKWRITE			0x02
#define FAULT_FLAG_ALLOW_RETRY		0x04
#define FAULT_FLAG_RETRY_NOWAIT		0x08
#define FAULT_FLAG_KILLABLE			0x10
#define FAULT_FLAG_TRIED			0x20
#define FAULT_FLAG_USER				0x40
#define FAULT_FLAG_REMOTE			0x80
#define FAULT_FLAG_INTERRUPTIBLE	0x200
#define fault_flag_allow_retry_first(f)	((f) & FAULT_FLAG_ALLOW_RETRY)

struct vm_operations_struct {
	void (*open)(struct vm_area_struct *area);
	void (*close)(struct vm_area_struct *area);
	int (*may_split)(struct vm_area_struct *area, unsigned long addr);
	vm_fault_t (*fault)(struct vm_fault *vmf);
	vm_fault_t (*huge_fault)(struct vm_fault *vmf, unsigned int order);
	vm_fault_t (*page_mkwrite)(struct vm_fault *vmf);
	vm_fault_t (*pfn_mkwrite)(struct vm_fault *vmf);
	int (*access)(struct vm_area_struct *vma, unsigned long addr, void *buf,
		int len, int write);
	const char *(*name)(struct vm_area_struct *vma);
	int (*mremap)(struct vm_area_struct *area);
};

struct file;
struct vm_area_struct {
	unsigned long vm_start;
	unsigned long vm_end;
	struct mm_struct *vm_mm;
	pgprot_t vm_page_prot;
	vm_flags_t vm_flags;
	unsigned long vm_pgoff;
	struct file *vm_file;
	void *vm_private_data;
	const struct vm_operations_struct *vm_ops;
	void *lx_haiku;			/* Haiku mapping being filled */
};

struct vm_fault {
	struct vm_area_struct *vma;
	gfp_t gfp_mask;
	pgoff_t pgoff;
	unsigned long address;
	unsigned int flags;
	struct page *page;
};

#define vma_pages(vma)	(((vma)->vm_end - (vma)->vm_start) >> PAGE_SHIFT)
#define vm_flags_set(vma, flags)	((vma)->vm_flags |= (flags))
#define vm_flags_clear(vma, flags)	((vma)->vm_flags &= ~(flags))
#define vm_flags_mod(vma, set, clear) \
	((vma)->vm_flags = ((vma)->vm_flags | (set)) & ~(clear))
#define vm_get_page_prot(flags)	PAGE_SHARED
#define is_cow_mapping(flags)	(((flags) & (VM_SHARED | VM_MAYWRITE)) == VM_MAYWRITE)
vm_fault_t vmf_insert_pfn_prot(struct vm_area_struct *vma, unsigned long addr,
	unsigned long pfn, pgprot_t pgprot);
#define vmf_insert_pfn(vma, addr, pfn) \
	vmf_insert_pfn_prot(vma, addr, pfn, (vma)->vm_page_prot)
#define vmf_insert_mixed(vma, addr, pfn) vmf_insert_pfn(vma, addr, pfn)
#define vmf_insert_page(vma, addr, page) \
	vmf_insert_pfn(vma, addr, page_to_pfn(page))
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
	unsigned long pfn, unsigned long size, pgprot_t prot);
#define io_remap_pfn_range	remap_pfn_range
void unmap_mapping_range(struct address_space *mapping, loff_t const holebegin,
	loff_t const holelen, int even_cows);
#define zap_vma_ptes(vma, address, size)	do { } while (0)
#define mmap_read_lock(mm)		down_read(&(mm)->mmap_lock)
#define mmap_read_unlock(mm)	up_read(&(mm)->mmap_lock)
#define mmap_write_lock(mm)		down_write(&(mm)->mmap_lock)
#define mmap_write_unlock(mm)	up_write(&(mm)->mmap_lock)
#define mmap_read_trylock(mm)	down_read_trylock(&(mm)->mmap_lock)
#define mmap_assert_locked(mm)	do { } while (0)
#define mmap_assert_write_locked(mm)	do { } while (0)
#define mmget(mm)				atomic_inc(&(mm)->mm_users)
#define mmget_not_zero(mm)		atomic_inc_not_zero(&(mm)->mm_users)
#define mmput(mm)				atomic_dec(&(mm)->mm_users)
#define mmput_async(mm)			mmput(mm)
#define mmgrab(mm)				atomic_inc(&(mm)->mm_count)
void mmdrop(struct mm_struct *mm);
#define find_vma(mm, addr)		((struct vm_area_struct *)NULL)
#define vma_lookup(mm, addr)	((struct vm_area_struct *)NULL)
#define get_task_mm(task)		((task)->mm)
#define access_ok(addr, size)	1
#define FOLL_WRITE		0x01
#define FOLL_LONGTERM	0x100
#define get_user_pages(start, nr, flags, pages)	(-EFAULT)
#define get_user_pages_fast(start, nr, flags, pages)	(-EFAULT)
#define pin_user_pages_fast(start, nr, flags, pages)	(-EFAULT)
#define pin_user_pages(start, nr, flags, pages)	(-EFAULT)

/* page cache, shmem: no swapping of graphics memory */
struct address_space {
	void *host;
	unsigned long nrpages;
	const void *a_ops;
};
struct file *shmem_file_setup(const char *name, loff_t size, unsigned long flags);
#define shmem_file_setup_with_mnt(mnt, name, size, flags) \
	shmem_file_setup(name, size, flags)
#define shmem_read_mapping_page_gfp(mapping, index, gfp)	ERR_PTR(-ENOSYS)
#define shmem_read_mapping_page(mapping, index)	ERR_PTR(-ENOSYS)
#define shmem_read_folio_gfp(mapping, index, gfp)	ERR_PTR(-ENOSYS)
#define shmem_read_folio(mapping, index)	ERR_PTR(-ENOSYS)
#define shmem_truncate_range(inode, start, end)	do { } while (0)
#define mapping_set_unevictable(mapping)	do { } while (0)
#define mapping_clear_unevictable(mapping)	do { } while (0)
#define mapping_gfp_mask(mapping)	GFP_KERNEL
#define mapping_set_gfp_mask(mapping, mask)	do { } while (0)
#define mapping_gfp_constraint(mapping, gfp)	(gfp)
#define invalidate_mapping_pages(mapping, start, end)	0UL

/* shrinkers: graphics memory is not reclaimed under memory pressure */
struct shrink_control {
	gfp_t gfp_mask;
	int nid;
	unsigned long nr_to_scan;
	unsigned long nr_scanned;
};
struct shrinker {
	unsigned long (*count_objects)(struct shrinker *, struct shrink_control *sc);
	unsigned long (*scan_objects)(struct shrinker *, struct shrink_control *sc);
	long batch;
	int seeks;
	unsigned flags;
	void *private_data;
};
#define SHRINK_STOP		(~0UL)
#define SHRINK_EMPTY	(~0UL - 1)
#define DEFAULT_SEEKS	2
#define SHRINKER_NUMA_AWARE	(1 << 2)
struct shrinker *shrinker_alloc(unsigned int flags, const char *fmt, ...);
void shrinker_register(struct shrinker *shrinker);
void shrinker_free(struct shrinker *shrinker);
#define current_is_kswapd()	false

/* #pragma mark - copying to and from teams */

unsigned long lx_copy_from_user(void *to, const void __user *from, unsigned long n);
unsigned long lx_copy_to_user(void __user *to, const void *from, unsigned long n);
unsigned long lx_clear_user(void __user *to, unsigned long n);
#define copy_from_user(to, from, n)		lx_copy_from_user(to, from, n)
#define copy_to_user(to, from, n)		lx_copy_to_user(to, from, n)
#define __copy_from_user(to, from, n)	lx_copy_from_user(to, from, n)
#define __copy_to_user(to, from, n)		lx_copy_to_user(to, from, n)
#define copy_from_user_inatomic(t, f, n)	lx_copy_from_user(t, f, n)
#define __copy_from_user_inatomic(t, f, n)	lx_copy_from_user(t, f, n)
#define clear_user(to, n)				lx_clear_user(to, n)
#define get_user(x, ptr) ({ \
	__typeof__(*(ptr)) __lx_val; \
	int __lx_err = lx_copy_from_user(&__lx_val, (ptr), sizeof(__lx_val)) \
		? -EFAULT : 0; \
	(x) = __lx_val; \
	__lx_err; \
})
#define put_user(x, ptr) ({ \
	__typeof__(*(ptr)) __lx_val = (x); \
	lx_copy_to_user((ptr), &__lx_val, sizeof(__lx_val)) ? -EFAULT : 0; \
})
#define __get_user(x, ptr)	get_user(x, ptr)
#define __put_user(x, ptr)	put_user(x, ptr)
#define user_access_begin(ptr, len)	1
#define user_access_end()	do { } while (0)
#define unsafe_put_user(x, ptr, label) \
	do { if (put_user(x, ptr)) goto label; } while (0)
#define unsafe_get_user(x, ptr, label) \
	do { if (get_user(x, ptr)) goto label; } while (0)
#define pagefault_disable()	do { } while (0)
#define pagefault_enable()	do { } while (0)
#define pagefault_disabled()	0
#define flush_dcache_page(page)	do { } while (0)
#define flush_kernel_vmap_range(addr, size)	do { } while (0)
#define invalidate_kernel_vmap_range(addr, size)	do { } while (0)
#define zone_device_pages_have_same_pgmap(a, b)	true
#define faults_disabled_mapping()	NULL
long strncpy_from_user(char *dst, const char __user *src, long count);
long strnlen_user(const char __user *str, long n);
#define untagged_addr(addr)	(addr)
#define check_copy_size(addr, bytes, is_source)	true
static inline int check_zeroed_user(const void __user *from, size_t size)
{
	unsigned char buffer[64];
	while (size > 0) {
		size_t chunk = min(size, sizeof(buffer));
		if (copy_from_user(buffer, from, chunk))
			return -EFAULT;
		if (memchr_inv(buffer, 0, chunk))
			return 0;
		from = (const char __user *)from + chunk;
		size -= chunk;
	}
	return 1;
}
static inline int copy_struct_from_user(void *dst, size_t ksize,
	const void __user *src, size_t usize)
{
	size_t size = min(ksize, usize);
	size_t rest = max(ksize, usize) - size;
	if (usize < ksize)
		memset((char *)dst + size, 0, rest);
	else if (usize > ksize) {
		int ret = check_zeroed_user((const char __user *)src + size, rest);
		if (ret <= 0)
			return ret ?: -E2BIG;
	}
	if (copy_from_user(dst, src, size))
		return -EFAULT;
	return 0;
}

/* #pragma mark - DMA mapping */

enum dma_data_direction {
	DMA_BIDIRECTIONAL = 0,
	DMA_TO_DEVICE = 1,
	DMA_FROM_DEVICE = 2,
	DMA_NONE = 3,
};
#define DMA_BIT_MASK(n)	(((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))
#define DMA_MAPPING_ERROR		(~(dma_addr_t)0)
#define DMA_ATTR_WEAK_ORDERING	(1UL << 1)
#define DMA_ATTR_WRITE_COMBINE	(1UL << 2)
#define DMA_ATTR_NO_KERNEL_MAPPING	(1UL << 4)
#define DMA_ATTR_SKIP_CPU_SYNC	(1UL << 5)
#define DMA_ATTR_FORCE_CONTIGUOUS	(1UL << 6)
#define DMA_ATTR_NO_WARN		(1UL << 8)

struct device;
struct scatterlist;
struct sg_table;

#define dma_map_page(dev, page, off, size, dir) \
	((dma_addr_t)(page_to_phys(page) + (off)))
#define dma_map_page_attrs(dev, page, off, size, dir, attrs) \
	dma_map_page(dev, page, off, size, dir)
#define dma_unmap_page(dev, addr, size, dir)	do { } while (0)
#define dma_unmap_page_attrs(dev, addr, size, dir, attrs)	do { } while (0)
#define dma_map_single(dev, ptr, size, dir)	((dma_addr_t)virt_to_phys(ptr))
#define dma_map_single_attrs(dev, ptr, size, dir, attrs) \
	dma_map_single(dev, ptr, size, dir)
#define dma_unmap_single(dev, addr, size, dir)	do { } while (0)
#define dma_map_resource(dev, phys, size, dir, attrs)	((dma_addr_t)(phys))
#define dma_unmap_resource(dev, addr, size, dir, attrs)	do { } while (0)
#define dma_mapping_error(dev, addr)	((addr) == DMA_MAPPING_ERROR ? -ENOMEM : 0)
#define dma_sync_single_for_cpu(dev, addr, size, dir)	do { } while (0)
#define dma_sync_single_for_device(dev, addr, size, dir)	do { } while (0)
#define dma_sync_sg_for_cpu(dev, sg, nents, dir)	do { } while (0)
#define dma_sync_sg_for_device(dev, sg, nents, dir)	do { } while (0)
#define dma_sync_sgtable_for_cpu(dev, sgt, dir)	do { } while (0)
#define dma_sync_sgtable_for_device(dev, sgt, dir)	do { } while (0)
int dma_map_sg_attrs(struct device *dev, struct scatterlist *sg, int nents,
	enum dma_data_direction dir, unsigned long attrs);
#define dma_map_sg(dev, sg, nents, dir)	dma_map_sg_attrs(dev, sg, nents, dir, 0)
#define dma_unmap_sg(dev, sg, nents, dir)	do { } while (0)
#define dma_unmap_sg_attrs(dev, sg, nents, dir, attrs)	do { } while (0)
int dma_map_sgtable(struct device *dev, struct sg_table *sgt,
	enum dma_data_direction dir, unsigned long attrs);
#define dma_unmap_sgtable(dev, sgt, dir, attrs)	do { } while (0)
void *dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
	gfp_t flag, unsigned long attrs);
void dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
	dma_addr_t dma_handle, unsigned long attrs);
#define dma_alloc_coherent(dev, size, handle, gfp) \
	dma_alloc_attrs(dev, size, handle, gfp, 0)
#define dma_free_coherent(dev, size, cpu, handle) \
	dma_free_attrs(dev, size, cpu, handle, 0)
#define dma_alloc_wc(dev, size, handle, gfp) \
	dma_alloc_attrs(dev, size, handle, gfp, DMA_ATTR_WRITE_COMBINE)
#define dma_free_wc(dev, size, cpu, handle)	dma_free_attrs(dev, size, cpu, handle, 0)
int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
#define dma_set_mask_and_coherent(dev, mask)	dma_set_mask(dev, mask)
u64 dma_get_mask(struct device *dev);
#define dma_get_required_mask(dev)	DMA_BIT_MASK(64)
#define dma_addressing_limited(dev)	false
#define dma_max_mapping_size(dev)	SIZE_MAX
#define dma_opt_mapping_size(dev)	SIZE_MAX
#define dma_set_max_seg_size(dev, size)	0
#define dma_get_max_seg_size(dev)	0xffffffffU
#define dma_set_seg_boundary(dev, mask)	0
#define dma_need_sync(dev, addr)	false
#define dma_capable(dev, addr, size, raw)	true
#define dma_pci_p2pdma_supported(dev)	false
#define is_swiotlb_active(dev)		false
#define swiotlb_max_segment()		0
#define dma_is_direct(ops)			true
#define get_dma_ops(dev)			NULL
#define device_iommu_mapped(dev)	false
struct iommu_domain_geometry {
	dma_addr_t aperture_start;
	dma_addr_t aperture_end;
	bool force_aperture;
};
struct iommu_domain {
	unsigned type;
	unsigned long pgsize_bitmap;
	struct iommu_domain_geometry geometry;
};
#define iommu_get_domain_for_dev(dev)	((struct iommu_domain *)NULL)
#define iommu_present(bus)			false
#define IOMMU_DOMAIN_IDENTITY		1
#define IOMMU_DOMAIN_DMA			2
#define dma_get_sgtable(dev, sgt, cpu, handle, size) (-ENXIO)
#define dma_mmap_coherent(dev, vma, cpu, handle, size) (-ENXIO)
#define dma_mmap_attrs(dev, vma, cpu, handle, size, attrs) (-ENXIO)
#define dma_alloc_pages(dev, size, handle, dir, gfp)	NULL
#define dma_free_pages(dev, size, page, handle, dir)	do { } while (0)
#define dma_alloc_noncoherent(dev, size, handle, dir, gfp)	\
	dma_alloc_coherent(dev, size, handle, gfp)
#define dma_free_noncoherent(dev, size, cpu, handle, dir) \
	dma_free_coherent(dev, size, cpu, handle)

static inline bool page_range_contiguous(const struct page *page,
	unsigned long nr_pages)
{
	return true;
}

#include <linux/scatterlist.h>

#endif /* _LX_MM_H */
