/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	PCI configuration space, MSI and interrupt handlers of lx/haiku.h. */

#include <new>
#include <string.h>

#include <KernelExport.h>
#include <PCI.h>

#include "../include/lx/haiku.h"


static const int kLinuxENODEV = 19;
static const int kLinuxEIO = 5;

static pci_module_info* sPCI = NULL;


static pci_module_info*
pci_module()
{
	if (sPCI == NULL) {
		if (get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI) != B_OK)
			sPCI = NULL;
	}
	return sPCI;
}


extern "C" int
lx_haiku_pci_get_nth(int index, struct lx_haiku_pci_info* info)
{
	pci_module_info* pci = pci_module();
	pci_info haiku;
	if (pci == NULL || pci->get_nth_pci_info(index, &haiku) != B_OK)
		return -kLinuxENODEV;

	memset(info, 0, sizeof(*info));
	info->bus = haiku.bus;
	info->device = haiku.device;
	info->function = haiku.function;
	info->vendor_id = haiku.vendor_id;
	info->device_id = haiku.device_id;
	info->class_base = haiku.class_base;
	info->class_sub = haiku.class_sub;
	info->class_api = haiku.class_api;
	info->revision = haiku.revision;
	info->header_type = haiku.header_type;

	if ((haiku.header_type & PCI_header_type_mask) == PCI_header_type_generic) {
		info->subsystem_vendor_id = haiku.u.h0.subsystem_vendor_id;
		info->subsystem_id = haiku.u.h0.subsystem_id;
		info->interrupt_line = haiku.u.h0.interrupt_line;
		for (int bar = 0; bar < 6; bar++) {
			uint32 flags = haiku.u.h0.base_register_flags[bar];
			info->bar_address[bar] = haiku.u.h0.base_registers[bar];
			info->bar_size[bar] = haiku.u.h0.base_register_sizes[bar];
			if ((flags & PCI_address_space) != 0)
				info->bar_flags[bar] |= 1;
			if ((flags & PCI_address_prefetchable) != 0)
				info->bar_flags[bar] |= 4;
			if ((flags & PCI_address_space) == 0
				&& (flags & PCI_address_type) == PCI_address_type_64
				&& bar < 5) {
				info->bar_flags[bar] |= 2;
				info->bar_address[bar]
					|= (uint64)haiku.u.h0.base_registers[bar + 1] << 32;
				info->bar_size[bar]
					|= (uint64)haiku.u.h0.base_register_sizes[bar + 1] << 32;
				bar++;
			}
		}
	}
	return 0;
}


extern "C" unsigned int
lx_haiku_pci_read(int bus, int device, int function, int offset, int size)
{
	pci_module_info* pci = pci_module();
	if (pci == NULL)
		return ~0u;
	return pci->read_pci_config(bus, device, function, offset, size);
}


extern "C" void
lx_haiku_pci_write(int bus, int device, int function, int offset, int size,
	unsigned int value)
{
	pci_module_info* pci = pci_module();
	if (pci != NULL)
		pci->write_pci_config(bus, device, function, offset, size, value);
}


extern "C" int
lx_haiku_pci_enable_msi(int bus, int device, int function)
{
	pci_module_info* pci = pci_module();
	uint32 vector;
	if (pci == NULL || pci->get_msi_count(bus, device, function) < 1)
		return -kLinuxENODEV;
	if (pci->configure_msi(bus, device, function, 1, &vector) != B_OK)
		return -kLinuxEIO;
	if (pci->enable_msi(bus, device, function) != B_OK) {
		pci->unconfigure_msi(bus, device, function);
		return -kLinuxEIO;
	}
	return vector;
}


extern "C" void
lx_haiku_pci_disable_msi(int bus, int device, int function)
{
	pci_module_info* pci = pci_module();
	if (pci == NULL)
		return;
	pci->disable_msi(bus, device, function);
	pci->unconfigure_msi(bus, device, function);
}


extern "C" int
lx_haiku_pci_msix_count(int bus, int device, int function)
{
	pci_module_info* pci = pci_module();
	if (pci == NULL)
		return 0;
	return pci->get_msix_count(bus, device, function);
}


extern "C" int
lx_haiku_pci_enable_msix(int bus, int device, int function, int count)
{
	pci_module_info* pci = pci_module();
	uint32 vector;
	if (pci == NULL || count < 1
		|| pci->get_msix_count(bus, device, function) < (uint32)count)
		return -kLinuxENODEV;
	if (pci->configure_msix(bus, device, function, count, &vector) != B_OK)
		return -kLinuxEIO;
	if (pci->enable_msix(bus, device, function) != B_OK) {
		// unconfigures MSI-X when that is what is configured
		pci->unconfigure_msi(bus, device, function);
		return -kLinuxEIO;
	}
	return vector;
}


struct InterruptHook {
	uint32	vector;
	void	(*function)(void*);
	void*	data;
};


static int32
interrupt_trampoline(void* _hook)
{
	InterruptHook* hook = (InterruptHook*)_hook;
	hook->function(hook->data);
	return B_INVOKE_SCHEDULER;
}


extern "C" void*
lx_haiku_install_interrupt(unsigned int vector, void (*function)(void*),
	void* data)
{
	InterruptHook* hook = new(std::nothrow) InterruptHook;
	if (hook == NULL)
		return NULL;
	hook->vector = vector;
	hook->function = function;
	hook->data = data;
	if (install_io_interrupt_handler(vector, interrupt_trampoline, hook, 0)
			!= B_OK) {
		delete hook;
		return NULL;
	}
	return hook;
}


extern "C" void
lx_haiku_remove_interrupt(void* _hook)
{
	InterruptHook* hook = (InterruptHook*)_hook;
	if (hook == NULL)
		return;
	remove_io_interrupt_handler(hook->vector, interrupt_trampoline, hook);
	delete hook;
}
