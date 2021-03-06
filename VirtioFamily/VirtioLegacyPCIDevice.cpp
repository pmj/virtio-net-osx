//
//  VirtioLegacyPCIDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#define __STDC_LIMIT_MACROS
#include "VirtioLegacyPCIDevice.h"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <stdint.h>

OSDefineMetaClassAndStructors(VirtioLegacyPCIDevice, VirtioDevice);

#define CHECK_BIT(var,pos) ((var) & (1llu<<(pos)))
#define VIRTIO_PCI_DEVICE_ISR_USED 0x01
#define VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE 0x02


namespace VirtioLegacyHeaderOffset
{
	enum VirtioLegacyPCIHeaderOffsets
	{
		DEVICE_FEATURE_BITS_0_31 = 0,
		GUEST_FEATURE_BITS_0_31 = 4 + DEVICE_FEATURE_BITS_0_31,
		QUEUE_ADDRESS = 4 + GUEST_FEATURE_BITS_0_31,
		QUEUE_SIZE = 4 + QUEUE_ADDRESS,
		QUEUE_SELECT = 2 + QUEUE_SIZE,
		QUEUE_NOTIFY = 2 + QUEUE_SELECT,
		DEVICE_STATUS = 2 + QUEUE_NOTIFY,
		ISR_STATUS = 1 + DEVICE_STATUS,
		BASIC_END_HEADER = 1 + ISR_STATUS,
		MSIX_CONFIG_VECTOR = BASIC_END_HEADER,
		MSIX_QUEUE_VECTOR = 2 + MSIX_CONFIG_VECTOR,
		MSIX_END_HEADER = 2 + MSIX_QUEUE_VECTOR,
	};
}
namespace
{
	const size_t VIRTIO_LEGACY_HEADER_MIN_LEN = VirtioLegacyHeaderOffset::BASIC_END_HEADER;
}

struct VirtioLegacyPCIVirtqueue
{
	IOBufferMemoryDescriptor* queue_mem;
	IODMACommand* queue_mem_dma;

	struct VirtioVirtqueue queue;
};

static inline bool is_pow2(uint16_t num)
{
	return 0u == (num & (num - 1));
}

const char* const VIRTIO_DEVICE_TYPES [] =
{
	"Reserved",
	"NetworkCard",
	"BlockDevice",
	"Console",
	"EntropySource",
	"MemoryBallooning",
	"ioMemory",
	"rpmsg",
	"SCSIHost",
	"9PTransport",
	"Mac80211Wlan",
	"RprocSerial",
	"VirtioCAIF"
};

namespace
{
	template <typename T, int N> char (&byte_array_of_same_dimension_as(T(&)[N]))[N];
}
#define DIMENSIONOF(x) sizeof(byte_array_of_same_dimension_as((x)))

IOService* VirtioLegacyPCIDevice::probe(IOService* provider, SInt32* score)
{
	//IOLog("VirtioLegacyPCIDevice::probe()\n");
	IOPCIDevice* pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (!pci_dev)
	{
		IOLog("VirtioPCIDevice::No PCI device found\n");
		return NULL;
	}

	// check the BAR0 range is in the I/O space and has the right minimum length
	if (0 == (kIOPCIIOSpace & pci_dev->configRead32(kIOPCIConfigBaseAddress0))) // is there a higher-level way of doing this?
	{
		IOLog("virtio-net probe(): BAR0 indicates the first device range is in the memory address space, this driver expects an I/O range.\n");
		return NULL;
	}
	if (IODeviceMemory* header_range = pci_dev->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0))
	{
		size_t header_len = header_range->getLength();
		if (header_len < VIRTIO_LEGACY_HEADER_MIN_LEN)
		{
			IOLog("virtio-net probe(): Virtio header I/O range too short. Expected at least %lu bytes, got %lu\n", VIRTIO_LEGACY_HEADER_MIN_LEN, header_len);
			return NULL;
		}
	}
	else
	{
		IOLog("virtio-net probe(): Failed to get virtio header I/O range\n");
		return NULL;
	}

	return this;
}

bool VirtioLegacyPCIDevice::start(IOService* provider)
{
	if (!IOService::start(provider))
	{
		return false;
	}
	//IOLog("VirtioLegacyPCIDevice::start()\n");
	IOPCIDevice* pciDevice = OSDynamicCast(IOPCIDevice, provider);
	OSObject* subSystemDeviceID = pciDevice->getProperty("subsystem-id");
	if(subSystemDeviceID == NULL)
	{
		return false;
	}
	OSData* deviceTypeIDData = OSDynamicCast(OSData, subSystemDeviceID);
	if (deviceTypeIDData == NULL)
	{
		return false;
	}

	//have a value check it is 4 bytes
	unsigned length = deviceTypeIDData->getLength();
	if (length != 4)
	{
		return false;
	}
	//correct length
	uint32_t deviceType;
	memcpy(&deviceType, deviceTypeIDData->getBytesNoCopy(), 4);
	this->virtio_device_type = deviceType;
	this->setProperty("VirtioDeviceTypeID", deviceType, 32);
	char name [100];
	if (deviceType < DIMENSIONOF(VIRTIO_DEVICE_TYPES))
	{
		snprintf(name, sizeof(name), "VirtioPCILegacyDevice@%s", VIRTIO_DEVICE_TYPES[deviceType]);
	}
	else
	{
		snprintf(name, sizeof(name), "VirtioPCILegacyDevice@%d", deviceType);
	}
	
	this->setName(name);
	if (!pciDevice->open(this))
	{
		IOLog("VirtioLegacyPCIDevice::start(): failed to open() PCI device %p (%s)\n", pciDevice, pciDevice ? pciDevice->getName() : "[NULL]");
		return false;
	}
	this->pci_device = pciDevice;
	
	if(!this->mapHeaderIORegion())
	{
		pciDevice->close(this);
		this->pci_device = nullptr;
		return false;
	}
	
	this->deviceSpecificConfigStartHeaderOffset = VirtioLegacyHeaderOffset::BASIC_END_HEADER;
	
	this->resetDevice();
	//write out supported features
	uint32_t supportedFeatures = this->supportedFeatures();
	this->setProperty("VirtioDeviceSupportedFeatures", supportedFeatures, 32);
	
	this->failDevice();
	
	this->registerService();
	return true;
}

bool VirtioLegacyPCIDevice::mapHeaderIORegion()
{
	assert(this->pci_virtio_header_iomap==NULL);
	IOMemoryMap* iomap = this->pci_device->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!iomap)
	{
		IOLog("writeDeviceStatusField mapConfigurationSpace(): Error! Memory-Mapping configuration space failed.\n");
		return false;
	}
	
	kprintf("writeDeviceStatusField mapConfigurationSpace(): Mapped %llu bytes of device memory at %llX. (physical address %llX)\n",
		static_cast<uint64_t>(iomap->getLength()), iomap->getAddress(), this->pci_device->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)->getPhysicalSegment(0, NULL, 0));

	IOByteCount config_bytes = iomap->getLength();
	for (uint16_t offset = 0; offset < config_bytes; offset += 4)
	{
		uint32_t val = this->pci_device->ioRead32(offset, this->pci_virtio_header_iomap);
		kprintf("%08x%s", val, (offset % 16 == 12) ? "\n" : " ");
	}
	kprintf("\n");

	this->pci_virtio_header_iomap = iomap;
	return true;
}


