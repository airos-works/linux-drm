/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_GGTT_GMCH_H__
#define __INTEL_GGTT_GMCH_H__

#include "intel_gtt.h"

/* For x86 platforms */
/* Haiku: no AGP/intel-gtt (GPUs before Sandy Bridge), so the stubs */
#if IS_ENABLED(CONFIG_X86) && !defined(__HAIKU_LINUX_COMPAT__)

void intel_ggtt_gmch_flush(void);
int intel_ggtt_gmch_enable_hw(struct drm_i915_private *i915);
int intel_ggtt_gmch_probe(struct i915_ggtt *ggtt);

/* Stubs for non-x86 platforms */
#else

static inline void intel_ggtt_gmch_flush(void) { }
static inline int intel_ggtt_gmch_enable_hw(struct drm_i915_private *i915) { return -ENODEV; }
static inline int intel_ggtt_gmch_probe(struct i915_ggtt *ggtt) { return -ENODEV; }

#endif

#endif /* __INTEL_GGTT_GMCH_H__ */
