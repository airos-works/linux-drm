/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	What stands in for the i915 files the add-on does not build (see
	tools/i915-sources.py): parts for hardware or Linux subsystems that
	Haiku and the GPUs it drives with i915 do without. */

#include "i915_drv.h"
#include "gem/i915_gemfs.h"
#include "gt/intel_gsc.h"

#include "stubs.h"
#include <drm/intel/pciids.h>


/* the TSC's frequency (intel_llc's ring/CPU frequency table falls back to
 * it): CPUID leaf 0x16's base frequency, as on Skylake and later */
unsigned int tsc_khz;

/* the stolen memory (the BIOS's reserve for the GPU), which Linux finds in
 * its early PCI quirks: from the GPU's config space on Haiku */
struct resource intel_graphics_stolen_res = DEFINE_RES_MEM(0, 0);


/* integrated GPUs whose stolen memory base is the 64-bit BDSM at 0xc0
 * (Gen11 and later), as Linux's early quirks list them */
#define LX_ID(id, ...)	id
static const u16 kBdsm64Ids[] = {
	INTEL_ICL_IDS(LX_ID), INTEL_EHL_IDS(LX_ID), INTEL_JSL_IDS(LX_ID),
	INTEL_TGL_IDS(LX_ID), INTEL_RKL_IDS(LX_ID), INTEL_ADLS_IDS(LX_ID),
	INTEL_ADLP_IDS(LX_ID), INTEL_ADLN_IDS(LX_ID), INTEL_RPLS_IDS(LX_ID),
	INTEL_RPLU_IDS(LX_ID), INTEL_RPLP_IDS(LX_ID), INTEL_MTL_IDS(LX_ID),
};


static void
cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
	__asm__ volatile("cpuid" : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
		: "a" (leaf), "c" (0));
}


void
lx_i915_platform_init(struct pci_dev *pdev)
{
	u32 a, b, c, d;
	u16 ggc;
	u32 bdsm;
	u64 size = 0;
	unsigned int gms;

	cpuid(0, &a, &b, &c, &d);
	if (a >= 0x16) {
		cpuid(0x16, &a, &b, &c, &d);
		tsc_khz = (a & 0xffff) * 1000;
	}

	if (resource_size(&intel_graphics_stolen_res) != 0
		|| pdev->vendor != PCI_VENDOR_ID_INTEL
		|| pdev->bus->number != 0 || pdev->devfn != PCI_DEVFN(2, 0))
		return;

	/* Gen6 to Gen10 (Sandy Bridge to Coffee Lake): GGC's GMS field and
	 * BDSM; Gen11 and later: a 64-bit BDSM at 0xc0 */
	pci_read_config_word(pdev, 0x50, &ggc);
	gms = (ggc >> 8) & 0xff;
	if (gms < 0xf0)
		size = (u64)gms * SZ_32M;
	else
		size = (u64)(gms - 0xf0 + 1) * SZ_4M;
	bool bdsm64 = false;
	for (unsigned int i = 0; i < ARRAY_SIZE(kBdsm64Ids); i++)
		bdsm64 |= kBdsm64Ids[i] == pdev->device;
	if (bdsm64) {
		u32 low, high;
		pci_read_config_dword(pdev, 0xc0, &low);
		pci_read_config_dword(pdev, 0xc4, &high);
		bdsm = 0;
		intel_graphics_stolen_res = (struct resource)DEFINE_RES_MEM(
			(((u64)high << 32) | low) & ~(u64)(SZ_1M - 1), size);
	} else {
		pci_read_config_dword(pdev, 0x5c, &bdsm);
		intel_graphics_stolen_res = (struct resource)DEFINE_RES_MEM(
			bdsm & 0xfff00000, size);
	}
	if (size == 0)
		intel_graphics_stolen_res = (struct resource)DEFINE_RES_MEM(0, 0);
	pr_info("i915: stolen memory %pR\n", &intel_graphics_stolen_res);
}


unsigned long
int_sqrt(unsigned long x)
{
	unsigned long b, m, y = 0;

	if (x <= 1)
		return x;
	m = 1UL << (__fls(x) & ~1UL);
	while (m != 0) {
		b = y + m;
		y >>= 1;
		if (x >= b) {
			x -= b;
			y += m;
		}
		m >>= 2;
	}
	return y;
}


/* no private tmpfs mount: shmem objects come from the compat layer's */
void
i915_gemfs_init(struct drm_i915_private *i915)
{
}


void
i915_gemfs_fini(struct drm_i915_private *i915)
{
}


/* the GSC (graphics security controller): discrete and Meteor Lake GPUs */
void
intel_gsc_init(struct intel_gsc *gsc, struct drm_i915_private *i915)
{
}


void
intel_gsc_fini(struct intel_gsc *gsc)
{
}


void
intel_gsc_irq_handler(struct intel_gt *gt, u32 iir)
{
}
