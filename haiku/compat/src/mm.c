/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Memory: kmalloc, vmalloc, pages and their registry, vmap, DMA, ioremap,
	user copies.

	kmalloc() blocks carry a header with their size, which gives ksize() and
	krealloc(), and are aligned like Linux's (power-of-two sizes naturally,
	up to a page). vmalloc() memory is an area. */

#include <lx/haiku.h>
#include <linux/hash.h>


/* #pragma mark - kmalloc */

void __cyg_profile_func_enter(void *function, void *site)
	__attribute__((no_instrument_function));

struct kmalloc_header {
	void *base;
	size_t size;
	void *site;			/* who allocated it, for lx_kmalloc_top() */
#ifdef LX_KMALLOC_DEBUG
	unsigned long magic;
#endif
};

#define KMALLOC_MAGIC_SIZE	sizeof(struct kmalloc_header)

#ifdef LX_KMALLOC_DEBUG
/*	A debugging build (KMALLOC_DEBUG=1): each block has bytes after its end
	that must still be there when it is freed, a mark that tells a second
	kfree() and a kfree() of something else, and after kfree() it is filled
	and kept for a while - until 16 MiB or 16384 newer blocks have been
	freed - to see whether anything still writes to it. Any of these
	panics, naming the block's allocation site (an offset as
	haiku/tools/symbolize-trace.sh takes them). */
#define KMALLOC_REDZONE		16
#define KMALLOC_ALIVE		0x6c786b6d616c6c6fUL
#define KMALLOC_FREED		0x6c786b6672656564UL
#define KMALLOC_KEPT		16384
#define KMALLOC_KEPT_BYTES	(16L << 20)

static struct kmalloc_header *kmalloc_kept[KMALLOC_KEPT];
static unsigned int kmalloc_kept_head, kmalloc_kept_count;
static long kmalloc_kept_bytes;
static DEFINE_SPINLOCK(kmalloc_kept_lock);


static void
kmalloc_debug_panic(const struct kmalloc_header *header, const char *what)
{
	static char text[160];
	snprintf(text, sizeof(text), "linux compat: %s (object %p, %zu bytes, "
		"allocated at offset %lx)", what, header + 1, header->size,
		(unsigned long)header->site
			- (unsigned long)&__cyg_profile_func_enter);
	lx_haiku_panic(text);
}


static void
kmalloc_debug_release(struct kmalloc_header *header)
{
	const unsigned char *bytes = (const unsigned char *)(header + 1);
	size_t i;
	for (i = 0; i < header->size + KMALLOC_REDZONE; i++) {
		if (bytes[i] != 0x6b)
			kmalloc_debug_panic(header, "memory written to after kfree()");
	}
	if (header->magic != KMALLOC_FREED)
		kmalloc_debug_panic(header, "header overwritten after kfree()");
	lx_haiku_free(header->base);
}


static void
kmalloc_debug_free(struct kmalloc_header *header)
{
	const unsigned char *bytes = (const unsigned char *)(header + 1);
	struct kmalloc_header *old[8];
	int count = 0, i;

	if (header->magic == KMALLOC_FREED)
		kmalloc_debug_panic(header, "kfree() of freed memory");
	if (header->magic != KMALLOC_ALIVE) {
		lx_haiku_panic("linux compat: kfree() of what kmalloc() did not "
			"return, or its header was overwritten");
	}
	for (i = 0; i < KMALLOC_REDZONE; i++) {
		if (bytes[header->size + i] != 0xa5)
			kmalloc_debug_panic(header, "memory written past its end");
	}
	header->magic = KMALLOC_FREED;
	memset(header + 1, 0x6b, header->size + KMALLOC_REDZONE);

	spin_lock(&kmalloc_kept_lock);
	/* room first (always for this one; the rest a few at a time) */
	while (kmalloc_kept_count > 0 && count < 8
		&& (kmalloc_kept_count == KMALLOC_KEPT
			|| kmalloc_kept_bytes > KMALLOC_KEPT_BYTES)) {
		old[count] = kmalloc_kept[kmalloc_kept_head];
		kmalloc_kept_head = (kmalloc_kept_head + 1) % KMALLOC_KEPT;
		kmalloc_kept_count--;
		kmalloc_kept_bytes -= old[count]->size;
		count++;
	}
	kmalloc_kept[(kmalloc_kept_head + kmalloc_kept_count) % KMALLOC_KEPT]
		= header;
	kmalloc_kept_count++;
	kmalloc_kept_bytes += header->size;
	spin_unlock(&kmalloc_kept_lock);
	for (i = 0; i < count; i++)
		kmalloc_debug_release(old[i]);
}
#else
#define KMALLOC_REDZONE		0
#endif


static size_t
kmalloc_alignment(size_t size)
{
	size_t align = 16;
	if (size >= 32 && size <= PAGE_SIZE && is_power_of_2(size))
		align = size;
	return align;
}


/* for the watchdog: what Linux code has allocated and not freed */
static long lx_kmalloc_live, lx_kmalloc_bytes, lx_pages_live;
static long lx_runs_refused;	/* high-order runs, see lx_alloc_pages() */
static int lx_page_pool_stats(char *buffer, int size);
static LIST_HEAD(lx_kmem_caches);
static DEFINE_MUTEX(lx_kmem_caches_lock);

/*	With "haiku_track_objects 1", the objects of some caches
	(lx_tracked_caches) are kept in a table with the time they were
	allocated, so that a lab report can look at the ones that live long
	(lx_kmem_cache_for_each()). Off, it costs nothing. */
static const char *const lx_tracked_caches[] = { "drm_sched_fence" };
static bool haiku_track_objects;
module_param(haiku_track_objects, bool, 0644);
static bool lx_tracked_ever;

struct lx_tracked_object {
	struct hlist_node node;
	void *object;
	s64 time;
};

#define LX_TRACKED_BITS	12
static struct hlist_head lx_tracked[1 << LX_TRACKED_BITS];
static DEFINE_SPINLOCK(lx_tracked_lock);


static unsigned int
lx_tracked_bucket(const void *object)
{
	return hash_ptr((void *)object, LX_TRACKED_BITS);
}


static void
lx_track(void *object)
{
	struct lx_tracked_object *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL)
		return;
	entry->object = object;
	entry->time = lx_system_time_ns();
	spin_lock(&lx_tracked_lock);
	hlist_add_head(&entry->node, &lx_tracked[lx_tracked_bucket(object)]);
	spin_unlock(&lx_tracked_lock);
}


static void
lx_untrack(void *object)
{
	struct lx_tracked_object *entry, *found = NULL;
	spin_lock(&lx_tracked_lock);
	hlist_for_each_entry(entry, &lx_tracked[lx_tracked_bucket(object)], node) {
		if (entry->object == object) {
			hlist_del(&entry->node);
			found = entry;
			break;
		}
	}
	spin_unlock(&lx_tracked_lock);
	kfree(found);
}


