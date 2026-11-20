/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef I915_STUBS_H
#define I915_STUBS_H

struct pci_dev;

/* before probing a GPU: the TSC frequency, the stolen memory */
void lx_i915_platform_init(struct pci_dev *pdev);

#endif /* I915_STUBS_H */