bool VirtioLegacyPCIDevice::resetDevice()
{
	this->pci_device->ioWrite8(VirtioLegacyHeaderOffset::DEVICE_STATUS, 0, this->pci_virtio_header_iomap);
	uint16_t deviceStatusValue = this->pci_device->ioRead8(
		VirtioLegacyHeaderOffset::DEVICE_STATUS, this->pci_virtio_header_iomap);
	if(deviceStatusValue != 0)
		return false;
	this->pci_device->ioWrite8(VirtioLegacyHeaderOffset::DEVICE_STATUS, 1, this->pci_virtio_header_iomap);
	this->pci_device->ioWrite8(VirtioLegacyHeaderOffset::DEVICE_STATUS, 1|2, this->pci_virtio_header_iomap);
	//read out feature bits
	this->features = this->pci_device->ioRead32(
		VirtioLegacyHeaderOffset::DEVICE_FEATURE_BITS_0_31, this->pci_virtio_header_iomap);
	return true;
}

uint32_t VirtioLegacyPCIDevice::supportedFeatures()
{
	return this->features;
}

bool VirtioLegacyPCIDevice::requestFeatures(uint32_t use_features)
{
//read out feature bits
	
	uint32_t invertedSupportedFeatures = ~this->features;
	uint32_t supportedFeaturesANDuseFeatures = invertedSupportedFeatures & use_features;
	if (supportedFeaturesANDuseFeatures != 0)
	{
		//a feature is present in the use features that is not supported
		return false;
	}
	if ((use_features & VirtioDeviceGenericFeature::VIRTIO_F_RING_EVENT_IDX) != 0)
	{
		IOLog("VirtioLegacyPCIDevice::requestFeatures(): feature VIRTIO_F_RING_EVENT_IDX (bit 29) is not currently supported.\n");
		return false;
	}
	
	//check bit 30 isnt set
	if (CHECK_BIT(use_features, 30))
	{
		//bit 30 is set which we do not want
		IOLog("VirtioLegacyPCIDevice::requestFeatures(): Do not request feature bit 30 - it is obsolete.\n");
		return false;
	}
	this->active_features = use_features;
	
	//otherwise all use features are in our supported features
	this->pci_device->ioWrite32(VirtioLegacyHeaderOffset::GUEST_FEATURE_BITS_0_31, use_features, this->pci_virtio_header_iomap);
	return true;
}

void VirtioLegacyPCIDevice::failDevice()
{
	//write 128 to device status field realse and null memory OSSafe Realease
	this->pci_device->ioWrite8(VirtioLegacyHeaderOffset::DEVICE_STATUS, 128, this->pci_virtio_header_iomap);
	this->endHandlingInterrupts();
	OSSafeReleaseNULL(this->pci_virtio_header_iomap);
}

/// Virtqueue size calculation, see section 2.3 in legacy virtio spec
static const size_t VIRTIO_PAGE_SIZE = 4096;
static inline unsigned virtio_page_align(unsigned size)
{
	return (size + VIRTIO_PAGE_SIZE - 1u) & ~(VIRTIO_PAGE_SIZE - 1u);
}
static inline unsigned vring_mem_size(unsigned qsz)
{
	return virtio_page_align(sizeof(VirtioVringDesc) * qsz + sizeof(uint16_t) * (2 + qsz))
		+ virtio_page_align(sizeof(VirtioVringUsedElement) * qsz);
}

IOReturn VirtioLegacyPCIDevice::setupVirtqueue(VirtioLegacyPCIVirtqueue* queue, uint16_t queue_id, bool interrupts_enabled, unsigned indirect_desc_per_request)
{
	// write queue selector
	this->pci_device->ioWrite16(VirtioLegacyHeaderOffset::QUEUE_SELECT, queue_id, this->pci_virtio_header_iomap);
	
	// read queue size
	uint16_t num_queue_entries = this->pci_device->ioRead16(VirtioLegacyHeaderOffset::QUEUE_SIZE, this->pci_virtio_header_iomap);
	// TODO: check it's power of 2
	if (num_queue_entries == 0)
	{
		IOLog("VirtioLegacyPCIDevice::setupVirtqueue(): Queue size for queue %u is 0.\n", queue_id);
		return kIOReturnBadArgument;
	}
	else if (!is_pow2(num_queue_entries))
	{
		IOLog("VirtioLegacyPCIDevice::setupVirtqueue(): Queue size for queue %u is %u, which is not a power of 2. Aborting.\n", queue_id, num_queue_entries);
		return kIOReturnDeviceError;
	}
	//IOLog("VirtioLegacyPCIDevice::setupVirtqueue(): Queue size for queue %u is %u.\n", queue_id, num_queue_entries);

	// calculate queue memory size
	const size_t queue_mem_size = vring_mem_size(num_queue_entries);
	
	// allocate & zero-init queue memory
	const mach_vm_address_t VIRTIO_RING_ALLOC_MASK = 0xffffffffull << 12u;
	IOBufferMemoryDescriptor* queue_mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut, queue_mem_size, VIRTIO_RING_ALLOC_MASK);
	if (queue_mem == nullptr)
		return kIOReturnNoMemory;
	
	memset(queue_mem->getBytesNoCopy(), 0, queue_mem_size);


	// allocate and inititalise DMA command (in+out directions)
	IODMACommand* dma_cmd = IODMACommand::withSpecification(
		IODMACommand::OutputHost64, 12 + 32,
		0 /* no limit on segment size */, IODMACommand::kMapped, 0 /* no limit on transfer size */, 4096 /* alignment */);
	if (dma_cmd == nullptr)
	{
		OSSafeReleaseNULL(queue_mem);
		return kIOReturnNoMemory;
	}
	IOReturn result = dma_cmd->setMemoryDescriptor(queue_mem);
	if (result != kIOReturnSuccess)
	{
		OSSafeReleaseNULL(queue_mem);
		OSSafeReleaseNULL(dma_cmd);
		return result;
	}
	// extract the physical address
	IODMACommand::Segment64 phys_segment = {};
	uint64_t offset = 0;
	UInt32 num_segments = 1;
	result = dma_cmd->genIOVMSegments(&offset, &phys_segment, &num_segments);
	if (result == kIOReturnSuccess && (offset != queue_mem_size || num_segments != 1 || phys_segment.fLength != queue_mem_size))
	{
		result = kIOReturnInternalError;
	}
	if (result != kIOReturnSuccess)
	{
		dma_cmd->clearMemoryDescriptor();
		OSSafeReleaseNULL(queue_mem);
		OSSafeReleaseNULL(dma_cmd);
		return result;
	}
	
	bool use_indirect =
		((this->active_features & VirtioDeviceGenericFeature::VIRTIO_F_RING_INDIRECT_DESC) && indirect_desc_per_request > 0);
	queue->queue.indirect_descriptors = use_indirect;
	
	// allocate array of VirtioBuffers for descriptor_array
	const size_t desc_buffer_array_size = sizeof(queue->queue.descriptor_buffers[0]) * num_queue_entries;
	VirtioBuffer* descriptor_buffers = static_cast<VirtioBuffer*>(
		IOMallocAligned(desc_buffer_array_size, alignof(decltype(queue->queue.descriptor_buffers[0]))));
	memset(descriptor_buffers, 0, desc_buffer_array_size);
	for (unsigned i = 0; i < num_queue_entries; i++)
	{
		bool ok = true;

		if(use_indirect)
		{
			IOBufferMemoryDescriptor* indirect_descriptors = IOBufferMemoryDescriptor::inTaskWithOptions(
				kernel_task, kIODirectionOut | kIOMemoryPhysicallyContiguous, indirect_desc_per_request * sizeof(VirtioVringDesc), alignof(VirtioVringDesc));
			IODMACommand* dma_indirect_descriptors = descriptor_buffers[i].dma_cmd = IODMACommand::withSpecification(
				outputVringDescSegmentForIndirectTable, 64, UINT32_MAX, IODMACommand::kMapped, UINT32_MAX);
			IODMACommand* buffer_dma = descriptor_buffers[i].dma_cmd = IODMACommand::withSpecification(
				outputIndirectVringDescSegment, 64, UINT32_MAX, IODMACommand::kMapped, UINT32_MAX);
			IODMACommand* dma_cmd_2 = descriptor_buffers[i].dma_cmd = IODMACommand::withSpecification(
				outputIndirectVringDescSegment, 64, UINT32_MAX, IODMACommand::kMapped, UINT32_MAX);
			
			if (buffer_dma && dma_cmd_2 && dma_indirect_descriptors && indirect_descriptors)
			{
				descriptor_buffers[i].dma_cmd = buffer_dma;
				descriptor_buffers[i].dma_cmd_2 = dma_cmd_2;
				descriptor_buffers[i].dma_indirect_descriptors = dma_indirect_descriptors;
				descriptor_buffers[i].indirect_descriptors = indirect_descriptors;
			}
			else
			{
				ok = false;
				OSSafeReleaseNULL(buffer_dma);
				OSSafeReleaseNULL(dma_cmd_2);
				OSSafeReleaseNULL(dma_indirect_descriptors);
				OSSafeReleaseNULL(indirect_descriptors);
			}
		}
		else
		{
			IODMACommand* buffer_dma = descriptor_buffers[i].dma_cmd = IODMACommand::withSpecification(
				outputVringDescSegment, 64, UINT32_MAX, IODMACommand::kMapped, UINT32_MAX);
			descriptor_buffers[i].dma_cmd = buffer_dma;
			descriptor_buffers[i].dma_cmd_2 = nullptr;
			descriptor_buffers[i].dma_indirect_descriptors = nullptr;
			descriptor_buffers[i].indirect_descriptors = nullptr;
			
			ok = buffer_dma != nullptr;
		}

		if (!ok)
		{
			for (unsigned j = 0; j < i; ++j)
			{
				OSSafeReleaseNULL(descriptor_buffers[j].dma_cmd);
			}
			OSSafeReleaseNULL(queue_mem);
			OSSafeReleaseNULL(dma_cmd);
			IOFreeAligned(descriptor_buffers, desc_buffer_array_size);
			return kIOReturnNoMemory;
		}
	}
	
	queue->queue_mem = queue_mem;
	queue->queue_mem_dma = dma_cmd;
	
	// fill out virtqueue pointer fields (desc table, rings, etc.)
	uint8_t* queue_mem_bytes = static_cast<uint8_t*>(queue_mem->getBytesNoCopy());
	unsigned spaceUsed = 0;
	queue->queue.descriptor_table = (struct VirtioVringDesc*)queue_mem_bytes;
	spaceUsed += sizeof(queue->queue.descriptor_table[0]) * num_queue_entries;
	queue->queue.available_ring = (struct VirtioVringAvail*)(queue_mem_bytes + spaceUsed);
	spaceUsed += sizeof(*queue->queue.available_ring) + sizeof(queue->queue.available_ring->ring[0]) * num_queue_entries;
	
	if (this->eventIndexFeatureEnabled)
	{
		queue->queue.used_ring_interrupt_index = (uint16_t*)(queue_mem_bytes + spaceUsed);
		spaceUsed += sizeof(*queue->queue.used_ring_interrupt_index);
	}
	
	spaceUsed = virtio_page_align(spaceUsed);
	queue->queue.used_ring = (struct VirtioVringUsed*)(queue_mem_bytes + spaceUsed);
	spaceUsed += sizeof(*queue->queue.used_ring) + sizeof(queue->queue.used_ring->ring[0]) * num_queue_entries;
	if (this->eventIndexFeatureEnabled)
	{
		queue->queue.used_ring_interrupt_index = (uint16_t*)(queue_mem_bytes + spaceUsed);
		spaceUsed += sizeof(*queue->queue.used_ring_interrupt_index);
	}
	
	queue->queue.used_ring_last_head_index = queue->queue.used_ring->head_index;

	queue->queue.interrupts_requested = interrupts_enabled;
	queue->queue.available_ring->flags = interrupts_enabled ? 0 : VirtioVringAvailFlag::NO_INTERRUPT;
	
	// initialise list of unused descriptors:
	queue->queue.first_unused_descriptor_index = 0;
	// iterate over all VirtioBuffers in descriptor_array and set up their next_desc to +1
	for(uint16_t i = 0; i < num_queue_entries; i++)
	{
		if(i == num_queue_entries-1)
		{
			descriptor_buffers[i].next_desc = -1;
		}
		else
		{
			descriptor_buffers[i].next_desc = i + 1;
		}
	}
	// set last next_desc to -1
	queue->queue.num_unused_descriptors = num_queue_entries;
	
	// write queue memory address to queue address field
	uint32_t address = static_cast<uint32_t>(phys_segment.fIOVMAddr >> 12);
	this->pci_device->ioWrite32(VirtioLegacyHeaderOffset::QUEUE_ADDRESS, address, this->pci_virtio_header_iomap);
	
	queue->queue.num_entries = num_queue_entries;
	queue->queue.descriptor_buffers = descriptor_buffers;
	
	
	
	return kIOReturnSuccess;
}