/*	Calls \a function for every live object of the tracked cache \a name,
	with the tracking lock held (it must not allocate or block). */
void
lx_kmem_cache_for_each(const char *name,
	void (*function)(void *object, s64 age_ns, void *data), void *data)
{
	struct lx_tracked_object *entry;
	s64 now = lx_system_time_ns();
	int i;
	(void)name;
	spin_lock(&lx_tracked_lock);
	for (i = 0; i < (1 << LX_TRACKED_BITS); i++) {
		hlist_for_each_entry(entry, &lx_tracked[i], node)
			function(entry->object, now - entry->time, data);
	}
	spin_unlock(&lx_tracked_lock);
}


/*	Objects alive per call site of kmalloc() and friends (the return
	address into the caller), for the watchdog: a leak shows as a site
	whose count only grows. Open addressing on the site; a full table
	counts the rest at slot 0. */
#define LX_SITE_BITS	12
static struct {
	void *site;
	long live;
	long bytes;
} lx_sites[1 << LX_SITE_BITS];


static int
lx_site_slot(void *site)
{
	unsigned int start = hash_ptr(site, LX_SITE_BITS), i;
	for (i = 0; i < (1u << LX_SITE_BITS); i++) {
		unsigned int slot = (start + i) & ((1u << LX_SITE_BITS) - 1);
		void *current_site = __atomic_load_n(&lx_sites[slot].site,
			__ATOMIC_ACQUIRE);
		if (current_site == site)
			return slot;
		if (current_site == NULL) {
			void *expected = NULL;
			if (__atomic_compare_exchange_n(&lx_sites[slot].site, &expected,
					site, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
				|| expected == site)
				return slot;
		}
	}
	return 0;
}


/*	The \a count sites with the most objects alive, as "offset:objects"
	from the trace anchor (haiku/tools/symbolize-trace.sh's). */
int
lx_kmalloc_top(char *buffer, int size, int count)
{
	int top[8], n = 0, i, j, slot;
	if (count > 8)
		count = 8;
	for (i = 0; i < count; i++)
		top[i] = -1;
	for (slot = 0; slot < (1 << LX_SITE_BITS); slot++) {
		long live = READ_ONCE(lx_sites[slot].live);
		if (READ_ONCE(lx_sites[slot].site) == NULL || live <= 0)
			continue;
		for (i = 0; i < count; i++) {
			if (top[i] < 0 || live > READ_ONCE(lx_sites[top[i]].live)) {
				for (j = count - 1; j > i; j--)
					top[j] = top[j - 1];
				top[i] = slot;
				break;
			}
		}
	}
	for (i = 0; i < count && top[i] >= 0; i++) {
		n += scnprintf(buffer + n, size - n, " %lx:%ld",
			(unsigned long)lx_sites[top[i]].site
				- (unsigned long)&__cyg_profile_func_enter,
			READ_ONCE(lx_sites[top[i]].live));
	}
	return n;
}


static void *
lx_kmalloc_site(size_t size, gfp_t flags, void *site);


void *
lx_kmalloc(size_t size, gfp_t flags)
{
	return lx_kmalloc_site(size, flags, __builtin_return_address(0));
}


static void *
lx_kmalloc_site(size_t size, gfp_t flags, void *site)
{
	size_t align;
	char *base, *object;
	struct kmalloc_header *header;
	int slot;

	if (size == 0)
		return ZERO_SIZE_PTR;
	align = kmalloc_alignment(size);
	base = lx_haiku_malloc(size + align + KMALLOC_MAGIC_SIZE + KMALLOC_REDZONE);
	if (base == NULL) {
		if (!(flags & __GFP_NOWARN))
			pr_warn("linux compat: kmalloc(%zu) failed\n", size);
		return NULL;
	}
	object = (char *)ALIGN((uintptr_t)base + KMALLOC_MAGIC_SIZE, align);
	header = (struct kmalloc_header *)object - 1;
	header->base = base;
	header->size = size;
	header->site = site;
#ifdef LX_KMALLOC_DEBUG
	header->magic = KMALLOC_ALIVE;
	memset(object + size, 0xa5, KMALLOC_REDZONE);
#endif
	slot = lx_site_slot(site);
	__atomic_add_fetch(&lx_sites[slot].live, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&lx_sites[slot].bytes, (long)size, __ATOMIC_RELAXED);
	__atomic_add_fetch(&lx_kmalloc_live, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&lx_kmalloc_bytes, (long)size, __ATOMIC_RELAXED);
	if (flags & __GFP_ZERO)
		memset(object, 0, size);
	return object;
}


void
lx_kfree(const void *object)
{
	struct kmalloc_header *header;
	if (ZERO_OR_NULL_PTR(object))
		return;
	if (((uintptr_t)object & (PAGE_SIZE - 1)) == 0
		&& lx_haiku_area_free((void *)object) == 0)
		return;		/* vmalloc() memory passed to kvfree() */
	header = (struct kmalloc_header *)object - 1;
	{
		int slot = lx_site_slot(header->site);
		__atomic_sub_fetch(&lx_sites[slot].live, 1, __ATOMIC_RELAXED);
		__atomic_sub_fetch(&lx_sites[slot].bytes, (long)header->size,
			__ATOMIC_RELAXED);
	}
	__atomic_sub_fetch(&lx_kmalloc_live, 1, __ATOMIC_RELAXED);
	__atomic_sub_fetch(&lx_kmalloc_bytes, (long)header->size, __ATOMIC_RELAXED);
#ifdef LX_KMALLOC_DEBUG
	kmalloc_debug_free(header);
#else
	lx_haiku_free(header->base);
#endif
}


size_t
ksize(const void *object)
{
	if (ZERO_OR_NULL_PTR(object))
		return 0;
	return ((const struct kmalloc_header *)object - 1)->size;
}


void *
lx_krealloc(const void *object, size_t new_size, gfp_t flags)
{
	void *result;
	size_t old_size;
	if (new_size == 0) {
		lx_kfree(object);
		return ZERO_SIZE_PTR;
	}
	old_size = ksize(object);
	if (old_size >= new_size && !ZERO_OR_NULL_PTR(object))
		return (void *)object;
	result = lx_kmalloc_site(new_size, flags & ~__GFP_ZERO,
		__builtin_return_address(0));
	if (result == NULL)
		return NULL;
	if (old_size > 0)
		memcpy(result, object, old_size);
	if (flags & __GFP_ZERO)
		memset((char *)result + old_size, 0, new_size - old_size);
	lx_kfree(object);
	return result;
}


void *
lx_vmalloc(size_t size, gfp_t flags)
{
	if (size == 0)
		return NULL;
	return lx_haiku_area_alloc(size, (flags & __GFP_ZERO) != 0);
}


void
lx_vfree(const void *address)
{
	if (address == NULL)
		return;
	if (lx_haiku_area_free((void *)address) != 0)
		lx_kfree(address);
}


bool
is_vmalloc_addr(const void *address)
{
	return lx_haiku_is_area_alloc(address);
}


/* #pragma mark - slab caches */


struct kmem_cache *
kmem_cache_create(const char *name, unsigned int size, unsigned int align,
	slab_flags_t flags, void (*ctor)(void *))
{
	struct kmem_cache *cache = kzalloc(sizeof(*cache), GFP_KERNEL);
	unsigned int i;
	if (cache == NULL)
		return NULL;
	cache->name = name;
	cache->size = size;
	cache->align = align;
	cache->flags = flags;
	cache->ctor = ctor;
	for (i = 0; i < ARRAY_SIZE(lx_tracked_caches); i++) {
		if (name != NULL && strcmp(name, lx_tracked_caches[i]) == 0)
			cache->tracked = true;
	}
	mutex_lock(&lx_kmem_caches_lock);
	list_add_tail(&cache->entry, &lx_kmem_caches);
	mutex_unlock(&lx_kmem_caches_lock);
	return cache;
}


void
kmem_cache_destroy(struct kmem_cache *cache)
{
	if (cache == NULL)
		return;
	mutex_lock(&lx_kmem_caches_lock);
	list_del(&cache->entry);
	mutex_unlock(&lx_kmem_caches_lock);
	kfree(cache);
}


void *
kmem_cache_alloc(struct kmem_cache *cache, gfp_t flags)
{
	void *object = lx_kmalloc_site(cache->size, flags,
		__builtin_return_address(0));
	if (object != NULL) {
		__atomic_add_fetch(&cache->live, 1, __ATOMIC_RELAXED);
		if (cache->tracked && READ_ONCE(haiku_track_objects)) {
			WRITE_ONCE(lx_tracked_ever, true);
			lx_track(object);
		}
		if (cache->ctor != NULL)
			cache->ctor(object);
	}
	return object;
}


void
kmem_cache_free(struct kmem_cache *cache, void *object)
{
	if (object != NULL) {
		__atomic_sub_fetch(&cache->live, 1, __ATOMIC_RELAXED);
		if (cache->tracked && READ_ONCE(lx_tracked_ever))
			lx_untrack(object);
	}
	kfree(object);
}


/*	For the watchdog: kmalloc() objects and bytes, pages, and the caches
	with the most objects. */
int
lx_mm_stats(char *buffer, int size)
{
	struct kmem_cache *cache, *top[4] = {};
	int n, i, j;
	n = scnprintf(buffer, size,
		"kmalloc %ld/%ld KiB pages %ld runs refused %ld",
		__atomic_load_n(&lx_kmalloc_live, __ATOMIC_RELAXED),
		__atomic_load_n(&lx_kmalloc_bytes, __ATOMIC_RELAXED) / 1024,
		__atomic_load_n(&lx_pages_live, __ATOMIC_RELAXED),
		__atomic_load_n(&lx_runs_refused, __ATOMIC_RELAXED));
	n += lx_page_pool_stats(buffer + n, size - n);
	mutex_lock(&lx_kmem_caches_lock);
	list_for_each_entry(cache, &lx_kmem_caches, entry) {
		for (i = 0; i < 4; i++) {
			if (top[i] == NULL || READ_ONCE(cache->live) > READ_ONCE(top[i]->live)) {
				for (j = 3; j > i; j--)
					top[j] = top[j - 1];
				top[i] = cache;
				break;
			}
		}
	}
	for (i = 0; i < 4 && top[i] != NULL; i++) {
		n += scnprintf(buffer + n, size - n, " %s %ld", top[i]->name,
			READ_ONCE(top[i]->live));
	}
	mutex_unlock(&lx_kmem_caches_lock);
	return n;
}


/* #pragma mark - pages */

/*	Every allocation of pages is a run of physically contiguous pages, and
	its array of struct page is one allocation that starts with the first
	page. pfn_to_page() finds a page of RAM through a table by frame number,
	without a lock: a directory with a leaf for every 512 frames, a leaf
	allocated when a frame in it is first described and then kept (at most
	8 bytes per frame of RAM; Linux's memmap takes 64). Frames beyond the
	end of RAM (device memory) are in an rbtree. Pages that came from
	elsewhere (virt_to_page() of kmalloc() memory) get single-page entries
	that stay, until the frame is allocated here. */

#define PAGE_LEAF_BITS		9
#define PAGE_LEAF_FRAMES	(1UL << PAGE_LEAF_BITS)

struct page_leaf {
	struct page *pages[PAGE_LEAF_FRAMES];
};

static struct page_leaf **page_directory;
static unsigned long page_frames;		/* the frames the directory covers */
static DEFINE_MUTEX(page_directory_lock);

struct device_page {
	struct rb_node node;
	unsigned long pfn;
	struct page page;
};

static struct rb_root device_pages = RB_ROOT;
static DEFINE_SPINLOCK(device_pages_lock);
struct page *lx_zero_page;


static struct page_leaf **
page_directory_get(void)
{
	struct page_leaf **directory = smp_load_acquire(&page_directory);
	if (directory != NULL)
		return directory;

	mutex_lock(&page_directory_lock);
	directory = page_directory;
	if (directory == NULL) {
		unsigned long frames = lx_haiku_ram_frames();
		directory = vzalloc(DIV_ROUND_UP(frames, PAGE_LEAF_FRAMES)
			* sizeof(*directory));
		if (directory != NULL) {
			page_frames = frames;
			smp_store_release(&page_directory, directory);
		}
	}
	mutex_unlock(&page_directory_lock);
	return directory;
}


/*	The table entry of frame \a pfn, its leaf allocated if \a create is set;
	NULL for frames beyond RAM, or without a leaf. */
static struct page **
page_slot(unsigned long pfn, bool create)
{
	struct page_leaf **directory = page_directory_get();
	struct page_leaf *leaf, *other;

	if (directory == NULL || pfn >= page_frames)
		return NULL;
	leaf = READ_ONCE(directory[pfn >> PAGE_LEAF_BITS]);
	if (leaf == NULL) {
		if (!create)
			return NULL;
		leaf = kzalloc(sizeof(*leaf), GFP_KERNEL);
		if (leaf == NULL)
			return NULL;
		other = cmpxchg(&directory[pfn >> PAGE_LEAF_BITS], NULL, leaf);
		if (other != NULL) {
			kfree(leaf);
			leaf = other;
		}
	}
	return &leaf->pages[pfn & (PAGE_LEAF_FRAMES - 1)];
}


static void
page_init(struct page *page, unsigned long pfn, void *haiku_page,
	struct page *head)
{
	page->lx_phys = PFN_PHYS(pfn);
	page->lx_haiku_page = haiku_page;
	page->lx_head = head;
	atomic_set(&page->_refcount, 1);
	INIT_LIST_HEAD(&page->lru);
}


/*	Describes the frames [pfn, pfn + count), Haiku's pages from \a haiku_page
	on, as one allocation of order \a order: its array of struct page, NULL
	if there was no memory for it. */
static struct page *
pages_describe(void *haiku_page, unsigned long pfn, unsigned long count,
	unsigned int order)
{
	struct page *pages = kcalloc(count, sizeof(*pages), GFP_KERNEL);
	unsigned long i;

	if (pages == NULL)
		return NULL;
	for (i = 0; i < count; i++) {
		struct page **slot = page_slot(pfn + i, true);
		if (slot == NULL) {
			while (i-- > 0)
				WRITE_ONCE(*page_slot(pfn + i, false), NULL);
			kfree(pages);
			return NULL;
		}
		page_init(&pages[i], pfn + i, haiku_page, pages);
		/* an entry from virt_to_page() before Haiku freed the frame is
		   replaced, and left to whoever still has it */
		WRITE_ONCE(*slot, &pages[i]);
	}
	pages[0].lx_order = order;
	pages[0].lx_count = count;
	return pages;
}


/* gives the \a count pages of an allocation back to Haiku */
static void
pages_release(struct page *pages, unsigned long count)
{
	unsigned long pfn = page_to_pfn(pages), i;
	for (i = 0; i < count; i++)
		WRITE_ONCE(*page_slot(pfn + i, false), NULL);
	lx_haiku_pages_free(pages->lx_haiku_page, count);
	kfree(pages);
}


/* #pragma mark - page pool */

/*	Freed pages are kept for the next allocation (airos-works/haiku#110). TTM
	gives a buffer's pages back one allocation at a time and asks for the
	next buffer's again: 2 MiB first, then smaller orders, down to single
	pages. Haiku has no buddy allocator, and its run allocator searches with
	the free page queues locked (lx_alloc_pages() refuses such searches), so
	every buffer used to take single pages: per page a reservation, the
	free page queues' lock, a struct page allocation, its frame table
	entries and zeroing, and the same again to free it. That was about
	0.25 us a page each way on the Framework 13 (AI 340), ~70 us to create
	and ~80 us to free a 1 MiB buffer, and twice that when TTM frees a busy
	buffer on its worker while the next one is made.

	The pool keeps freed allocations of every order up to 2 MiB
	(PAGE_POOL_RUN_ORDER) with their struct pages and table entries, and
	hands them out again: a buffer takes one allocation per 2 MiB, 1 MiB,
	... instead of one per page, and the GPU maps large ones with large
	fragments. An order the pool has none of is split off a larger
	allocation (the upper half of one gets struct pages of its own each
	time, the lower half keeps the old ones), down from 2 MiB runs. Those
	the worker collects after TTM asked for allocations the pool did not
	have: it looks for a free 2 MiB block in Haiku's page array without a
	lock and then allocates exactly that block, which keeps Haiku's free
	page queues locked for 512 pages, not for a search. Pieces are not
	merged again; they go back to Haiku as they are.

	The worker also zeroes what was freed, so that an allocation that wants
	zeroed pages (every buffer does) usually finds some ready; otherwise
	they are zeroed on the spot, which still costs less than Haiku's
	allocator. The pool gives everything back to Haiku when it has not been
	used for PAGE_POOL_IDLE, when Haiku runs low on pages, when free pages
	get scarce, and when the driver stops. Allocations with address
	restrictions (DMA32) do not come from the pool.

	"drmparam haiku_page_pool_mb <n>": the most it keeps (-1: a 32nd of
	RAM, at most 256 MiB; 0: none). */

static int lx_page_pool_mb = -1;
module_param_named(haiku_page_pool_mb, lx_page_pool_mb, int, 0644);

#define PAGE_POOL_BATCH		32
#define PAGE_POOL_IDLE		(10 * HZ)
#define PAGE_POOL_RUN_ORDER	9
#define PAGE_POOL_RUN		(1UL << PAGE_POOL_RUN_ORDER)
#define PAGE_POOL_ORDERS	(PAGE_POOL_RUN_ORDER + 1)
#define PAGE_POOL_COLLECT	32			/* runs collected at a time, at most */
#define PAGE_POOL_SCAN		(1UL << 18)	/* frames looked at for one run */

struct page_pool_order {
	struct list_head clear;		/* zeroed allocations (first pages) */
	struct list_head dirty;		/* still to be zeroed */
	long clear_count, dirty_count;
};

static struct page_pool_order page_pool[PAGE_POOL_ORDERS];
static long page_pool_pages;				/* in all lists */
static bool page_pool_running;
static long page_pool_auto_limit;			/* pages, for -1 */
static long page_pool_headroom;				/* free pages Haiku keeps */
static unsigned long page_pool_used;		/* jiffies of the last take/give */
static long run_pool_wanted;				/* asked for and not there */
static unsigned long run_pool_cursor;		/* where to look for runs next */
static long page_pool_hits, page_pool_misses, page_pool_splits;
static long page_pool_inline_clears, run_pool_collected, run_pool_not_found;
static long page_pool_returned;
static struct workqueue_struct *page_pool_wq;
static struct work_struct page_pool_work;
static struct delayed_work page_pool_trim_work;


/* the most pages the pool may hold now, 0 if it is off */
static long
page_pool_limit(void)
{
	long megabytes = READ_ONCE(lx_page_pool_mb);
	if (!READ_ONCE(page_pool_running) || megabytes == 0)
		return 0;
	if (megabytes < 0)
		return page_pool_auto_limit;
	return megabytes << (20 - PAGE_SHIFT);
}


/* with the pool's lock held */
static void
page_pool_add(struct page *pages, unsigned int order, bool dirty)
{
	struct page_pool_order *list = &page_pool[order];
	/* in the pool, pages have no allocation: a second free is caught */
	pages->lx_count = 0;
	pages->lx_order = order;
	if (dirty) {
		list_add(&pages->lru, &list->dirty);
		list->dirty_count++;
	} else {
		list_add(&pages->lru, &list->clear);
		list->clear_count++;
	}
	page_pool_pages += 1L << order;
}


/* with the pool's lock held: the first allocation of \a order on \a list */
static struct page *
page_pool_remove(unsigned int order, bool dirty)
{
	struct page_pool_order *list = &page_pool[order];
	struct page *pages = list_first_entry_or_null(dirty ? &list->dirty
		: &list->clear, struct page, lru);
	if (pages == NULL)
		return NULL;
	list_del(&pages->lru);
	if (dirty)
		list->dirty_count--;
	else
		list->clear_count--;
	page_pool_pages -= 1L << order;
	return pages;
}


/* the \a count pages from \a pages made like newly allocated ones */
static void
page_pool_reuse(struct page *pages, unsigned long count, unsigned int order)
{
	unsigned long i;
	for (i = 0; i < count; i++) {
		struct page *page = &pages[i];
		page->flags = 0;
		page->private = 0;
		atomic_set(&page->_refcount, 1);
		atomic_set(&page->_mapcount, 0);
		page->lx_head = pages;
		page->mapping = NULL;
		page->index = 0;
		INIT_LIST_HEAD(&page->lru);
	}
	pages->lx_order = order;
	pages->lx_count = count;
}


/*	Splits \a pages, an allocation of \a from pages taken from the pool,
	down to \a to: the upper halves go back into the pool with struct pages
	of their own, \a pages keeps the lower one. Returns the order \a pages
	has now (more than \a to if memory for struct pages ran out). */
static unsigned int
page_pool_split(struct page *pages, unsigned int from, unsigned int to,
	bool dirty)
{
	while (from > to) {
		unsigned long half = 1UL << (from - 1);
		unsigned long pfn = page_to_pfn(pages) + half, state;
		struct page *upper = pages_describe(lx_haiku_vm_page(pfn), pfn, half,
			from - 1);
		if (upper == NULL)
			break;
		state = lx_haiku_page_pool_lock();
		page_pool_add(upper, from - 1, dirty);
		lx_haiku_page_pool_unlock(state);
		from--;
		__atomic_add_fetch(&page_pool_splits, 1, __ATOMIC_RELAXED);
	}
	return from;
}


/*	Takes an allocation of \a order from the pool, zeroed if \a zeroed, or
	splits one off a larger one; NULL if there is none. Single pages are not
	split off: that would describe up to 511 other pages, at 2 MiB, which
	costs more than Haiku's allocator does for one page. */
static struct page *
page_pool_take(unsigned int order, bool zeroed)
{
	struct page *pages = NULL;
	unsigned long state, count = 1UL << order, i;
	unsigned int found = order, last, pass;
	bool dirty = false;

	if (!READ_ONCE(page_pool_running) || order > PAGE_POOL_RUN_ORDER)
		return NULL;
	last = order == 0 ? 0 : PAGE_POOL_RUN_ORDER;
	state = lx_haiku_page_pool_lock();
	/* what needs no zeroing first; of that, the smallest order that does */
	for (pass = 0; pages == NULL && pass < 2; pass++) {
		dirty = zeroed ? pass == 1 : pass == 0;
		for (found = order; found <= last; found++) {
			pages = page_pool_remove(found, dirty);
			if (pages != NULL)
				break;
		}
	}
	if (pages == NULL && order > 0)
		run_pool_wanted++;
	lx_haiku_page_pool_unlock(state);

	if (pages == NULL) {
		__atomic_add_fetch(&page_pool_misses, 1, __ATOMIC_RELAXED);
		if (order > 0)
			queue_work(page_pool_wq, &page_pool_work);
		return NULL;
	}
	WRITE_ONCE(page_pool_used, jiffies);
	__atomic_add_fetch(&page_pool_hits, 1, __ATOMIC_RELAXED);
	if (found > order) {
		found = page_pool_split(pages, found, order, dirty);
		if (found != order) {
			/* no memory to describe the pieces: back with the rest */
			state = lx_haiku_page_pool_lock();
			page_pool_add(pages, found, dirty);
			lx_haiku_page_pool_unlock(state);
			return NULL;
		}
	}
	if (zeroed && dirty) {
		for (i = 0; i < count; i++)
			clear_page(page_address(&pages[i]));
		__atomic_add_fetch(&page_pool_inline_clears, count, __ATOMIC_RELAXED);
	}
	page_pool_reuse(pages, count, order);
	return pages;
}


/* keeps a freed allocation, if the pool has room for it */
static bool
page_pool_give(struct page *pages, unsigned long count)
{
	unsigned int order = pages->lx_order;
	long limit = page_pool_limit();
	unsigned long state;
	bool kick;

	if (limit == 0 || order > PAGE_POOL_RUN_ORDER || count != 1UL << order
		|| (long)lx_haiku_unused_pages() < page_pool_headroom)
		return false;
	state = lx_haiku_page_pool_lock();
	if (page_pool_pages + (long)count > limit) {
		lx_haiku_page_pool_unlock(state);
		return false;
	}
	kick = page_pool[order].dirty_count == 0;
	page_pool_add(pages, order, true);
	lx_haiku_page_pool_unlock(state);
	WRITE_ONCE(page_pool_used, jiffies);

	/* a worker that runs takes what is freed meanwhile as well */
	if (kick)
		queue_work(page_pool_wq, &page_pool_work);
	return true;
}


/* gives pages back to Haiku until the pool holds no more than \a keep */
static void
page_pool_shrink(long keep)
{
	for (;;) {
		struct page *batch[PAGE_POOL_BATCH];
		unsigned int orders[PAGE_POOL_BATCH];
		unsigned long state;
		long released = 0;
		int count = 0, i;

		state = lx_haiku_page_pool_lock();
		while (count < PAGE_POOL_BATCH && page_pool_pages > keep) {
			struct page *pages = NULL;
			int order;
			/* large ones first, what is not zeroed yet first */
			for (order = PAGE_POOL_RUN_ORDER; pages == NULL && order >= 0;
					order--) {
				pages = page_pool_remove(order, true);
				if (pages == NULL)
					pages = page_pool_remove(order, false);
				if (pages != NULL)
					orders[count] = order;
			}
			batch[count++] = pages;
		}
		lx_haiku_page_pool_unlock(state);
		if (count == 0)
			break;

		for (i = 0; i < count; i++) {
			pages_release(batch[i], 1UL << orders[i]);
			released += 1L << orders[i];
		}
		__atomic_add_fetch(&page_pool_returned, released, __ATOMIC_RELAXED);
	}
}


/*	Finds a free 2 MiB block, allocates it from Haiku, zeroed, and puts it
	into the pool; false if there was none, or no room for it. */
static bool
run_pool_collect(void)
{
	unsigned long frames = page_frames, start = READ_ONCE(run_pool_cursor);
	unsigned long long physical;
	unsigned long pfn, state;
	struct page *pages;
	void *haiku_page;
	bool room;

	if (frames == 0 || (long)lx_haiku_unused_pages()
			< page_pool_headroom + (long)PAGE_POOL_RUN)
		return false;
	state = lx_haiku_page_pool_lock();
	room = page_pool_pages + (long)PAGE_POOL_RUN <= page_pool_limit();
	lx_haiku_page_pool_unlock(state);
	if (!room)
		return false;

	if (start >= frames)
		start = 0;
	pfn = lx_haiku_find_free_run(start, frames, PAGE_POOL_RUN, PAGE_POOL_SCAN);
	if (pfn == 0 && start > 0)
		pfn = lx_haiku_find_free_run(0, start, PAGE_POOL_RUN, PAGE_POOL_SCAN);
	if (pfn == 0) {
		__atomic_add_fetch(&run_pool_not_found, 1, __ATOMIC_RELAXED);
		return false;
	}
	WRITE_ONCE(run_pool_cursor, pfn + PAGE_POOL_RUN);
	haiku_page = lx_haiku_pages_alloc_at(pfn, PAGE_POOL_RUN, true, &physical);
	if (haiku_page == NULL) {
		/* taken meanwhile */
		__atomic_add_fetch(&run_pool_not_found, 1, __ATOMIC_RELAXED);
		return false;
	}
	pages = pages_describe(haiku_page, physical >> PAGE_SHIFT, PAGE_POOL_RUN,
		PAGE_POOL_RUN_ORDER);
	if (pages == NULL) {
		lx_haiku_pages_free(haiku_page, PAGE_POOL_RUN);
		return false;
	}
	state = lx_haiku_page_pool_lock();
	page_pool_add(pages, PAGE_POOL_RUN_ORDER, false);
	lx_haiku_page_pool_unlock(state);
	__atomic_add_fetch(&run_pool_collected, 1, __ATOMIC_RELAXED);
	return true;
}


/*	The worker: zeroes what was freed, and collects the runs that were
	asked for. */
static void
page_pool_work_fn(struct work_struct *work)
{
	unsigned int order;
	long wanted;

	for (order = 0; order < PAGE_POOL_ORDERS; order++) {
		for (;;) {
			struct page *batch[PAGE_POOL_BATCH];
			unsigned long state, count = 1UL << order, i;
			int taken = 0, j;

			/* single pages a batch at a time, larger ones one by one */
			state = lx_haiku_page_pool_lock();
			while (taken < (order == 0 ? PAGE_POOL_BATCH : 1)
				&& (batch[taken] = page_pool_remove(order, true)) != NULL)
				taken++;
			lx_haiku_page_pool_unlock(state);
			if (taken == 0)
				break;

			for (j = 0; j < taken; j++) {
				for (i = 0; i < count; i++)
					clear_page(page_address(&batch[j][i]));
			}

			state = lx_haiku_page_pool_lock();
			for (j = 0; j < taken; j++)
				page_pool_add(batch[j], order, false);
			lx_haiku_page_pool_unlock(state);
		}
	}

	{
		unsigned long state = lx_haiku_page_pool_lock();
		wanted = min_t(long, run_pool_wanted, PAGE_POOL_COLLECT);
		run_pool_wanted = 0;
		lx_haiku_page_pool_unlock(state);
	}
	while (wanted-- > 0 && run_pool_collect())
		;

	queue_delayed_work(page_pool_wq, &page_pool_trim_work, HZ);
}


/* the worker, every second while the pool holds pages */
static void
page_pool_trim(struct work_struct *work)
{
	long keep = page_pool_limit();

	if (time_after(jiffies, READ_ONCE(page_pool_used) + PAGE_POOL_IDLE)
		|| (long)lx_haiku_unused_pages() < page_pool_headroom)
		keep = 0;
	page_pool_shrink(keep);

	if (READ_ONCE(page_pool_pages) != 0)
		queue_delayed_work(page_pool_wq, &page_pool_trim_work, HZ);
}


/* Haiku's low resource manager: free pages or memory are getting scarce */
static void
page_pool_low_memory(int level)
{
	page_pool_shrink(0);
}


static int
lx_page_pool_stats(char *buffer, int size)
{
	int n = scnprintf(buffer, size, " pool %ld pages (by order, zeroed+not:",
		READ_ONCE(page_pool_pages));
	unsigned int order;
	for (order = 0; order < PAGE_POOL_ORDERS; order++) {
		long clear = READ_ONCE(page_pool[order].clear_count);
		long dirty = READ_ONCE(page_pool[order].dirty_count);
		if (clear + dirty > 0)
			n += scnprintf(buffer + n, size - n, " %u:%ld+%ld", order, clear,
				dirty);
	}
	n += scnprintf(buffer + n, size - n, ") hits %ld misses %ld splits %ld, "
		"zeroed on use %ld, runs collected %ld not found %ld, returned %ld",
		__atomic_load_n(&page_pool_hits, __ATOMIC_RELAXED),
		__atomic_load_n(&page_pool_misses, __ATOMIC_RELAXED),
		__atomic_load_n(&page_pool_splits, __ATOMIC_RELAXED),
		__atomic_load_n(&page_pool_inline_clears, __ATOMIC_RELAXED),
		__atomic_load_n(&run_pool_collected, __ATOMIC_RELAXED),
		__atomic_load_n(&run_pool_not_found, __ATOMIC_RELAXED),
		__atomic_load_n(&page_pool_returned, __ATOMIC_RELAXED));
	return n;
}


/*	Starts the pool (after lx_workqueue_init()); a driver that does not
	frees its pages without it. */
int
lx_page_pool_init(void)
{
	long total = (long)lx_haiku_total_pages();
	unsigned int order;

	if (page_directory_get() == NULL)
		return -ENOMEM;
	page_pool_wq = alloc_workqueue("linux page pool", WQ_UNBOUND, 1);
	if (page_pool_wq == NULL)
		return -ENOMEM;
	for (order = 0; order < PAGE_POOL_ORDERS; order++) {
		INIT_LIST_HEAD(&page_pool[order].clear);
		INIT_LIST_HEAD(&page_pool[order].dirty);
	}
	INIT_WORK(&page_pool_work, page_pool_work_fn);
	INIT_DELAYED_WORK(&page_pool_trim_work, page_pool_trim);
	page_pool_auto_limit = min(total / 32, 256L << (20 - PAGE_SHIFT));
	page_pool_headroom = max(total / 32, 64L << (20 - PAGE_SHIFT));
	/* runs are looked for from the upper half of RAM up, where fewer
	   long-lived allocations are */
	run_pool_cursor = page_frames / 2;
	lx_haiku_low_memory_hook(page_pool_low_memory);
	WRITE_ONCE(page_pool_running, true);
	return 0;
}


/*	Stops the pool and gives all of its pages back (before
	lx_workqueue_exit(), with no pages being freed any more). */
void
lx_page_pool_exit(void)
{
	if (page_pool_wq == NULL)
		return;
	WRITE_ONCE(page_pool_running, false);
	lx_haiku_low_memory_hook(NULL);
	cancel_work_sync(&page_pool_work);
	cancel_delayed_work_sync(&page_pool_trim_work);
	destroy_workqueue(page_pool_wq);
	page_pool_wq = NULL;
	page_pool_shrink(0);
}


/* #pragma mark - allocating pages */


struct page *
lx_alloc_pages(gfp_t gfp, unsigned int order)
{
	unsigned long count = 1UL << order;
	unsigned long long physical;
	unsigned long long max_address = 0;
	bool zeroed = (gfp & __GFP_ZERO) != 0;
	struct page *pages;
	void *haiku_page;

	if (gfp & (__GFP_DMA32 | __GFP_DMA))
		max_address = 1ULL << 32;
	else {
		pages = page_pool_take(order, zeroed);
		if (pages != NULL) {
			__atomic_add_fetch(&lx_pages_live, (long)count, __ATOMIC_RELAXED);
			return pages;
		}
	}

	/*	Linux asks for high-order runs opportunistically (__GFP_NORETRY:
		TTM's pool tries 2 MiB, then 1 MiB, ..., down to single pages). Haiku
		has no buddy allocator: a run is a search through all of its pages
		from the start, with the free page queues write locked, so every page
		fault and page allocation in the system waits for it, whether it
		finds a run or not. It took 2.3 ms for each 1 MiB buffer in GTT on
		the Framework 13 (jmgasper/haiku#36). These requests fail at once
		when the pool has nothing for them, and the caller tries a smaller
		order. Runs it cannot do without (DMA buffers) still search. */
	if (order > 0 && (gfp & __GFP_NORETRY) != 0) {
		__atomic_add_fetch(&lx_runs_refused, 1, __ATOMIC_RELAXED);
		return NULL;
	}

	haiku_page = lx_haiku_pages_alloc(count, max_address, zeroed, &physical);
	if (haiku_page == NULL)
		return NULL;
	pages = pages_describe(haiku_page, physical >> PAGE_SHIFT, count, order);
	if (pages == NULL) {
		lx_haiku_pages_free(haiku_page, count);
		return NULL;
	}
	__atomic_add_fetch(&lx_pages_live, (long)count, __ATOMIC_RELAXED);
	return pages;
}


void
lx_free_pages(struct page *page, unsigned int order)
{
	unsigned long pfn, count;
	struct page **slot;

	if (page == NULL)
		return;
	pfn = page_to_pfn(page);
	slot = page_slot(pfn, false);
	if (slot == NULL || READ_ONCE(*slot) != page || page->lx_head != page
		|| page->lx_haiku_page == NULL) {
		WARN(1, "freeing pages that were not allocated (pfn %lx)\n", pfn);
		return;
	}
	count = page->lx_count;
	if (count == 0) {
		WARN(1, "freeing pages twice (pfn %lx)\n", pfn);
		return;
	}
	__atomic_sub_fetch(&lx_pages_live, (long)count, __ATOMIC_RELAXED);
	if (page_pool_give(page, count))
		return;
	pages_release(page, count);
}


void
put_page(struct page *page)
{
	if (atomic_dec_and_test(&page->_refcount)) {
		struct page *head = page->lx_head != NULL ? page->lx_head : page;
		if (head == page && page->lx_haiku_page != NULL)
			lx_free_pages(page, page->lx_order);
	}
}


static struct page *
device_page_find(unsigned long pfn)
{
	struct rb_node *node = device_pages.rb_node;
	while (node != NULL) {
		struct device_page *entry = rb_entry(node, struct device_page, node);
		if (pfn < entry->pfn)
			node = node->rb_left;
		else if (pfn > entry->pfn)
			node = node->rb_right;
		else
			return &entry->page;
	}
	return NULL;
}


/* a frame beyond RAM: described from its first lookup on */
static struct page *
device_page(unsigned long pfn)
{
	struct rb_node **link = &device_pages.rb_node, *parent = NULL;
	struct device_page *entry;
	struct page *page;

	spin_lock(&device_pages_lock);
	page = device_page_find(pfn);
	spin_unlock(&device_pages_lock);
	if (page != NULL)
		return page;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL)
		return NULL;
	entry->pfn = pfn;
	page_init(&entry->page, pfn, NULL, &entry->page);
	spin_lock(&device_pages_lock);
	page = device_page_find(pfn);
	if (page == NULL) {
		while (*link != NULL) {
			struct device_page *other
				= rb_entry(*link, struct device_page, node);
			parent = *link;
			link = pfn < other->pfn ? &(*link)->rb_left : &(*link)->rb_right;
		}
		rb_link_node(&entry->node, parent, link);
		rb_insert_color(&entry->node, &device_pages);
		page = &entry->page;
		entry = NULL;
	}
	spin_unlock(&device_pages_lock);
	kfree(entry);
	return page;
}


