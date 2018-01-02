//
//  VirtioPCIDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#include "VirtioPCIDevice.h"
#include "../lib/kextgizmos/iopcidevice_helpers.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOFilterInterruptEventSource.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#include <IOKit/pci/IOPCIDevice.h>
#pragma clang diagnostic pop

#define ANSI_ESCAPE_RESET "\x1b[0m"
#define ANSI_ESCAPE_DARKGREY "\x1b[90m"
#define ANSI_ESCAPE_RED "\x1b[31m"

#define LogWithLocation(fmt, ...) ({ kprintf( ANSI_ESCAPE_DARKGREY "%s:" ANSI_ESCAPE_RESET " " fmt , __PRETTY_FUNCTION__ , ## __VA_ARGS__); })
#define LogWarning(fmt, ...) LogWithLocation(ANSI_ESCAPE_RED "Warning: " ANSI_ESCAPE_RESET fmt, ## __VA_ARGS__)
#if DEBUG
#define LogVerbose(fmt, ...) LogWithLocation(fmt, ## __VA_ARGS__)
#endif


#define IOKIT_PCI_VENDOR_ID_KEY "vendor-id"
#define IOKIT_PCI_DEVICE_ID_KEY "device-id"

inline bool is_pow2(size_t val)
{
	return val != 0ul && 0ul == (val & (val - 1ul));
}

bool djt_ioregentry_read_uint32_from_data_property(uint32_t& out_value, IORegistryEntry* object, const char* property)
{
	OSObject* prop_obj = object->copyProperty(property);
	if (prop_obj == nullptr)
		return false;
	
	bool success = false;
	OSData* prop_data = OSDynamicCast(OSData, prop_obj);
	if (prop_data != nullptr && prop_data->getLength() == sizeof(out_value))
	{
		memcpy(&out_value, prop_data->getBytesNoCopy(), sizeof(out_value));
		success = true;
	}
	prop_obj->release(); // balance retain in copyProperty
	return success;
}

OSDefineMetaClassAndStructors(VirtioPCIDevice, IOService);

template<typename FN_T> bool djt_iopcidevice_iterate_capabilities(IOPCIDevice* dev, FN_T fn)
{
	uint8_t offset = djt_iopcidevice_first_capability_offset(dev);
	if (offset == 0)
		return false;
	
	bool success = true;
	uint8_t cap_type = dev->configRead8(offset);
	fn(offset, cap_type);
	
	uint8_t trail = offset;
	bool trail_skip = true;
	do
	{
		offset = djt_iopcidevice_next_capability_offset(dev, offset);
		if (offset < 0x40 || offset % 4 != 0 || trail == offset)
		{
			LogVerbose("end iteration: offset = %u (0x%x), trail = %u\n", offset, offset, trail);
			success = (offset == 0);
			break;
		}

		uint8_t cap_type = dev->configRead8(offset);
		fn(offset, cap_type);
		
		// Walk list at half speed to detect cycles
		if (!trail_skip)
			trail = djt_iopcidevice_next_capability_offset(dev, trail);
		trail_skip = !trail_skip;
	}
	while (success);
	return success;
}


namespace VirtioCapOffset
{
	enum virtio_pci_cap : uint8_t
	{
		CAP_VNDR = 0,
		CAP_NEXT,
		CAP_LEN,
		CFG_TYPE,
		BAR,
		OFFSET = 8,
		LENGTH = 12
	};
}

struct VirtioModernPCIVirtqueue
{
	IOBufferMemoryDescriptor* descriptor_table_mem;
	IODMACommand* descriptor_table_mem_dma;
	IOBufferMemoryDescriptor* available_ring_mem;
	IODMACommand* available_ring_mem_dma;
	IOBufferMemoryDescriptor* used_ring_mem;
	IODMACommand* used_ring_mem_dma;

	struct VirtioVirtqueue queue;
};

// Types used in Virtio spec
typedef uint64_t le64;
typedef uint32_t le32;
typedef uint16_t le16;
typedef uint8_t u8;

// BEGIN From 4.1.4 "Virtio Structure PCI Capabilities":

struct virtio_pci_cap
{
  uint8_t cap_vndr;
	uint8_t cap_next;
	uint8_t cap_len;
	uint8_t cfg_type;
	uint8_t bar;
	uint8_t padding[3];
	uint32_t bar_offset;
	uint32_t bar_length;
};

/* Common configuration */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
/* Notifications */
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
/* ISR Status */
#define VIRTIO_PCI_CAP_ISR_CFG 3
/* Device specific configuration */
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
/* PCI configuration access */
#define VIRTIO_PCI_CAP_PCI_CFG 5

