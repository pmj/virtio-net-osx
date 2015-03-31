//
//  VirtioLegacyPCIDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#include "VirtioLegacyPCIDevice.h"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>

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
		END_HEADER = 1 + ISR_STATUS
	};
}
namespace
{
	const size_t VIRTIO_LEGACY_HEADER_MIN_LEN = VirtioLegacyHeaderOffset::END_HEADER;
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
	IOLog("VirtioLegacyPCIDevice::probe()\n");
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
	IOLog("VirtioLegacyPCIDevice::start()\n");
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
	
	this->deviceSpecificConfigStartHeaderOffset = VirtioLegacyHeaderOffset::END_HEADER;
	
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
	IOLog("writeDeviceStatusField mapConfigurationSpace(): Mapped %llu bytes of device memory at %llX. (physical address %llX)\n",
		static_cast<uint64_t>(iomap->getLength()), iomap->getAddress(), this->pci_device->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)->getPhysicalSegment(0, NULL, 0));
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

IOReturn VirtioLegacyPCIDevice::setupVirtqueue(VirtioLegacyPCIVirtqueue* queue, unsigned queue_id, bool interrupts_enabled)
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
	IOLog("VirtioLegacyPCIDevice::setupVirtqueue(): Queue size for queue %u is %u.\n", queue_id, num_queue_entries);

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
	
	// allocate array of VirtioBuffers for descriptor_array
	const size_t desc_buffer_array_size = sizeof(queue->queue.descriptor_buffers[0]) * num_queue_entries;
	VirtioBuffer* descriptor_buffers = static_cast<VirtioBuffer*>(
		IOMallocAligned(desc_buffer_array_size, alignof(decltype(queue->queue.descriptor_buffers[0]))));
	memset(descriptor_buffers, 0, desc_buffer_array_size);
	for (unsigned i = 0; i < num_queue_entries; i++)
	{
		IODMACommand* buffer_dma = descriptor_buffers[i].dma_cmd = IODMACommand::withSpecification(
			outputVringDescSegment, 64, UINT32_MAX, IODMACommand::kMapped, UINT32_MAX);
		if (buffer_dma == nullptr)
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
		descriptor_buffers[i].dma_cmd = buffer_dma;
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
	for(unsigned i = 0; i < num_queue_entries; i++)
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

IOReturn VirtioLegacyPCIDevice::setVirtqueueInterruptsEnabled(unsigned queue_id, bool enabled)
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

IOReturn VirtioLegacyPCIDevice::setupVirtqueues(unsigned number_queues, const bool queue_interrupts_enabled[], unsigned out_queue_sizes[])
{
	const size_t queue_array_size = sizeof(this->virtqueues[0]) * number_queues;
	VirtioLegacyPCIVirtqueue* queues = static_cast<VirtioLegacyPCIVirtqueue*>(
		IOMallocAligned(queue_array_size, alignof(decltype(this->virtqueues[0]))));
	if (queues == nullptr)
		return kIOReturnNoMemory;
	memset(queues, 0, queue_array_size);
	
	IOReturn result = kIOReturnSuccess;
	for (unsigned i = 0; i < number_queues; ++i)
	{
		result = this->setupVirtqueue(&queues[i], i, queue_interrupts_enabled ? queue_interrupts_enabled[i] : true);
		
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

void VirtioLegacyPCIDevice::startDevice(ConfigChangeAction action, OSObject* target)
{
	// save action & target
	this->configChangeAction = action;
	this->configChangeTarget = target;
	
	// enable interrupt handling
	this->beginHandlingInterrupts();


	this->pci_device->ioWrite8(VirtioLegacyHeaderOffset::DEVICE_STATUS, 1|2|4, this->pci_virtio_header_iomap);
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

uint8_t VirtioLegacyPCIDevice::readDeviceSpecificConfig8(unsigned device_specific_offset)
{
	return this->pci_device->ioRead8(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
}

uint32_t VirtioLegacyPCIDevice::readDeviceSpecificConfig32LE(unsigned device_specific_offset)
{
	uint32_t val = this->pci_device->ioRead32(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
	return OSSwapLittleToHostInt32(val);
}
uint16_t VirtioLegacyPCIDevice::readDeviceSpecificConfig16Native(unsigned device_specific_offset)
{
	return this->pci_device->ioRead16(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, this->pci_virtio_header_iomap);
}

void VirtioLegacyPCIDevice::writeDeviceSpecificConfig32LE(unsigned device_specific_offset, uint32_t value_to_write)
{
	uint32_t le_value = OSSwapHostToLittleInt32(value_to_write);
	this->pci_device->ioWrite32(this->deviceSpecificConfigStartHeaderOffset + device_specific_offset, le_value, this->pci_virtio_header_iomap);
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

IOReturn VirtioLegacyPCIDevice::submitBuffersToVirtqueue(unsigned queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion)
{
	if(queue_index >= this->num_virtqueues)
	{
		return kIOReturnBadArgument;
	}
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
		IOLog("VirtioLegacyPCIDevice::submitBuffersToVirtqueue(): reserved descriptor %d as first device_readable\n", descriptorIndex);
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
		IOLog("VirtioLegacyPCIDevice::submitBuffersToVirtqueue(): reserved descriptor %d as first device writable\n", descriptorIndex);
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
	
	// add index of first descriptor in chain to 'available' ring
	
	uint16_t avail_pos = queue->available_ring->head_index;
	queue->available_ring->ring[avail_pos % queue->num_entries] = first_descriptor_index;
	avail_pos++;
	OSSynchronizeIO();
	queue->available_ring->head_index = avail_pos;
	OSSynchronizeIO();
	if((queue->used_ring->flags & VirtioVringUsedFlag::NO_NOTIFY)==0)
	{
		this->pci_device->ioWrite16(VirtioLegacyHeaderOffset::QUEUE_NOTIFY, queue_index, this->pci_virtio_header_iomap);
	}
	return kIOReturnSuccess;
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
		IOLog("VirtioLegacyPCIDevice::outputVringDescSegment(): reserved descriptor %d for segment %u\n", descriptorIndex,segmentIndex);
	}
	
	VirtioVringDesc* descriptor = &queue->descriptor_table[descriptorIndex];
	
	// 2. fill physical address & length fields in descriptor with values from segment argument
	descriptor->phys_address = segment.fIOVMAddr;
	descriptor->length_bytes = static_cast<uint32_t>(segment.fLength);
	
	// set flags to 0 or WRITE depending on chain->device_writable
	if(chain->device_writable)
	{
		descriptor->flags = VirtioVringDescFlag::DEVICE_WRITABLE;
	}
	else
	{
		descriptor->flags = 0;
	}
	// 3. Check if this is the first segment
	if(chain->current_last_descriptor_index != UINT16_MAX)
	{
		// 4a. If not first segment: find previous descriptor, update its next field with current descriptor index, and set "next" flag
		uint16_t previousDescriptorIndex = chain->current_last_descriptor_index;
		VirtioVringDesc* previousDescriptor = &queue->descriptor_table[previousDescriptorIndex];
		previousDescriptor->next = descriptorIndex;
		previousDescriptor->flags |= VirtioVringDescFlag::NEXT;
		VirtioBuffer* previousBuffer = &queue->descriptor_buffers[previousDescriptorIndex];
		previousBuffer->next_desc = descriptorIndex;
	}

	// 5. Save index of current descriptor as last descriptor
	descriptor->next = 0xffff;
	queue->descriptor_buffers[descriptorIndex].next_desc = -1;
	chain->current_last_descriptor_index = descriptorIndex;

	return true;
}

unsigned VirtioLegacyPCIDevice::pollCompletedRequestsInVirtqueue(unsigned queue_index, unsigned completion_limit)
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
		if(numAdded == 0 || total_handled >= completion_limit)
		{
			return total_handled;
		}
		for( ; nextUsedRingIndex != currentUsedRingHeadIndex && total_handled < completion_limit; nextUsedRingIndex++)
		{
			unsigned item = nextUsedRingIndex % queue_len;
			uint32_t writtenBytes = virtqueue->used_ring->ring[item].written_bytes;
			uint32_t dequeuedDescriptor = virtqueue->used_ring->ring[item].descriptor_id;

			VirtioCompletion completion = virtqueue->descriptor_buffers[dequeuedDescriptor].completion;
			int16_t descriptorIndex = dequeuedDescriptor;
			while (descriptorIndex >= 0)
			{
				int16_t next = virtqueue->descriptor_buffers[descriptorIndex].next_desc;
				if (virtqueue->descriptor_buffers[descriptorIndex].dma_cmd_used)
				{
					virtqueue->descriptor_buffers[descriptorIndex].dma_cmd->clearMemoryDescriptor(true);
					virtqueue->descriptor_buffers[descriptorIndex].dma_cmd_used = false;
				}
				IOLog("VirtioLegacyPCIDevice::processCompletedRequestsInVirtqueue(): returning descriptor %d to unused list\n", descriptorIndex);
				returnUnusedDescriptor(virtqueue, descriptorIndex);
				descriptorIndex = next;
			}
			completion.action(completion.target, completion.ref, false, writtenBytes);
		}
		virtqueue->used_ring_last_head_index = nextUsedRingIndex;
		if (virtqueue->interrupts_requested)
			virtqueue->available_ring->flags = 0; // clear NO_INTERRUPT
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

bool VirtioLegacyPCIDevice::beginHandlingInterrupts()
{
	IOLog("virtio-net beginHandlingInterrupts()\n");
	if (!this->pci_device)
	{
		IOLog("virtio-net beginHandlingInterrupts(): Error! PCI device must be known for generating interrupts.\n");
		return false;
	}
	
	// Message signaled interrupts (MSI) are more efficient than the normal broadcast ones, so let's try to use them
	int msi_index = -1;
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
			msi_index = intr_index;
			break;
		}
		++intr_index;
	}
	
	if (msi_index >= 0)
	{
		intr_index = msi_index;
		IOLog("virtio-net beginHandlingInterrupts(): Enabled message signaled interrupts (index %d).\n", intr_index);
	}
	else
	{
		intr_index = 0;
	}

	
	this->intr_event_source = IOFilterInterruptEventSource::filterInterruptEventSource(this, &interruptAction, &interruptFilter, this->pci_device, intr_index);
	if (!intr_event_source)
	{
		IOLog("virtio-net beginHandlingInterrupts(): Error! %s interrupt event source failed.\n", intr_event_source ? "Initialising" : "Allocating");
		OSSafeReleaseNULL(intr_event_source);
		return false;
	}
	this->work_loop = getWorkLoop();
	if (!work_loop)
		return false;
	this->work_loop->retain();
	
	if (kIOReturnSuccess != this->work_loop->addEventSource(intr_event_source))
	{
		IOLog("virtio-net beginHandlingInterrupts(): Error! Adding interrupt event source to work loop failed.\n");
		OSSafeReleaseNULL(intr_event_source);
		return false;
	}
	intr_event_source->enable();
	IOLog("virtio-net beginHandlingInterrupts(): now handling interrupts, good to go.\n");
	return true;
}



bool VirtioLegacyPCIDevice::interruptFilter(OSObject* me, IOFilterInterruptEventSource* source)
{
	// deliberately minimalistic function, as it will be called from an interrupt
	VirtioLegacyPCIDevice* virtio_pci = OSDynamicCast(VirtioLegacyPCIDevice, me);
	if (!virtio_pci || source != virtio_pci->intr_event_source)
		return false; // this isn't really for us
	
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