struct page *
lx_pfn_to_page(unsigned long pfn)
{
	struct page **slot, *page, *other;

	if (page_directory_get() != NULL && pfn >= page_frames)
		return device_page(pfn);
	slot = page_slot(pfn, true);
	if (slot == NULL)
		return NULL;
	page = READ_ONCE(*slot);
	if (page != NULL)
		return page;

	/* a page we did not allocate: describe it from now on */
	page = kzalloc(sizeof(*page), GFP_KERNEL);
	if (page == NULL)
		return NULL;
	page_init(page, pfn, NULL, page);
	other = cmpxchg(slot, NULL, page);
	if (other != NULL) {
		kfree(page);
		return other;
	}
	return page;
}


phys_addr_t
lx_virt_to_phys(const volatile void *address)
{
	return lx_haiku_virt_to_phys((const void *)address);
}


unsigned long
__get_free_pages(gfp_t gfp, unsigned int order)
{
	struct page *page = lx_alloc_pages(gfp, order);
	return page != NULL ? (unsigned long)page_address(page) : 0;
}


void
free_pages(unsigned long address, unsigned int order)
{
	if (address != 0)
		lx_free_pages(virt_to_page((void *)address), order);
}


void *
alloc_pages_exact(size_t size, gfp_t gfp)
{
	return (void *)__get_free_pages(gfp, get_order(size));
}