// END

// BEGIN 4.1.4.3

struct virtio_pci_common_cfg
{
	/* About the whole device. */
	le32 device_feature_select; /* read-write */
	le32 device_feature; /* read-only for driver */
	le32 driver_feature_select; /* read-write */
	le32 driver_feature; /* read-write */
	le16 msix_config; /* read-write */
	le16 num_queues; /* read-only for driver */
	u8 device_status; /* read-write */
	u8 config_generation; /* read-only for driver */
	/* About a specific virtqueue. */
	le16 queue_select; /* read-write */
	le16 queue_size; /* read-write, power of 2, or 0. */
	le16 queue_msix_vector; /* read-write */
	le16 queue_enable; /* read-write */
	le16 queue_notify_off; /* read-only for driver */
	le64 queue_desc; /* read-write */
	le64 queue_avail; /* read-write */
	le64 queue_used; /* read-write */
};

static bool check_bar_type_and_length(IOPCIDevice* dev, uint8_t bar_index, uint32_t min_length)
{
	if (bar_index > 5)
	{
		LogVerbose("BAR number %u is not valid.\n", bar_index);
		return false;
	}

	uint8_t bar_reg = djt_iopcidevice_register_for_range_index(bar_index);
	int bar_type = djt_iopcidevice_memory_range_type(dev, bar_reg);
	
	bool ok = true;
	switch (bar_type)
	{
	case kIOPCI32BitMemorySpace:
	case kIOPCI64BitMemorySpace:
		{
			IODeviceMemory* bar_mem = dev->getDeviceMemoryWithRegister(bar_reg);
			if (bar_mem == nullptr || bar_mem->getLength() < min_length)
			{
				if (bar_mem == nullptr)
					LogVerbose("Could not get device memory for BAR number %u.\n", bar_index);
				else
					LogVerbose("Device memory for BAR %u is too short (%llu, expect at least %u).\n", bar_index, bar_mem->getLength(), min_length);
				ok = false;
			}
			break;
		}
	case kIOPCIIOSpace:
		LogVerbose("BAR %u for common configuration is an I/O range. Checking for second capability for MMIO common configuration, or falling back to legacy/traditional PCI driver.\n", bar_index);
			ok = false;
	case -1:
	default:
		LogVerbose("Error getting device memory range type for BAR %u. (result = %d)\n", bar_index, bar_type);
		ok = false;
	}
	return ok;
}

bool VirtioPCIDevice::setupCommonCFG(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup)
{
	LogVerbose("@ offset %u\n", config_offset);
	bool ok = true;
	if (cap.bar_offset % 4 != 0)
	{
		LogVerbose("BAR offset %u (0x%x) not 4-byte aligned\n", cap.bar_offset, cap.bar_offset);
		ok = false;
	}
	
	if (cap.bar_length < sizeof(virtio_pci_common_cfg))
	{
		LogVerbose("BAR range length for common config %u (0x%x) is smaller than sizeof virtio_pci_common_cfg %lu\n",
			cap.bar_length, cap.bar_length, sizeof(virtio_pci_common_cfg));
		ok = false;
	}
	
	if (!check_bar_type_and_length(dev, cap.bar, cap.bar_length))
		ok = false;
	
	if (ok && do_setup)
	{
#warning TODO
	}
	return ok;
}

bool VirtioPCIDevice::setupNotificationStructure(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup)
{
	bool ok = true;

	uint32_t notify_off_multiplier = dev->configRead32(config_offset + sizeof(virtio_pci_cap));
	LogVerbose("@ offset %u; notify_off_multiplier = %u\n", config_offset, notify_off_multiplier);
	
	if (notify_off_multiplier != 0 && (!is_pow2(notify_off_multiplier)))
	{
		LogVerbose("Notify offset multiplier %u should be 0 or an even power of 2.\n", notify_off_multiplier);
		ok = false;
	}
	
	if (cap.bar_offset % 2 != 0)
	{
		LogVerbose("BAR offset %u (0x%x) not 2-byte aligned\n", cap.bar_offset, cap.bar_offset);
		ok = false;
	}
	
	if (cap.bar_length < 2)
	{
		LogVerbose("BAR range length for notify struct %u (0x%x) is smaller than 2!\n",
			cap.bar_length, cap.bar_length);
		ok = false;
	}

	if (!check_bar_type_and_length(dev, cap.bar, cap.bar_length))
		ok = false;

	if (ok && do_setup)
	{
#warning TODO
	}
	return ok;
}

