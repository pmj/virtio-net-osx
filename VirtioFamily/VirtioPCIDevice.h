//
//  VirtioPCIDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#pragma once

#include "VirtioDevice.h"

#define VirtioPCIDevice eu_dennis__jordan_driver_VirtioPCIDevice

struct virtio_pci_cap;
class IOPCIDevice;
class IOInterruptEventSource;
class IOFilterInterruptEventSource;


struct djt_msi_interrupt_handlers
{
	unsigned num_sources;
	int base_index;
	IOFilterInterruptEventSource** sources;
	IOWorkLoop** workloops;
};

class VirtioPCIDevice : public IOService
{
	OSDeclareDefaultStructors(VirtioPCIDevice);
public:
	virtual IOService* probe(IOService* provider, SInt32* score) override;
	virtual bool start(IOService* provider) override;
	virtual void stop(IOService* provider) override;

protected:

	bool setupCommonCFG(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup);
	bool setupNotificationStructure(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup);
	bool setupISRStatusStructure(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup);
	bool setupDeviceSpecificStructure(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup);

	bool setupInterruptHandlers(IOPCIDevice* pci_device);
	void shutdownInterruptHandlers();
	
	djt_msi_interrupt_handlers msi_handlers;
	IOFilterInterruptEventSource* irq_source;
	IOWorkLoop* irq_workloop;
	static void interruptAction(OSObject* me, IOInterruptEventSource* source, int count);
	static bool interruptFilter(OSObject* me, IOFilterInterruptEventSource* source);
	//virtual void interruptAction(IOInterruptEventSource* source, int count);
};