void
free_pages_exact(void *virt, size_t size)
{
	free_pages((unsigned long)virt, get_order(size));
}


/* see set_pages_wc() */
void
lx_flush_virt_range(const void *address, unsigned long size)
{
	const char *line = (const char *)((unsigned long)address & ~63UL);
	const char *end = (const char *)address + size;
	for (; line < end; line += 64)
		__asm__ __volatile__("clflush %0" : "+m" (*(volatile char *)line));
	__asm__ __volatile__("mfence" ::: "memory");
}


void
lx_flush_page_range(struct page *page, unsigned long count)
{
	unsigned long i;
	for (i = 0; i < count; i++)
		lx_flush_virt_range(page_address(page + i), PAGE_SIZE);
}


void
lx_flush_page_array(struct page **pages, unsigned long count)
{
	unsigned long i;
	for (i = 0; i < count; i++)
		lx_flush_virt_range(page_address(pages[i]), PAGE_SIZE);
}


void *
vmap(struct page **pages, unsigned int count, unsigned long flags,
	pgprot_t prot)
{
	unsigned long long *physical;
	void *address;
	unsigned int i;
	int type = LX_HAIKU_MAP_WB;

	if ((pgprot_val(prot) & _PAGE_CACHE_MASK) == 0x18)
		type = LX_HAIKU_MAP_UC;
	else if (pgprot_val(prot) & 0x8)
		type = LX_HAIKU_MAP_WC;

	physical = kmalloc_array(count, sizeof(*physical), GFP_KERNEL);
	if (physical == NULL)
		return NULL;
	for (i = 0; i < count; i++)
		physical[i] = page_to_phys(pages[i]);
	address = lx_haiku_vmap(physical, count, type);
	kfree(physical);
	return address;
}