bool VirtioPCIDevice::setupISRStatusStructure(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup)
{
	LogVerbose("@ offset %u\n", config_offset);
	bool ok = true;

	if (cap.bar_length < 1)
	{
		LogVerbose("BAR range length for ISR range %u (0x%x) not valid\n",
			cap.bar_length, cap.bar_length);
		ok = false;
	}
	
	if (!check_bar_type_and_length(dev, cap.bar, cap.bar_length))
		ok = false;
	
	if (ok && do_setup)
	{
#warning TODO
	}
	return ok;
}

bool VirtioPCIDevice::setupDeviceSpecificStructure(IOPCIDevice* dev, const virtio_pci_cap& cap, unsigned config_offset, bool do_setup)
{
	LogVerbose("@ offset %u, BAR %u, length %u\n", config_offset, cap.bar, cap.bar_length);
	
	bool ok = true;
	if (cap.bar_offset % 4 != 0)
	{
		LogVerbose("BAR offset %u (0x%x) not 4-byte aligned\n", cap.bar_offset, cap.bar_offset);
		ok = false;
	}

	if (!check_bar_type_and_length(dev, cap.bar, cap.bar_length))
		ok = false;
	
	if (ok && do_setup)
	{
#warning TODO
	}
	return ok;
}

IOService* VirtioPCIDevice::probe(IOService* provider, SInt32* score)
{
	IOPCIDevice* pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (!pci_dev)
	{
		LogVerbose("VirtioPCIDevice::No PCI device found\n");
		return NULL;
	}
	
	uint32_t vendor_id = 0, device_id = 0;
	if (!djt_ioregentry_read_uint32_from_data_property(vendor_id, pci_dev, IOKIT_PCI_VENDOR_ID_KEY))
		LogVerbose("No Vendor ID on provider PCI device?\n");
	if (!djt_ioregentry_read_uint32_from_data_property(device_id, pci_dev, IOKIT_PCI_DEVICE_ID_KEY))
		LogVerbose("No Vendor ID on provider PCI device?\n");
	LogVerbose("Vendor ID: 0x%04x, device ID: 0x%04x\n", vendor_id, device_id);
	
	/*
	for (unsigned r = 0; r < 16; ++r)
	{
		uint8_t b[16];
		for (unsigned i = 0; i < 16; ++i)
		{
			b[i] = pci_dev->configRead8(r * 16 + i);
		}
		IOLog("%02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			b[0], b[1], b[2], b[3],
			b[4], b[5], b[6], b[7],
			b[8], b[9], b[10], b[11],
			b[12], b[13], b[14], b[15]);
	}
	*/
	
	uint8_t config_space[256];
	
	for (unsigned i = 0; i < 256; ++i)
	{
		config_space[i] = pci_dev->configRead8(i);
	}
	
	for (unsigned i = 0; i < 16; ++i)
	{
		unsigned offset = i * 16;
		const uint8_t* b = config_space + offset;
		LogVerbose("0x%04x:0x%04x [%3u]: %02x %02x %02x %02x  %02x %02x %02x %02x    %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			vendor_id, device_id, offset,
			b[0], b[1], b[2], b[3],  b[4], b[5], b[6], b[7],  b[8], b[9], b[10], b[11],  b[12], b[13], b[14], b[15]);
	}
	
	bool cap_checked[5] = {};
	
	bool mem_enable_reset = pci_dev->setMemoryEnable(true);
	
	bool ok =
		djt_iopcidevice_iterate_capabilities(
			pci_dev,
			[this, pci_dev, &cap_checked](uint8_t offset, uint8_t cap_type)
			{
				LogVerbose("Capability 0x%02x found at offset %u\n", cap_type, offset);
				if (cap_type == kIOPCIVendorSpecificCapability)
				{
					virtio_pci_cap cap =
					{
						.cap_vndr = pci_dev->configRead8( offset + VirtioCapOffset::CAP_VNDR),
						.cap_next = pci_dev->configRead8( offset + VirtioCapOffset::CAP_NEXT),
						.cap_len =  pci_dev->configRead8( offset + VirtioCapOffset::CAP_LEN),
						.cfg_type = pci_dev->configRead8( offset + VirtioCapOffset::CFG_TYPE),
						.bar =      pci_dev->configRead8( offset + VirtioCapOffset::BAR),
						.bar_offset =  pci_dev->configRead32(offset + VirtioCapOffset::OFFSET),
						.bar_length =  pci_dev->configRead32(offset + VirtioCapOffset::LENGTH),
					};
					LogVerbose("Virtio capability:\ncap_vndr = %d \ncap_next = %d \n cap_len = %d \n cfg_type = %d \n bar = %d \n offset = %d \n length = %d \n",
						cap.cap_vndr, cap.cap_next, cap.cap_len, cap.cfg_type,	cap.bar, cap.bar_offset, cap.bar_length);
					
					if (cap.cap_len < sizeof(cap))
					{
						LogVerbose("Virtio capability at offset %u too short: reported length %u, min %lu\n", offset, cap.cap_len, sizeof(cap));
					}
					else if (cap.cfg_type >= VIRTIO_PCI_CAP_COMMON_CFG && cap.cfg_type <= VIRTIO_PCI_CAP_PCI_CFG)
					{
						bool& cap_checked_flag = cap_checked[cap.cfg_type - 1];
						switch (cap.cfg_type)
						{
							case VIRTIO_PCI_CAP_COMMON_CFG:
								// "The driver SHOULD use the first instance of each virtio structure type they can support."
								if (!cap_checked_flag)
								{
									cap_checked_flag = this->setupCommonCFG(pci_dev, cap, offset, false /* don't set up, just check */);
								}
								break;
						
							case VIRTIO_PCI_CAP_NOTIFY_CFG:
								if (!cap_checked_flag)
								{
									cap_checked_flag = this->setupNotificationStructure(pci_dev, cap, offset, false /* don't set up, just check */);
								}
								break;
						
							case VIRTIO_PCI_CAP_ISR_CFG:
								if (!cap_checked_flag)
								{
									cap_checked_flag = this->setupISRStatusStructure(pci_dev, cap, offset, false /* don't set up, just check */);
								}
								break;
						
							case VIRTIO_PCI_CAP_DEVICE_CFG:
								if (!cap_checked_flag)
								{
									cap_checked_flag = this->setupDeviceSpecificStructure(pci_dev, cap, offset, false /* don't set up, just check */);
								}
								break;
						
							case VIRTIO_PCI_CAP_PCI_CFG:
								break;
						}
					}
					
				}
			});
	
	pci_dev->setMemoryEnable(mem_enable_reset);
	
	djt_pci_interrupt_index_ranges interrupt_ranges = djt_iopcidevice_find_interrupt_ranges(pci_dev);
	LogVerbose("Interrupt ranges: IRQ %d..%d, MSI(-X) %d..%d\n",
		interrupt_ranges.irq_pin_start, interrupt_ranges.irq_pin_end,
		interrupt_ranges.msi_start, interrupt_ranges.msi_end);
	
	if (interrupt_ranges.irq_pin_start == interrupt_ranges.irq_pin_end && interrupt_ranges.msi_start == interrupt_ranges.msi_end)
	{
		LogWarning("No interrupt sources found!\n");
		ok = false;
	}
	
	if (!ok)
		return nullptr;
	
	
	return this;
}

