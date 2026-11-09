/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef AMDXDNA_FAKEFW_H
#define AMDXDNA_FAKEFW_H

/*	The stand-in for the NPU in a virtual machine (fakefw.c). */

struct pci_dev;

/* the PCI function dressed up as the NPU, with its other half running */
struct pci_dev *lx_fakefw_start(void);
void lx_fakefw_stop(void);
/* what it has done so far, for the line of counters */
int lx_fakefw_stats(char *buffer, int size);

#endif /* AMDXDNA_FAKEFW_H */
