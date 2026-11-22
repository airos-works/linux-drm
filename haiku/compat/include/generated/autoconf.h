/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
/*	The kernel configuration the Linux code is built for, in Kconfig's
	autoconf.h form. Anything not listed is off. Options that would need Linux
	subsystems Haiku has no equivalent for (debugfs, sysfs, tracing, the ACPI
	glue, KFD compute, user pointers via HMM, vga_switcheroo, fbdev) stay off;
	the drivers have stubs for all of them. */

/* not Kconfig: the firmware tables of the ACPI glue (amdgpu: the VBIOS image
   in VFCT, which APUs and most laptops have no other copy of) */
#define CONFIG_HAIKU_ACPI_TABLES 1

#define CONFIG_X86 1
#define CONFIG_X86_64 1
#define CONFIG_64BIT 1
#define CONFIG_MMU 1
#define CONFIG_SMP 1
#define CONFIG_NR_CPUS 64
#define CONFIG_PCI 1
#define CONFIG_PCI_MSI 1
#define CONFIG_HZ 1000
#define CONFIG_PAGE_SHIFT 12
#define CONFIG_PRINTK 1
#define CONFIG_CC_IS_GCC 1
#define CONFIG_GCC_VERSION 130300
#define CONFIG_ARCH_DMA_ADDR_T_64BIT 1
#define CONFIG_PHYS_ADDR_T_64BIT 1
#define CONFIG_ARCH_HAS_SET_MEMORY 1
#define CONFIG_HAS_IOMEM 1
#define CONFIG_HAS_DMA 1
#define CONFIG_BASE_SMALL 0
#define CONFIG_I2C 1
#define CONFIG_I2C_ALGOBIT 1

#define CONFIG_DRM 1
#define CONFIG_DRM_KMS_HELPER 1
#define CONFIG_DRM_DISPLAY_HELPER 1
#define CONFIG_DRM_DISPLAY_DP_HELPER 1
#define CONFIG_DRM_DISPLAY_DSC_HELPER 1
#define CONFIG_DRM_DISPLAY_HDCP_HELPER 1
#define CONFIG_DRM_DISPLAY_HDMI_HELPER 1
#define CONFIG_DRM_TTM 1
#define CONFIG_DRM_TTM_HELPER 1
#define CONFIG_DRM_EXEC 1
#define CONFIG_DRM_BUDDY 1
#define CONFIG_DRM_SUBALLOC_HELPER 1
#define CONFIG_DRM_SCHED 1
#define CONFIG_DMA_SHARED_BUFFER 1
#define CONFIG_SYNC_FILE 1

#define CONFIG_DRM_AMDGPU 1
#define CONFIG_DRM_AMDGPU_SI 1
#define CONFIG_DRM_AMDGPU_CIK 1
/* Haiku's radeon_hd is the radeon driver here: as on Linux with radeon
 * built, Southern and Sea Islands GPUs are left to it unless amdgpu_drm's
 * settings say "si_support 1" or "cik_support 1" (only amdgpu_drv.c looks) */
#define CONFIG_DRM_RADEON_MODULE 1
#define CONFIG_DRM_AMD_DC 1
#define CONFIG_DRM_AMD_DC_FP 1
#define CONFIG_DRM_AMD_DC_SI 1


/* The NVIDIA add-on (make nouveau, haiku/nouveau): Linux's nouveau with its
 * Kconfig defaults, the GPU VA manager for its VM_BIND interface. */
#ifdef LX_ADDON_NOUVEAU
#define CONFIG_DRM_NOUVEAU 1
#define CONFIG_DRM_GPUVM 1
#define CONFIG_NOUVEAU_DEBUG 6
#define CONFIG_NOUVEAU_DEBUG_DEFAULT 3
#endif

/* The AMD NPU add-on (make amdxdna, haiku/amdxdna): compute accelerator
 * nodes in the DRM core and the driver. Not in amdgpu_drm. */
#ifdef LX_ADDON_AMDXDNA
#define CONFIG_DRM_ACCEL 1
#define CONFIG_DRM_ACCEL_AMDXDNA 1
#define CONFIG_DRM_GEM_SHMEM_HELPER 1
#endif

/* The Intel add-on (make i915, haiku/i915): Linux's i915 with its Kconfig
 * defaults, without PXP, GVT or error capture. CONFIG_ACPI only for it:
 * the OpRegion holds the VBT (the board's outputs and panel), the rest of
 * ACPI is stubbed. CONFIG_MMU_NOTIFIER only for it too, and only for its
 * user pointer objects (i915_gem_userptr.c), which wire their pages here
 * instead of following them with a notifier. */
#ifdef LX_ADDON_I915
#define CONFIG_DRM_I915 1
#define CONFIG_MMU_NOTIFIER 1
#define CONFIG_DRM_I915_FORCE_PROBE ""
#define CONFIG_DRM_I915_REQUEST_TIMEOUT 20000
#define CONFIG_DRM_I915_FENCE_TIMEOUT 10000
#define CONFIG_DRM_I915_USERFAULT_AUTOSUSPEND 250
#define CONFIG_DRM_I915_HEARTBEAT_INTERVAL 2500
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT 640
#define CONFIG_DRM_I915_PREEMPT_TIMEOUT_COMPUTE 7500
#define CONFIG_DRM_I915_MAX_REQUEST_BUSYWAIT 8000
#define CONFIG_DRM_I915_STOP_TIMEOUT 100
#define CONFIG_DRM_I915_TIMESLICE_DURATION 1
/* the panel helpers of its DSI encoders */
#define CONFIG_DRM_PANEL 1
#define CONFIG_DRM_MIPI_DSI 1
#define CONFIG_ACPI 1
#endif