void
vunmap(const void *address)
{
	if (address != NULL)
		lx_haiku_vunmap((void *)address);
}


unsigned long
totalram_pages(void)
{
	return lx_haiku_total_pages();
}


void
si_meminfo(struct sysinfo *info)
{
	memset(info, 0, sizeof(*info));
	info->totalram = lx_haiku_total_pages();
	info->freeram = lx_haiku_free_pages();
	info->mem_unit = PAGE_SIZE;
}


struct cpuinfo_x86 boot_cpu_data;


static void
lx_cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid" : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
		: "a" (leaf), "c" (0));
}


/*	boot_cpu_data from CPUID: amdgpu decides by the vendor whether the host
	takes part in PCIe link speed switching, and with a zeroed one (Intel)
	it gets the platform's link capabilities wrong. */
void lx_cpu_init(void);

void
lx_cpu_init(void)
{
	u32 a, b, c, d, family, model;
	lx_cpuid(0, &a, &b, &c, &d);
	if (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163)
		boot_cpu_data.x86_vendor = X86_VENDOR_AMD;		/* AuthenticAMD */
	else if (b == 0x6f677948 && d == 0x6e65476e && c == 0x656e6975)
		boot_cpu_data.x86_vendor = X86_VENDOR_HYGON;	/* HygonGenuine */
	else
		boot_cpu_data.x86_vendor = X86_VENDOR_INTEL;
	lx_cpuid(1, &a, &b, &c, &d);
	family = (a >> 8) & 0xf;
	model = (a >> 4) & 0xf;
	if (family == 0xf)
		family += (a >> 20) & 0xff;
	if (family >= 6)
		model |= ((a >> 16) & 0xf) << 4;
	boot_cpu_data.x86 = family;
	boot_cpu_data.x86_model = model;
	boot_cpu_data.x86_stepping = a & 0xf;
	boot_cpu_data.x86_clflush_size = ((b >> 8) & 0xff) * 8;
	boot_cpu_data.x86_cache_alignment = boot_cpu_data.x86_clflush_size;
}