void VirtioPCIDevice::interruptAction(OSObject* me, IOInterruptEventSource* source, int count)
{
	int int_index = source->getIntIndex();
	VirtioPCIDevice* driver = static_cast<VirtioPCIDevice*>(me);
	int vector_index = int_index - driver->msi_handlers.base_index;
	LogVerbose("Interrupt on object %p, source %p, count = %d, index = %d (MSI[-X] vector %d)\n", me, source, count, int_index, vector_index);
}
bool VirtioPCIDevice::interruptFilter(OSObject* me, IOFilterInterruptEventSource* source)
{
	return true;
}

bool VirtioPCIDevice::start(IOService* provider)
{
	LogVerbose("\n");

	IOPCIDevice* pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (pci_dev == nullptr)
	{
		LogVerbose("Provider must be PCI device, aborting.\n");
		return false;
	}

	if (!this->IOService::start(provider))
		return false;

	bool ok = true;

	ok = setupInterruptHandlers(pci_dev);
	if (!ok)
	{
		this->stop(provider);
		return false;
	}
	
	return ok;
}

	
bool VirtioPCIDevice::setupInterruptHandlers(IOPCIDevice* pci_dev)
{
	djt_pci_interrupt_index_ranges interrupt_ranges = djt_iopcidevice_find_interrupt_ranges(pci_dev);
	LogVerbose("Interrupt ranges: IRQ %d..%d, MSI(-X) %d..%d\n",
		interrupt_ranges.irq_pin_start, interrupt_ranges.irq_pin_end,
		interrupt_ranges.msi_start, interrupt_ranges.msi_end);
	
	if (interrupt_ranges.irq_pin_start == interrupt_ranges.irq_pin_end && interrupt_ranges.msi_start == interrupt_ranges.msi_end)
	{
		LogWarning("No interrupt sources found!\n");
		return false;
	}
	
	bool ok = true;
	if (interrupt_ranges.msi_start != interrupt_ranges.msi_end)
	{
		// Device supports MSI or MSI-X
		unsigned msi_sources = interrupt_ranges.msi_end - interrupt_ranges.msi_start;
		this->msi_handlers.num_sources = msi_sources;
		this->msi_handlers.base_index = interrupt_ranges.msi_start;
		IOFilterInterruptEventSource** sources = IONew(IOFilterInterruptEventSource*, msi_sources);
		this->msi_handlers.sources = sources;
		IOWorkLoop** workloops = IONew(IOWorkLoop*, msi_sources);
		this->msi_handlers.workloops = workloops;
		memset(sources, 0, msi_sources * sizeof(sources[0]));
		memset(workloops, 0, msi_sources * sizeof(workloops[0]));
		for (int intr_index = interrupt_ranges.msi_start, i = 0; intr_index < interrupt_ranges.msi_end; ++intr_index, ++i)
		{
			LogVerbose("Trying to create interrupt source for index %d\n", intr_index);
			auto temp_event_source = IOFilterInterruptEventSource::filterInterruptEventSource(this, &interruptAction, &interruptFilter, pci_dev, intr_index);
			if (temp_event_source == nullptr)
			{
				LogWarning("VirtioLegacyPCIDevice beginHandlingInterrupts(): Error! Failed to create interrupt source with index %u.\n", intr_index);
				ok = false;
				break;
			}
			else
			{
				sources[i] = temp_event_source;
				auto wl = workloops[i] = IOWorkLoop::workLoop();
				if (wl == nullptr)
				{
					LogWarning("Failed to create interrupt workloop\n");
					ok = false;
					break;
				}
				else
				{
					LogVerbose("Interrupt index %d: source %p, workloop %p\n", intr_index, temp_event_source, wl);
					wl->addEventSource(temp_event_source);
					temp_event_source->enable();
				}
			}
		}
		
	}
	else
	{
		IOFilterInterruptEventSource* source =
			this->irq_source = IOFilterInterruptEventSource::filterInterruptEventSource(this, &interruptAction, &interruptFilter, pci_dev, interrupt_ranges.irq_pin_start);
		if (source == nullptr)
		{
			LogWarning("Failed to create interrupt source for pin-based interrupt\n");
			ok = false;
		}
		else
		{
			auto wl = this->irq_workloop = IOWorkLoop::workLoop();
			if (wl == nullptr)
			{
				LogWarning("Failed to create interrupt workloop\n");
				ok = false;
			}
			else
			{
				wl->addEventSource(source);
				source->enable();
			}
		}
	}
	LogVerbose("Done!\n");

	return ok;
}


