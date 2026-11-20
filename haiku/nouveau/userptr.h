/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NOUVEAU_USERPTR_H
#define NOUVEAU_USERPTR_H

/* the wired pages of a userptr buffer (userptr.c, shared with amdgpu_drm) */
struct drm_gem_object;
struct hmm_range;
struct lx_userptr;
int lx_userptr_create(struct drm_gem_object *gobj, unsigned long address,
		      unsigned long count, bool writable, bool anon_only,
		      struct lx_userptr **userptr);
void lx_userptr_detach(struct lx_userptr *userptr);
void lx_userptr_release(struct lx_userptr *userptr);
int lx_userptr_get_pages(struct lx_userptr *userptr,
			 struct hmm_range **range);
bool lx_userptr_get_pages_done(struct hmm_range *range);

#endif