IOReturn VirtioLegacyPCIDevice::setVirtqueueInterruptsEnabled(uint16_t queue_id, bool enabled)
{
	if (queue_id > this->num_virtqueues)
	{
		return kIOReturnBadArgument;
	}
	
	if (this->virtqueues[queue_id].queue.interrupts_requested != enabled)
	{
		this->virtqueues[queue_id].queue.interrupts_requested = enabled;
		this->virtqueues[queue_id].queue.available_ring->flags = enabled ? 0 : VirtioVringAvailFlag::NO_INTERRUPT;
	}
	return kIOReturnSuccess;
}


static void destroy_virtqueue(VirtioLegacyPCIVirtqueue* queue)
{
	// free any resources allocated for the queue
	for (unsigned i = 0; i < queue->queue.num_entries; ++i)
	{
		OSSafeReleaseNULL(queue->queue.descriptor_buffers[i].dma_cmd);
	}

	IOFreeAligned(queue->queue.descriptor_buffers, sizeof(queue->queue.descriptor_buffers[0])* queue->queue.num_entries);
	queue->queue.descriptor_buffers = nullptr;
	queue->queue_mem_dma->clearMemoryDescriptor();
	OSSafeReleaseNULL(queue->queue_mem_dma);
	OSSafeReleaseNULL(queue->queue_mem);
}

IOReturn VirtioLegacyPCIDevice::setupVirtqueues(uint16_t number_queues, const bool queue_interrupts_enabled[], unsigned out_queue_sizes[], const unsigned indirect_desc_per_request[])
{
	const size_t queue_array_size = sizeof(this->virtqueues[0]) * number_queues;
	VirtioLegacyPCIVirtqueue* queues = static_cast<VirtioLegacyPCIVirtqueue*>(
		IOMallocAligned(queue_array_size, alignof(decltype(this->virtqueues[0]))));
	if (queues == nullptr)
		return kIOReturnNoMemory;
	memset(queues, 0, queue_array_size);
	
	IOReturn result = kIOReturnSuccess;
	for (uint16_t i = 0; i < number_queues; ++i)
	{
		result = this->setupVirtqueue(&queues[i], i, queue_interrupts_enabled ? queue_interrupts_enabled[i] : true, indirect_desc_per_request ? indirect_desc_per_request[i] : 0);
		
		if (result != kIOReturnSuccess)
		{
			this->failDevice();
			for (unsigned j = 0; j < i; ++j)
			{
				destroy_virtqueue(&queues[j]);
			}
			goto fail;
		}
		
		if (out_queue_sizes != nullptr)
		{
			out_queue_sizes[i] = queues[i].queue.num_entries;
		}
	}
	
	this->virtqueues = queues;
	this->num_virtqueues = number_queues;
	
	return kIOReturnSuccess;
	
fail:
	IOFreeAligned(queues, queue_array_size);
	
	return result;
}