bool
lx_cpu_has(int feature)
{
	return lx_haiku_cpu_has(feature);
}


int
lx_wbinvd_on_all_cpus(void)
{
	lx_haiku_wbinvd_all();
	return 0;
}


void
clflush_cache_range(void *address, unsigned int size)
{
	lx_haiku_clflush_range(address, size);
}


/* #pragma mark - DMA */


void *
dma_alloc_attrs(struct device *dev, size_t size, dma_addr_t *dma_handle,
	gfp_t flag, unsigned long attrs)
{
	struct page *page = lx_alloc_pages(flag | __GFP_ZERO, get_order(size));
	if (page == NULL)
		return NULL;
	*dma_handle = page_to_phys(page);
	return page_address(page);
}


void
dma_free_attrs(struct device *dev, size_t size, void *cpu_addr,
	dma_addr_t dma_handle, unsigned long attrs)
{
	if (cpu_addr != NULL)
		lx_free_pages(virt_to_page(cpu_addr), get_order(size));
}


int
dma_map_sg_attrs(struct device *dev, struct scatterlist *sgl, int nents,
	enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;
	for_each_sg(sgl, sg, nents, i) {
		sg->dma_address = sg_phys(sg);
		sg_dma_len(sg) = sg->length;
	}
	return nents;
}