void VirtioPCIDevice::shutdownInterruptHandlers()
{
	LogVerbose("Shutting down interrupts\n");
	IOFilterInterruptEventSource** sources = this->msi_handlers.sources;
	IOWorkLoop** workloops = this->msi_handlers.workloops;
	for (unsigned i = 0; i < this->msi_handlers.num_sources; ++i)
	{
		if (workloops[i] != nullptr)
		{
			LogVerbose("Disabling, removing and freeing: source %p, workloop %p\n", sources[i], workloops[i]);
			sources[i]->disable();
			workloops[i]->removeEventSource(sources[i]);
		}
		OSSafeReleaseNULL(workloops[i]);
		OSSafeReleaseNULL(sources[i]);
	}
	
	if (this->msi_handlers.num_sources > 0)
	{
		assert(sources != nullptr);
		assert(workloops != nullptr);
		IODelete(sources, IOFilterInterruptEventSource*, this->msi_handlers.num_sources);
		IODelete(workloops, IOWorkLoop*, this->msi_handlers.num_sources);
		this->msi_handlers.num_sources = 0;
		this->msi_handlers.workloops = nullptr;
		this->msi_handlers.sources = nullptr;
	}
	else if (this->irq_workloop != nullptr)
	{
		LogVerbose("Disabling, removing and freeing: source %p, workloop %p\n", this->irq_source, this->irq_workloop);
		this->irq_source->disable();
		this->irq_workloop->removeEventSource(this->irq_source);
		OSSafeReleaseNULL(this->irq_workloop);
		OSSafeReleaseNULL(this->irq_source);
	}

	LogVerbose("Done!\n");

}

void VirtioPCIDevice::stop(IOService* provider)
{
	this->shutdownInterruptHandlers();
	
	this->IOService::stop(provider);
}