void VirtioLegacyPCIDevice::startDevice(ConfigChangeAction action, OSObject* target, IOWorkLoop* workloop)
{
	// save action & target
	this->configChangeAction = action;
	this->configChangeTarget = target;
	
	// enable interrupt handling
	this->beginHandlingInterrupts(workloop);


	this->pci_device->ioWrite8(VirtioLegacyHeaderOffset::DEVICE_STATUS, 1|2|4, this->pci_virtio_header_iomap);

	kprintf("Config area after device start:\n");
	IOByteCount config_bytes = this->pci_virtio_header_iomap->getLength();
	for (uint16_t offset = 0; offset < config_bytes; offset += 4)
	{
		uint32_t val = this->pci_device->ioRead32(offset, this->pci_virtio_header_iomap);
		kprintf("%08x%s", val, offset % 4 == 3 ? "\n" : " ");
	}
	kprintf("\n");
}

bool VirtioLegacyPCIDevice::handleOpen(IOService* forClient, IOOptionBits options, void* arg)
{
	if (this->pci_virtio_header_iomap != nullptr)
		return false;
	if (!VirtioDevice::handleOpen(forClient, options, arg))
		return false;
	
	if (!this->mapHeaderIORegion())
	{
		VirtioDevice::handleClose(forClient, options);
		return false;
	}
	
	return true;
}

void VirtioLegacyPCIDevice::handleClose(IOService* forClient, IOOptionBits options)
{
	if(this->virtqueues != nullptr)
	{
		this->failDevice();
		for (unsigned j = 0; j < this->num_virtqueues ; ++j)
		{
			destroy_virtqueue(&this->virtqueues[j]);
		}
		IOFreeAligned(this->virtqueues, sizeof(this->virtqueues[0]) * this->num_virtqueues);
		this->virtqueues = nullptr;
		this->num_virtqueues = 0;
	}
	OSSafeReleaseNULL(this->pci_virtio_header_iomap);
	VirtioDevice::handleClose(forClient, options);
	
}

