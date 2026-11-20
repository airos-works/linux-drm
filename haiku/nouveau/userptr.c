/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	amdgpu's userptr buffers (DRM_AMDGPU_GEM_USERPTR, behind RADV's
	VK_EXT_external_memory_host) on Haiku. Linux follows the user pages with
	an MMU notifier and lets them move; here they are wired when the buffer
	is made (lock_memory_etc(), as nvidia_rm does) and stay wired until TTM
	destroys the buffer's pages, which it does once the GPU is done with them.
	Haiku moves no wired page, so the pages amdgpu_ttm_tt_get_user_pages()
	returns never change.

	A team's areas must not be wired when its address space goes, but the
	buffer's DRM file is only closed after that (team_delete_team() removes
	the address space, ~Team closes the files). So each buffer watches its
	team: when the team dies, the hook waits for the GPU work that uses the
	buffer and unwires the pages, still before the address space goes. From
	then on the buffer cannot be used (-ESRCH). A team that exec()s with such
	a buffer is not covered: exec_team() deletes the areas before it closes
	any file.

	Unlike on Linux, the memory has to stay mapped until the buffer is
	freed: delete_area() or munmap() of it waits until then, and can
	neither be interrupted nor killed, so a program that frees the memory
	before the buffer (Vulkan does not allow it either) hangs.

	The pages get a struct page each, kept here (not in the frame table:
	they are not the driver's); a range's "pfns" are pointers to them
	(hmm_pfn_to_page()), in one array every range shares, since they never
	change: each command submission with the buffer asks for them. */

#include <lx/haiku.h>
#include <linux/dma-resv.h>
#include <linux/hmm.h>
#include <linux/overflow.h>
#include <drm/drm_gem.h>

#include "userptr.h"


struct lx_userptr {
	struct kref ref;		/* the buffer's ttm_tt's and the team watcher's */
	struct mutex lock;
	wait_queue_head_t released;
	struct drm_gem_object *gobj;	/* until the GEM object is freed */
	lx_thread_id team;
	unsigned long address;
	unsigned long count;
	bool writable;
	bool wired;
	bool team_gone;
	unsigned long *pfns;	/* the ranges' */
	struct page pages[];
};


static void
userptr_free(struct kref *ref)
{
	struct lx_userptr *userptr = container_of(ref, struct lx_userptr, ref);
	WARN_ON(userptr->wired);
	mutex_destroy(&userptr->lock);
	kvfree(userptr->pfns);
	kvfree(userptr);
}


static void
userptr_unwire(struct lx_userptr *userptr)
{
	mutex_lock(&userptr->lock);
	if (userptr->wired) {
		lx_haiku_user_pages_unlock(userptr->team, userptr->address,
			userptr->count, userptr->writable);
		userptr->wired = false;
	}
	mutex_unlock(&userptr->lock);
	wake_up_all(&userptr->released);
}


/*	The team is being deleted, with its lock held; its address space is still
	there. GPU work submitted before may still use the pages. */
static void
userptr_team_gone(lx_thread_id team, void *data)
{
	struct lx_userptr *userptr = data;
	struct drm_gem_object *gobj = NULL;

	mutex_lock(&userptr->lock);
	userptr->team_gone = true;
	if (userptr->gobj != NULL && kref_get_unless_zero(&userptr->gobj->refcount))
		gobj = userptr->gobj;
	mutex_unlock(&userptr->lock);

	if (gobj != NULL) {
		while (dma_resv_wait_timeout(gobj->resv, DMA_RESV_USAGE_BOOKKEEP,
				false, 10 * HZ) == 0) {
			pr_warn("nouveau: team %d is gone, the GPU still uses its memory "
				"at %#lx\n", team, userptr->address);
		}
		userptr_unwire(userptr);
		drm_gem_object_put(gobj);
	} else {
		/* the buffer is being freed: TTM destroys its pages (and unwires
		   them) once the GPU is done */
		while (!wait_event_timeout(userptr->released,
				!READ_ONCE(userptr->wired), 10 * HZ)) {
			pr_warn("nouveau: team %d is gone, waiting for the GPU to finish "
				"with its memory at %#lx\n", team, userptr->address);
		}
	}
	kref_put(&userptr->ref, userptr_free);
}


/*	Wires \a count pages of the calling team at \a address for \a gobj and
	watches the team. */
int
lx_userptr_create(struct drm_gem_object *gobj, unsigned long address,
	unsigned long count, bool writable, bool anon_only,
	struct lx_userptr **_userptr)
{
	struct lx_userptr *userptr;
	unsigned long long *physical;
	unsigned long i;
	int error;

	userptr = kvzalloc(struct_size(userptr, pages, count), GFP_KERNEL);
	physical = kvmalloc_array(count, sizeof(*physical), GFP_KERNEL);
	if (userptr != NULL)
		userptr->pfns = kvmalloc_array(count, sizeof(*userptr->pfns),
			GFP_KERNEL);
	if (userptr == NULL || userptr->pfns == NULL || physical == NULL) {
		if (userptr != NULL)
			kvfree(userptr->pfns);
		kvfree(userptr);
		kvfree(physical);
		return -ENOMEM;
	}
	kref_init(&userptr->ref);
	mutex_init(&userptr->lock);
	init_waitqueue_head(&userptr->released);
	userptr->gobj = gobj;
	userptr->address = address;
	userptr->count = count;
	userptr->writable = writable;

	error = lx_haiku_user_pages_lock(address, count, writable, anon_only,
		&userptr->team, physical);
	if (error == 0) {
		userptr->wired = true;
		for (i = 0; i < count; i++) {
			struct page *page = &userptr->pages[i];
			page->lx_phys = physical[i];
			page->lx_head = page;
			atomic_set(&page->_refcount, 1);
			INIT_LIST_HEAD(&page->lru);
			userptr->pfns[i] = (unsigned long)page;
		}
		kref_get(&userptr->ref);
		error = lx_haiku_team_watch(userptr->team, userptr_team_gone, userptr);
		if (error != 0) {
			kref_put(&userptr->ref, userptr_free);
			userptr_unwire(userptr);
		}
	}
	kvfree(physical);
	if (error != 0) {
		kref_put(&userptr->ref, userptr_free);
		return error;
	}
	*_userptr = userptr;
	return 0;
}


/* the GEM object is being freed (its ttm_tt may live on for a while) */
void
lx_userptr_detach(struct lx_userptr *userptr)
{
	mutex_lock(&userptr->lock);
	userptr->gobj = NULL;
	mutex_unlock(&userptr->lock);
}


/* TTM destroys the buffer's pages: the GPU no longer uses them */
void
lx_userptr_release(struct lx_userptr *userptr)
{
	userptr_unwire(userptr);
	if (lx_haiku_team_unwatch(userptr->team, userptr_team_gone, userptr) == 0)
		kref_put(&userptr->ref, userptr_free);
	kref_put(&userptr->ref, userptr_free);
}


/*	The pages, for amdgpu_ttm_tt_get_user_pages(); the range is freed by
	lx_userptr_get_pages_done(). */
int
lx_userptr_get_pages(struct lx_userptr *userptr, struct hmm_range **_range)
{
	struct hmm_range *range;

	if (READ_ONCE(userptr->team_gone))
		return -ESRCH;
	range = kzalloc(sizeof(*range), GFP_KERNEL);
	if (range == NULL)
		return -ENOMEM;
	range->hmm_pfns = userptr->pfns;
	range->start = userptr->address;
	range->end = userptr->address + (userptr->count << PAGE_SHIFT);
	range->dev_private_owner = userptr;
	*_range = range;
	return 0;
}


/* frees the range; false if the pages were let go since (the team died) */
bool
lx_userptr_get_pages_done(struct hmm_range *range)
{
	struct lx_userptr *userptr = range->dev_private_owner;
	bool valid = !READ_ONCE(userptr->team_gone);

	kfree(range);
	return valid;
}


void
lx_userptr_set_object(struct lx_userptr *userptr, struct drm_gem_object *gobj)
{
	mutex_lock(&userptr->lock);
	userptr->gobj = gobj;
	mutex_unlock(&userptr->lock);
}