int
dma_map_sgtable(struct device *dev, struct sg_table *sgt,
	enum dma_data_direction dir, unsigned long attrs)
{
	int nents = dma_map_sg_attrs(dev, sgt->sgl, sgt->orig_nents, dir, attrs);
	sgt->nents = nents;
	return 0;
}


int
dma_set_mask(struct device *dev, u64 mask)
{
	dev->lx_dma_mask = mask;
	dev->dma_mask = &dev->lx_dma_mask;
	return 0;
}


int
dma_set_coherent_mask(struct device *dev, u64 mask)
{
	dev->coherent_dma_mask = mask;
	return 0;
}


u64
dma_get_mask(struct device *dev)
{
	return dev->dma_mask != NULL ? *dev->dma_mask : DMA_BIT_MASK(32);
}


/* #pragma mark - ioremap */


void __iomem *
lx_ioremap(phys_addr_t offset, size_t size, int type)
{
	return (void __iomem *)lx_haiku_map_physical(offset, size, type);
}


void
lx_iounmap(volatile void __iomem *address)
{
	if (address != NULL)
		lx_haiku_unmap_physical((void __force *)address);
}


struct resource iomem_resource = {
	.name = "PCI mem",
	.start = 0,
	.end = -1,
	.flags = IORESOURCE_MEM,
};