uint8_t VirtioLegacyPCIDevice::readDeviceConfig8(uint16_t device_specific_offset)
{
	return this->pci_device->ioRead8(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
}

uint16_t VirtioLegacyPCIDevice::readDeviceConfig16LETransitional(uint16_t device_specific_offset)
{
	return VirtioLegacyPCIDevice::readDeviceConfig16Native(device_specific_offset);
}
uint32_t VirtioLegacyPCIDevice::readDeviceConfig32LETransitional(uint16_t device_specific_offset)
{
	return VirtioLegacyPCIDevice::readDeviceConfig32Native(device_specific_offset);
}
uint64_t VirtioLegacyPCIDevice::readDeviceConfig64LETransitional(uint16_t device_specific_offset)
{
	return this->readDeviceConfig64Native(device_specific_offset);
}

uint16_t VirtioLegacyPCIDevice::readDeviceConfig16Native(uint16_t device_specific_offset)
{
	return this->pci_device->ioRead16(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
}

uint32_t VirtioLegacyPCIDevice::readDeviceConfig32Native(uint16_t device_specific_offset)
{
	return this->pci_device->ioRead32(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
}

uint64_t VirtioLegacyPCIDevice::readDeviceConfig64Native(uint16_t device_specific_offset)
{
#if defined(__LITTLE_ENDIAN__)
	uint32_t low = this->pci_device->ioRead32(
		this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
	uint32_t high = this->pci_device->ioRead32(
		this->deviceSpecificConfigStartHeaderOffset + device_specific_offset + 4, this->pci_virtio_header_iomap);
#elif defined(__BIG_ENDIAN__)
	uint32_t high = this->pci_device->ioRead32(
		this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
	uint32_t low = this->pci_device->ioRead32(
		this->deviceSpecificConfigStartHeaderOffset + device_specific_offset + 4, this->pci_virtio_header_iomap);
#endif
	return (static_cast<uint64_t>(high) << 32u) | low;
}


void VirtioLegacyPCIDevice::writeDeviceConfig8(uint16_t offset, uint8_t value_to_write)
{
	this->pci_device->ioWrite8(this->deviceSpecificConfigStartHeaderOffset + offset, value_to_write, this->pci_virtio_header_iomap);
}

void VirtioLegacyPCIDevice::writeDeviceConfig16Native(uint16_t offset, uint16_t value_to_write)
{
	this->pci_device->ioWrite16(this->deviceSpecificConfigStartHeaderOffset + offset, value_to_write, this->pci_virtio_header_iomap);
}

void VirtioLegacyPCIDevice::writeDeviceConfig32Native(uint16_t offset, uint32_t value_to_write)
{
	this->pci_device->ioWrite32(this->deviceSpecificConfigStartHeaderOffset + offset, value_to_write, this->pci_virtio_header_iomap);
}

void VirtioLegacyPCIDevice::writeDeviceConfig16LETransitional(uint16_t device_specific_offset, uint16_t value_to_write)
{
	this->writeDeviceConfig16Native(device_specific_offset, value_to_write);
}

void VirtioLegacyPCIDevice::writeDeviceConfig32LETransitional(uint16_t device_specific_offset, uint32_t value_to_write)
{
	this->writeDeviceConfig32Native(device_specific_offset, value_to_write);
}


struct virtio_legacy_pci_vring_desc_chain
{
	VirtioVirtqueue* queue;
	// Index of a descriptor that has already been reserved, or UINT16_MAX
	uint16_t reserved_descriptor_index;
	uint16_t current_last_descriptor_index;
	bool device_writable;
};

int16_t reserveNewDescriptor(VirtioVirtqueue* virtqueue)
{
	int16_t descriptorIndex = virtqueue->first_unused_descriptor_index;
	if (descriptorIndex < 0)
		return -1;
	virtqueue->first_unused_descriptor_index = virtqueue->descriptor_buffers[descriptorIndex].next_desc;
	virtqueue->num_unused_descriptors--;
	return descriptorIndex;
}

void returnUnusedDescriptor(VirtioVirtqueue* virtqueue, uint16_t descriptorIndex)
{
	virtqueue->num_unused_descriptors++;
	virtqueue->descriptor_buffers[descriptorIndex].next_desc = virtqueue->first_unused_descriptor_index;
	virtqueue->first_unused_descriptor_index = descriptorIndex;
}

IOReturn VirtioLegacyPCIDevice::submitBuffersToVirtqueue(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion)
{
	if(queue_index >= this->num_virtqueues)
	{
		return kIOReturnBadArgument;
	}
	
	VirtioVirtqueue* queue = &this->virtqueues[queue_index].queue;
	if (queue->indirect_descriptors)
	{
		return this->submitBuffersToVirtqueueIndirect(queue_index, device_readable_buf, device_writable_buf, completion);
	}
	else
	{
		return this->submitBuffersToVirtqueueDirect(queue_index, device_readable_buf, device_writable_buf, completion);
	}
}

static void virtio_virtqueue_add_descriptor_to_ring(VirtioVirtqueue* queue, uint16_t first_descriptor_index);

IOReturn VirtioLegacyPCIDevice::submitBuffersToVirtqueueDirect(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion)
{
	VirtioVirtqueue* queue = &this->virtqueues[queue_index].queue;
	
	uint16_t first_descriptor_index = UINT16_MAX;
	virtio_legacy_pci_vring_desc_chain chain = { queue, UINT16_MAX, UINT16_MAX, false };
	
	const bool device_readable_descs = (device_readable_buf != nullptr && device_readable_buf->getLength() != 0);
	const bool device_writable_descs = (device_writable_buf != nullptr && device_writable_buf->getLength() != 0);
	unsigned min_descs_required = (device_readable_descs ? 1 : 0) + (device_writable_descs ? 1 : 0);
	if (min_descs_required > queue->num_entries)
	{
		return kIOReturnUnsupported;
	}
	if (min_descs_required == 0)
	{
		return kIOReturnBadArgument;
	}
	if (min_descs_required > queue->num_unused_descriptors)
	{
		return kIOReturnBusy;
	}
	
	if (device_readable_descs)
	{
		min_descs_required--;
		UInt32 max_segments = queue->num_unused_descriptors - min_descs_required;
		
		// 1. reserve a descriptor
		int16_t descriptorIndex = reserveNewDescriptor(queue);
		//IOLog("VirtioLegacyPCIDevice::submitBuffersToVirtqueue(): reserved descriptor %d as first device_readable\n", descriptorIndex);
		// 2. save it as first_descriptor_index
		first_descriptor_index = descriptorIndex;
		// 3. save it as chain.reserved_descriptor_index
		VirtioBuffer* desc_buffer = &queue->descriptor_buffers[descriptorIndex];
		chain.reserved_descriptor_index = descriptorIndex;
		desc_buffer->completion = completion;
		// 4. get its DMA command:
		IODMACommand* device_readable_dma = desc_buffer->dma_cmd;
		desc_buffer->dma_cmd_used = true;
		// 5. prepare DMA command
		IOReturn result = device_readable_dma->setMemoryDescriptor(device_readable_buf, true /* prepare DMA */);
		if (result != kIOReturnSuccess)
		{
			returnUnusedDescriptor(queue, descriptorIndex);
			return result;
		}
		// 6.
		UInt64 offset = 0;
		result = device_readable_dma->genIOVMSegments(&offset, &chain, &max_segments);
		if (result != kIOReturnSuccess || max_segments < 1 || offset != device_readable_buf->getLength())
		{
			if (result == kIOReturnSuccess)
			{
				IOLog("VirtioLegacyPCIDevice::submitBuffersToVirtqueue(): emitted %u segments up to offset %llu for device-readable buffer with %llu bytes\n", max_segments, offset, device_readable_buf->getLength());
				result = kIOReturnInternalError;
			}
			device_readable_dma->clearMemoryDescriptor(true /* complete dma */);
			// clean up, return descriptors to unused list
			desc_buffer->dma_cmd_used = false;
			
			while(descriptorIndex >= 0)
			{
				int16_t next = queue->descriptor_buffers[descriptorIndex].next_desc;
				returnUnusedDescriptor(queue, descriptorIndex);
				descriptorIndex = next;
			}
			return result;
		}
	}
	if (device_writable_descs)
	{
		chain.device_writable = true;
		// otherwise, same as above..
		UInt32 max_segments = queue->num_unused_descriptors;
		
		// 1. reserve a descriptor
		int16_t descriptorIndex = reserveNewDescriptor(queue);
		//IOLog("VirtioLegacyPCIDevice::submitBuffersToVirtqueue(): reserved descriptor %d as first device writable\n", descriptorIndex);
		// 2. save it as first_descriptor_index
		VirtioBuffer* desc_buffer = &queue->descriptor_buffers[descriptorIndex];
		if(first_descriptor_index == UINT16_MAX)
		{
			//no reads have been made so the first descriptor needs to be set
			first_descriptor_index = descriptorIndex;
			desc_buffer->completion = completion;
		}
		// 3. save it as chain.reserved_descriptor_index
		chain.reserved_descriptor_index = descriptorIndex;
		// 4. get its DMA command:
		IODMACommand* dma = desc_buffer->dma_cmd;
		desc_buffer->dma_cmd_used = true;
		// 5.
		IOReturn result = dma->setMemoryDescriptor(device_writable_buf, true /* prepare DMA */);
		if (result != kIOReturnSuccess)
		{
			descriptorIndex = first_descriptor_index;
			while(descriptorIndex >= 0)
			{
				int16_t next = queue->descriptor_buffers[descriptorIndex].next_desc;
				if (queue->descriptor_buffers[descriptorIndex].dma_cmd_used)
				{
					queue->descriptor_buffers[descriptorIndex].dma_cmd->clearMemoryDescriptor(true);
					queue->descriptor_buffers[descriptorIndex].dma_cmd_used = false;
				}
				returnUnusedDescriptor(queue, descriptorIndex);
				descriptorIndex = next;
			}
			return result;
		}
		// 6.
		UInt64 offset = 0;
		result = dma->genIOVMSegments(&offset, &chain, &max_segments);
		if (result != kIOReturnSuccess || max_segments < 1 || offset != device_writable_buf->getLength())
		{
			if (result == kIOReturnSuccess)
			{
				IOLog("VirtioLegacyPCIDevice::submitBuffersToVirtqueue(): emitted %u segments up to offset %llu for device-writable buffer with %llu bytes\n", max_segments, offset, device_writable_buf->getLength());
				result = kIOReturnInternalError;
			}
			dma->clearMemoryDescriptor(true /* complete dma */);
			// clean up, return descriptors to unused list
			desc_buffer->dma_cmd_used = false;
			descriptorIndex = first_descriptor_index;
			while(descriptorIndex >= 0)
			{
				int16_t next = queue->descriptor_buffers[descriptorIndex].next_desc;
				if (queue->descriptor_buffers[descriptorIndex].dma_cmd_used)
				{
					queue->descriptor_buffers[descriptorIndex].dma_cmd->clearMemoryDescriptor(true);
					queue->descriptor_buffers[descriptorIndex].dma_cmd_used = false;
				}
				returnUnusedDescriptor(queue, descriptorIndex);
				descriptorIndex = next;
			}
			return result;
		}

		
	}
	
	virtio_virtqueue_add_descriptor_to_ring(queue, first_descriptor_index);
	
	if((queue->used_ring->flags & VirtioVringUsedFlag::NO_NOTIFY)==0)
	{
		pci_device->ioWrite16(VirtioLegacyHeaderOffset::QUEUE_NOTIFY, queue_index, this->pci_virtio_header_iomap);
	}

	return kIOReturnSuccess;
}

static void virtio_virtqueue_add_descriptor_to_ring(VirtioVirtqueue* queue, uint16_t first_descriptor_index)
{
	// add index of first descriptor in chain to 'available' ring
	
	uint16_t avail_pos = queue->available_ring->head_index;
	queue->available_ring->ring[avail_pos % queue->num_entries] = first_descriptor_index;
	avail_pos++;
	OSSynchronizeIO();
	queue->available_ring->head_index = avail_pos;
	OSSynchronizeIO();
}

struct virtio_output_indirect_segment_state
{
	VirtioVringDesc* desc_array;
	uint16_t next_descriptor_index;
	bool writable;
};
struct virtio_output_segment_for_indirect_descs_state
{
	VirtioVirtqueue* queue;
	int16_t main_descriptor_index;
};

static IOReturn generate_indirect_segment_dma(VirtioVirtqueue* queue, IODMACommand* dma_cmd, IOMemoryDescriptor* buf, unsigned& min_descs_required, UInt32& max_segments, virtio_output_indirect_segment_state* desc_output);

IOReturn VirtioLegacyPCIDevice::submitBuffersToVirtqueueIndirect(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion)
{
	VirtioVirtqueue* queue = &this->virtqueues[queue_index].queue;
	
	int16_t main_descriptor_index = reserveNewDescriptor(queue);
	if (main_descriptor_index < 0)
		return kIOReturnBusy;
	
	const bool device_readable_descs = (device_readable_buf != nullptr && device_readable_buf->getLength() != 0);
	const bool device_writable_descs = (device_writable_buf != nullptr && device_writable_buf->getLength() != 0);
	unsigned min_descs_required = (device_readable_descs ? 1 : 0) + (device_writable_descs ? 1 : 0);
	
	VirtioBuffer* desc_buffer = &queue->descriptor_buffers[main_descriptor_index];
	desc_buffer->indirect_descriptors->setLength(desc_buffer->indirect_descriptors->getCapacity());
	UInt32 max_segments = static_cast<UInt32>(
		desc_buffer->indirect_descriptors->getLength() / sizeof(VirtioVringDesc));
	VirtioVringDesc* desc_array = static_cast<VirtioVringDesc*>(desc_buffer->indirect_descriptors->getBytesNoCopy());
	
	if (min_descs_required > max_segments)
	{
		return kIOReturnUnsupported;
	}
	if (min_descs_required == 0)
	{
		return kIOReturnBadArgument;
	}

	virtio_output_indirect_segment_state desc_output = { desc_array, 0 };
	desc_buffer->dma_cmd_used = true;
	if (device_readable_descs)
	{
		desc_output.writable = false;
		IOReturn result = generate_indirect_segment_dma(queue, desc_buffer->dma_cmd, device_readable_buf, min_descs_required, max_segments, &desc_output);
		if (result != kIOReturnSuccess)
		{
			returnUnusedDescriptor(queue, main_descriptor_index);
			return result;
		}
	}
	if (device_writable_descs)
	{
		desc_output.writable = true;
		IOReturn result = generate_indirect_segment_dma(queue, desc_buffer->dma_cmd_2, device_writable_buf, min_descs_required, max_segments, &desc_output);
		if (result != kIOReturnSuccess)
		{
			if (device_readable_descs)
				desc_buffer->dma_cmd->clearMemoryDescriptor();
			returnUnusedDescriptor(queue, main_descriptor_index);
			return result;
		}
	}
	
	desc_buffer->indirect_descriptors->setLength(desc_output.next_descriptor_index * sizeof(VirtioVringDesc));
	IOReturn result = desc_buffer->dma_indirect_descriptors->setMemoryDescriptor(desc_buffer->indirect_descriptors, true /* prepare DMA */);
	if (result != kIOReturnSuccess)
	{
		desc_buffer->dma_cmd->clearMemoryDescriptor();
		desc_buffer->dma_cmd_2->clearMemoryDescriptor();
		returnUnusedDescriptor(queue, main_descriptor_index);
		return result;
	}
	
	UInt64 offset = 0;
	UInt32 segments = 1;
	virtio_output_segment_for_indirect_descs_state state = { queue, main_descriptor_index };
	result = desc_buffer->dma_indirect_descriptors->genIOVMSegments(&offset, &state, &segments);
	if (result != kIOReturnSuccess || segments < 1 || offset != desc_buffer->indirect_descriptors->getLength())
	{
		desc_buffer->dma_indirect_descriptors->clearMemoryDescriptor();
		desc_buffer->dma_cmd->clearMemoryDescriptor();
		desc_buffer->dma_cmd_2->clearMemoryDescriptor();
		returnUnusedDescriptor(queue, main_descriptor_index);
		return result;
	}
	desc_buffer->completion = completion;
	desc_buffer->next_desc = -1;
	
	/*
	kprintf("Emitting request on descriptor %u with %llu bytes and %u indirect descriptors...\n",
		main_descriptor_index, (device_readable_buf ? device_readable_buf->getLength() : 0) + (device_writable_buf ? device_writable_buf->getLength() : 0), desc_output.next_descriptor_index);
	*/
	
	virtio_virtqueue_add_descriptor_to_ring(queue, main_descriptor_index);
	
	if((queue->used_ring->flags & VirtioVringUsedFlag::NO_NOTIFY)==0)
	{
		pci_device->ioWrite16(VirtioLegacyHeaderOffset::QUEUE_NOTIFY, queue_index, this->pci_virtio_header_iomap);
	}
	return kIOReturnSuccess;
}

static IOReturn generate_indirect_segment_dma(VirtioVirtqueue* queue, IODMACommand* dma_cmd, IOMemoryDescriptor* buf, unsigned& min_descs_required, UInt32& max_segments, virtio_output_indirect_segment_state* desc_output)
{
	IOReturn result = dma_cmd->setMemoryDescriptor(buf, true /* prepare DMA */);
	UInt64 offset = 0;
	--min_descs_required;
	UInt32 gen_segments = max_segments - min_descs_required;
	result = dma_cmd->genIOVMSegments(&offset, desc_output, &gen_segments);
	if (result != kIOReturnSuccess || max_segments < 1 || offset != buf->getLength())
	{
		if (result == kIOReturnSuccess)
		{
			IOLog("VirtioLegacyPCIDevice: generate_indirect_segment_dma(): emitted %u segments up to offset %llu for buffer with %llu bytes\n", max_segments, offset, buf->getLength());
			result = kIOReturnInternalError;
		}
		dma_cmd->clearMemoryDescriptor();
		return result;
	}
	
	max_segments -= gen_segments;
	return kIOReturnSuccess;
}

static void fill_vring_descriptor(VirtioVringDesc* descriptor, int16_t descriptorIndex, VirtioVringDesc* previousDescriptor, IODMACommand::Segment64 segment, bool device_writable);

bool VirtioLegacyPCIDevice::outputVringDescSegmentForIndirectTable(
	IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex)
{
	virtio_output_segment_for_indirect_descs_state* state = static_cast<virtio_output_segment_for_indirect_descs_state*>(segments);
	VirtioVringDesc* descriptor = &state->queue->descriptor_table[state->main_descriptor_index];
	fill_vring_descriptor(descriptor, state->main_descriptor_index, nullptr, segment, false);
	descriptor->flags = VirtioVringDescFlag::INDIRECT;

	return true;
}

bool VirtioLegacyPCIDevice::outputIndirectVringDescSegment(
	IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex)
{
	virtio_output_indirect_segment_state* state = static_cast<virtio_output_indirect_segment_state*>(segments);
	uint16_t index = state->next_descriptor_index;
	VirtioVringDesc* descriptor = &state->desc_array[index];
	
	fill_vring_descriptor(descriptor, index, index == 0 ? nullptr : &state->desc_array[index - 1], segment, state->writable);
	
	state->next_descriptor_index++;
	
	return true;
}


bool VirtioLegacyPCIDevice::outputVringDescSegment(
	IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex)
{
	virtio_legacy_pci_vring_desc_chain* chain = static_cast<virtio_legacy_pci_vring_desc_chain*>(segments);
	VirtioVirtqueue* queue = chain->queue;
	
	// 1. Claim an unused descriptor from the descriptor table (or use chain->reserved_descriptor_index)
	int16_t descriptorIndex;
	if (chain->reserved_descriptor_index != UINT16_MAX)
	{
		descriptorIndex = chain->reserved_descriptor_index;
		chain->reserved_descriptor_index = UINT16_MAX;
	}
	else
	{
		descriptorIndex = reserveNewDescriptor(queue);
		//IOLog("VirtioLegacyPCIDevice::outputVringDescSegment(): reserved descriptor %d for segment %u\n", descriptorIndex,segmentIndex);
	}
	
	VirtioVringDesc* descriptor = &queue->descriptor_table[descriptorIndex];
	uint16_t previousDescriptorIndex = chain->current_last_descriptor_index;
	VirtioVringDesc* previousDescriptor = previousDescriptorIndex == UINT16_MAX ? nullptr : &queue->descriptor_table[previousDescriptorIndex];
	if (previousDescriptorIndex != UINT16_MAX)
	{
		VirtioBuffer* previousBuffer = &queue->descriptor_buffers[previousDescriptorIndex];
		previousBuffer->next_desc = descriptorIndex;
	}
	
	fill_vring_descriptor(descriptor, descriptorIndex, previousDescriptor, segment, chain->device_writable);

	queue->descriptor_buffers[descriptorIndex].next_desc = -1;
	chain->current_last_descriptor_index = descriptorIndex;
	return true;
}

static void fill_vring_descriptor(VirtioVringDesc* descriptor, int16_t descriptorIndex, VirtioVringDesc* previousDescriptor, IODMACommand::Segment64 segment, bool device_writable)
{
	// 2. fill physical address & length fields in descriptor with values from segment argument
	descriptor->phys_address = segment.fIOVMAddr;
	descriptor->length_bytes = static_cast<uint32_t>(segment.fLength);
	
	// set flags to 0 or WRITE depending on chain->device_writable
	if (device_writable)
	{
		descriptor->flags = VirtioVringDescFlag::DEVICE_WRITABLE;
	}
	else
	{
		descriptor->flags = 0;
	}
	// 3. Check if this is the first segment
	if (previousDescriptor != nullptr)
	{
		// update previous descriptor's next field with current descriptor index, and set "next" flag
		previousDescriptor->next = descriptorIndex;
		previousDescriptor->flags |= VirtioVringDescFlag::NEXT;
	}

	// 5. Save index of current descriptor as last descriptor
	descriptor->next = 0xffff;
}

unsigned VirtioLegacyPCIDevice::pollCompletedRequestsInVirtqueue(uint16_t queue_index, unsigned completion_limit)
{
	return this->processCompletedRequestsInVirtqueue(&this->virtqueues[queue_index].queue, completion_limit);
}


unsigned VirtioLegacyPCIDevice::processCompletedRequestsInVirtqueue(VirtioVirtqueue* virtqueue, unsigned completion_limit)
{
	unsigned total_handled = 0;
	const unsigned queue_len = virtqueue->num_entries;
	while (true)
	{
		uint16_t currentUsedRingHeadIndex = virtqueue->used_ring->head_index;
		uint16_t nextUsedRingIndex = virtqueue->used_ring_last_head_index;
		uint16_t numAdded = currentUsedRingHeadIndex - virtqueue->used_ring_last_head_index;
		if(numAdded == 0 || (completion_limit != 0 && total_handled >= completion_limit))
		{
			if (virtqueue->interrupts_requested)
				virtqueue->available_ring->flags = 0; // clear NO_INTERRUPT
			OSSynchronizeIO();
			currentUsedRingHeadIndex = virtqueue->used_ring->head_index;
			numAdded = currentUsedRingHeadIndex - virtqueue->used_ring_last_head_index;
			if (numAdded == 0 || (completion_limit != 0 && total_handled >= completion_limit))
				return total_handled;
		}
		for( ; nextUsedRingIndex != currentUsedRingHeadIndex && (completion_limit == 0 || total_handled < completion_limit); nextUsedRingIndex++)
		{
			unsigned item = nextUsedRingIndex % queue_len;
			uint32_t writtenBytes = virtqueue->used_ring->ring[item].written_bytes;
			uint32_t dequeuedDescriptor = virtqueue->used_ring->ring[item].descriptor_id;

			VirtioCompletion completion = virtqueue->descriptor_buffers[dequeuedDescriptor].completion;
			int16_t descriptorIndex = static_cast<uint16_t>(dequeuedDescriptor);
			while (descriptorIndex >= 0)
			{
				int16_t next = virtqueue->descriptor_buffers[descriptorIndex].next_desc;
				VirtioBuffer* buffer = &virtqueue->descriptor_buffers[descriptorIndex];
				if (buffer->dma_cmd_used)
				{
					buffer->dma_cmd->clearMemoryDescriptor(true);
					buffer->dma_cmd_used = false;
					if (virtqueue->indirect_descriptors)
					{
						buffer->dma_cmd_2->clearMemoryDescriptor(true);
						buffer->dma_indirect_descriptors->clearMemoryDescriptor(true);
						//kprintf("Completed request on descriptor %u\n", descriptorIndex);
					}
				}
				//IOLog("VirtioLegacyPCIDevice::processCompletedRequestsInVirtqueue(): returning descriptor %d to unused list\n", descriptorIndex);
				returnUnusedDescriptor(virtqueue, descriptorIndex);
				descriptorIndex = next;
			}
			completion.action(completion.target, completion.ref, false, writtenBytes);
		}
		virtqueue->used_ring_last_head_index = nextUsedRingIndex;
	}
}

void VirtioLegacyPCIDevice::closePCIDevice()
{

	this->endHandlingInterrupts();
	if (this->pci_device != nullptr)
	{
		this->pci_device->close(this);
		this->pci_device = nullptr;
	}
}

void VirtioLegacyPCIDevice::stop(IOService* provider)
{
	VLTLog("VirtioLegacyPCIDevice[%p]::stop() (provider = %p)\n", this, provider);
	closePCIDevice();
	
	VirtioDevice::stop(provider);
	VLTLog("VirtioLegacyPCIDevice::stop(): done\n");
}

IOWorkLoop* VirtioLegacyPCIDevice::getWorkLoop() const
{
	return this->work_loop ?: (this->pci_device ? this->pci_device->getWorkLoop() : nullptr) ;
}


bool VirtioLegacyPCIDevice::beginHandlingInterrupts(IOWorkLoop* workloop)
{
	//IOLog("VirtioLegacyPCIDevice::beginHandlingInterrupts()\n");
	if (!this->pci_device)
	{
		IOLog("VirtioLegacyPCIDevice beginHandlingInterrupts(): Error! PCI device must be known for generating interrupts.\n");
		return false;
	}
	
	// Message signaled interrupts (MSI) are more efficient than the normal broadcast ones, so let's try to use them
	int msi_start_index = -1;
	int msi_last_index = -1;
	int legacy_index = -1;
	int intr_index = 0;
	
	// keep trying interrupt source indices until we run out or find an MSI one
	while (intr_index >= 0)
	{
		int intr_type = 0;
		IOReturn ret = this->pci_device->getInterruptType(intr_index, &intr_type);
		if (ret != kIOReturnSuccess)
			break;
			
		if (intr_type & kIOInterruptTypePCIMessaged)
		{
			// found MSI interrupt source
			if (msi_start_index < 0)
				msi_start_index = intr_index;
			msi_last_index = intr_index;
		}
		else
		{
			legacy_index = intr_index;
		}
		++intr_index;
	}
	
	if (msi_start_index >= 0)
	{
		intr_index = msi_start_index;
		kprintf("VirtioLegacyPCIDevice beginHandlingInterrupts(): Enabled message signaled interrupts (start index %d, last %d: %u total vectors).\n", msi_start_index, msi_last_index, msi_last_index - msi_start_index + 1);
	}
	else
	{
		intr_index = 0;
	}

	IOByteCount msix_cap_offset = 0;
	if (msi_start_index >= 0)
	{
		// MSI or MSI-X detected
		uint32_t msix_cap_val = this->pci_device->extendedFindPCICapability(kIOPCIMSIXCapability, &msix_cap_offset);
		
		if (msix_cap_offset != 0)
		{
			kprintf("VirtioLegacyPCIDevice beginHandlingInterrupts(): MSI-X detected, capability offset %llu\n", msix_cap_offset);
			/*
			// Device supports MSI-X. IOPCIFamily seems to have problems with this, so revert to legacy interrupts
			intr_index = legacy_index;
			kprintf("VirtioLegacyPCIDevice beginHandlingInterrupts(): Reverted from message signaled interrupts (index %d) to legacy due to problems with MSI-X.\n", intr_index);
			*/
		}
	}
	
	__asm__("int $3");
	
	this->intr_event_source = IOFilterInterruptEventSource::filterInterruptEventSource(
		this, &interruptAction, &interruptFilter, this->pci_device, intr_index);
	if (!intr_event_source)
	{
		IOLog("VirtioLegacyPCIDevice beginHandlingInterrupts(): Error! %s interrupt event source with index %u failed.\n", intr_event_source ? "Initialising" : "Allocating", msi_last_index);
		kprintf("VirtioLegacyPCIDevice beginHandlingInterrupts(): Error! %s interrupt event source with index %u failed.\n", intr_event_source ? "Initialising" : "Allocating", msi_last_index);
		OSSafeReleaseNULL(intr_event_source);
		return false;
	}

	{
		++intr_index;
		auto temp_event_source = IOFilterInterruptEventSource::filterInterruptEventSource(this, &interruptAction, &interruptFilter, this->pci_device, intr_index);
		if (temp_event_source == nullptr)
		{
			kprintf("VirtioLegacyPCIDevice beginHandlingInterrupts(): Error! Failed to create second interrupt source with index %u.\n", msi_start_index);
		}
	}
	
	if (msi_start_index >= 0 && msix_cap_offset > 0)
	{
		// check if MSI-X is enabled on device. If so, shift the configuration area
		uint16_t msix_control = this->pci_device->configRead16(msix_cap_offset + 2);
		this->msix_active = (msix_control & 0x8000) != 0;
		if (this->msix_active)
		{
			kprintf("VirtioLegacyPCIDevice[%p] beginHandlingInterrupts(): MSI-X appears to be active\n", this);
			this->deviceSpecificConfigStartHeaderOffset = VirtioLegacyHeaderOffset::MSIX_END_HEADER;
			
			// Attempt to use vector 0 for both config and queue events
			this->pci_device->ioWrite16(VirtioLegacyHeaderOffset::MSIX_CONFIG_VECTOR, 0, this->pci_virtio_header_iomap);
			uint16_t msix_vector = this->pci_device->ioRead16(VirtioLegacyHeaderOffset::MSIX_CONFIG_VECTOR, this->pci_virtio_header_iomap);
			kprintf("VirtioLegacyPCIDevice[%p] beginHandlingInterrupts(): config MSI-X vector read-back: %4x\n", this, msix_vector);
			for (uint16_t queue_id = 0; queue_id < this->num_virtqueues; ++queue_id)
			{
				this->pci_device->ioWrite16(VirtioLegacyHeaderOffset::QUEUE_SELECT, queue_id, this->pci_virtio_header_iomap);
				this->pci_device->ioWrite16(VirtioLegacyHeaderOffset::MSIX_QUEUE_VECTOR, 0, this->pci_virtio_header_iomap);
				msix_vector = this->pci_device->ioRead16(VirtioLegacyHeaderOffset::MSIX_QUEUE_VECTOR, this->pci_virtio_header_iomap);
				kprintf("VirtioLegacyPCIDevice[%p] beginHandlingInterrupts(): queue %u MSI-X vector read-back: %4x\n", this, queue_id, msix_vector);
			}
		}
	}
	
	assert(this->work_loop == nullptr);
	if (workloop == nullptr)
	{
		workloop = this->pci_device->getWorkLoop();
	}
	if (workloop == nullptr)
	{
		workloop = IOWorkLoop::workLoop();
	}
	else
	{
		workloop->retain();
	}
	this->work_loop = workloop;
	if (!this->work_loop)
		return false;
	
	if (kIOReturnSuccess != this->work_loop->addEventSource(intr_event_source))
	{
		IOLog("VirtioLegacyPCIDevice beginHandlingInterrupts(): Error! Adding interrupt event source to work loop failed.\n");
		OSSafeReleaseNULL(intr_event_source);
		return false;
	}
	intr_event_source->enable();
	//IOLog("VirtioLegacyPCIDevice beginHandlingInterrupts(): now handling interrupts, good to go.\n");
	return true;
}



bool VirtioLegacyPCIDevice::interruptFilter(OSObject* me, IOFilterInterruptEventSource* source)
{
	// deliberately minimalistic function, as it will be called from an interrupt
	VirtioLegacyPCIDevice* virtio_pci = OSDynamicCast(VirtioLegacyPCIDevice, me);
	if (!virtio_pci || source != virtio_pci->intr_event_source)
		return false; // this isn't really for us

	kprintf("VirtioLegacyPCIDevice::interruptFilter\n");
	// check if anything interesting has happened, record status register
	
	uint8_t isr = virtio_pci->pci_device->ioRead8(VirtioLegacyHeaderOffset::ISR_STATUS, virtio_pci->pci_virtio_header_iomap);
	//virtio_pci->last_isr = isr;
	if (isr & VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE)
	{
		OSTestAndSet(0, &virtio_pci->received_config_change);
		return true;
	}
	if (isr & VIRTIO_PCI_DEVICE_ISR_USED)
	{
		// disable further virtqueue interrupts until the handler has run?
		for (unsigned i = 0; i < virtio_pci->num_virtqueues; ++i)
		{
			virtio_pci->virtqueues[i].queue.available_ring->flags = VirtioVringAvailFlag::NO_INTERRUPT;
		}
		return true;
	}
	return false;
}

bool VirtioLegacyPCIDevice::didTerminate( IOService * provider, IOOptionBits options, bool * defer )
{
	VLTLog("VirtioLegacyPCIDevice[%p]::didTerminate() provider = %p, options = %x, defer = %s [%p]\n", this, provider, options, defer ? ((*defer) ? "true" : "false") : "NULL", defer);
	closePCIDevice();
	
	bool res = IOService::didTerminate(provider, options, defer);
	VLTLog("VirtioLegacyPCIDevice[%p]::didTerminate() done: %s, defer = %s [%p]\n", this, res ? "true" : "false", defer ? ((*defer) ? "true" : "false") : "NULL", defer);
	return res;
}


void VirtioLegacyPCIDevice::interruptAction(OSObject* me, IOInterruptEventSource* source, int count)
{
	VirtioLegacyPCIDevice* virtio_pci = OSDynamicCast(VirtioLegacyPCIDevice, me);
	if (!virtio_pci || source != virtio_pci->intr_event_source)
		return;
	
	virtio_pci->interruptAction(source, count);
}

void VirtioLegacyPCIDevice::interruptAction(IOInterruptEventSource* source, int count)
{
	if (this->received_config_change)
	{
		this->received_config_change = 0;
		if (this->configChangeAction != nullptr)
		{
			this->configChangeAction(this->configChangeTarget, this);
		}
	}
	
	for(unsigned i = 0; i < this->num_virtqueues; i++)
	{
		this->processCompletedRequestsInVirtqueue(&this->virtqueues[i].queue, 0 /* no limit */);
	}
}

bool VirtioLegacyPCIDevice::endHandlingInterrupts()
{
	if(this->intr_event_source)
	{
		this->intr_event_source->disable();
		this->work_loop->removeEventSource(this->intr_event_source);
		OSSafeReleaseNULL(this->intr_event_source);
	}
	OSSafeReleaseNULL(this->work_loop);
	return true;
}


// DON'T ADD ANYTHING BELOW THIS

#ifdef VIRTIO_LOG_TERMINATION

bool VirtioLegacyPCIDevice::terminateClient(IOService * client, IOOptionBits options)
{
	IOLog("VirtioLegacyPCIDevice[%p]::terminateClient() client = %p, options = %x\n", this, client, options);
	bool res = IOService::terminateClient(client, options);
	IOLog("VirtioLegacyPCIDevice[%p]::terminateClient() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioLegacyPCIDevice::requestTerminate( IOService * provider, IOOptionBits options )
{
	IOLog("VirtioLegacyPCIDevice[%p]::requestTerminate() provider = %p, options = %x\n", this, provider, options);
	bool res = IOService::requestTerminate(provider, options);
	IOLog("VirtioLegacyPCIDevice[%p]::requestTerminate() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioLegacyPCIDevice::willTerminate( IOService * provider, IOOptionBits options )
{
	IOLog("VirtioLegacyPCIDevice[%p]::willTerminate() provider = %p, options = %x\n", this, provider, options);
	bool res = IOService::willTerminate(provider, options);
	IOLog("VirtioLegacyPCIDevice[%p]::willTerminate() done: %s\n", this, res ? "true" : "false");
	return res;
}


bool VirtioLegacyPCIDevice::terminate( IOOptionBits options )
{
	IOLog("VirtioLegacyPCIDevice[%p]::terminate() options = %x\n", this, options);
	bool res = IOService::terminate(options);
	IOLog("VirtioLegacyPCIDevice[%p]::terminate() done: %s\n", this, res ? "true" : "false");
	return res;
}

#endif // ifdef VIRTIO_LOG_TERMINATION