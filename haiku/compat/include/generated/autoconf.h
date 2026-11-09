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


/* The AMD NPU add-on (make amdxdna, haiku/amdxdna): compute accelerator
 * nodes in the DRM core and the driver. Not in amdgpu_drm. */
#ifdef LX_ADDON_AMDXDNA
#define CONFIG_DRM_ACCEL 1
#define CONFIG_DRM_ACCEL_AMDXDNA 1
#define CONFIG_DRM_GEM_SHMEM_HELPER 1
#endif