/* #pragma mark - teams' memory */


unsigned long
lx_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	if (lx_haiku_user_memcpy(to, (const void __force *)from, n) != 0) {
		memset(to, 0, n);
		return n;
	}
	return 0;
}


unsigned long
lx_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	return lx_haiku_user_memcpy((void __force *)to, from, n) != 0 ? n : 0;
}


unsigned long
lx_clear_user(void __user *to, unsigned long n)
{
	static const char zeros[256];
	while (n > 0) {
		unsigned long chunk = min(n, sizeof(zeros));
		if (lx_haiku_user_memcpy((void __force *)to, zeros, chunk) != 0)
			return n;
		to = (char __user *)to + chunk;
		n -= chunk;
	}
	return 0;
}


long
strncpy_from_user(char *dst, const char __user *src, long count)
{
	long length = lx_haiku_user_strlcpy(dst, (const char __force *)src, count);
	if (length < 0)
		return -EFAULT;
	return min(length, count);
}


long
strnlen_user(const char __user *str, long n)
{
	char buffer[256];
	long length;
	if (n > (long)sizeof(buffer))
		n = sizeof(buffer);
	length = lx_haiku_user_strlcpy(buffer, (const char __force *)str, n);
	if (length < 0)
		return 0;
	return length + 1;
}


/* #pragma mark - things graphics memory does without */


struct file *
shmem_file_setup(const char *name, loff_t size, unsigned long flags)
{
	return ERR_PTR(-ENOSYS);
}


struct shrinker *
shrinker_alloc(unsigned int flags, const char *fmt, ...)
{
	return kzalloc(sizeof(struct shrinker), GFP_KERNEL);
}


void
shrinker_register(struct shrinker *shrinker)
{
}


void
shrinker_free(struct shrinker *shrinker)
{
	kfree(shrinker);
}
