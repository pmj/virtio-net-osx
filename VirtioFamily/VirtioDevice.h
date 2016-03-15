//
//  VirtioDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 20/03/2015.
//
//

#ifndef __virtio_osx__VirtioDevice__
#define __virtio_osx__VirtioDevice__

#include <IOKit/IOService.h>

#ifdef VIRTIO_LOG_TERMINATION
#define VLTLog IOLog
#else
#define VLTLog(...) ({})
#endif


#define VirtioDevice eu_dennis__jordan_driver_VirtioDevice

struct VirtioCompletion;
struct VirtioVirtqueue;
struct VirtioBuffer;
class IOBufferMemoryDescriptor;

class VirtioDevice : public IOService
{
	OSDeclareAbstractStructors(VirtioDevice);
protected:
	uint32_t virtio_device_type;
	
public:
	virtual bool matchPropertyTable(OSDictionary* table, SInt32* score) override;

	virtual bool resetDevice() = 0;
	virtual uint32_t supportedFeatures() = 0;
	virtual bool requestFeatures(uint32_t use_features) = 0;
	virtual void failDevice() = 0;

	virtual IOReturn setupVirtqueues(uint16_t number_queues, const bool queue_interrupts_enabled[] = nullptr, unsigned out_queue_sizes[] = nullptr, const unsigned indirect_desc_per_request[] = nullptr) = 0;
	virtual IOReturn setVirtqueueInterruptsEnabled(uint16_t queue_id, bool enabled) = 0;
	
	typedef void(*ConfigChangeAction)(OSObject* target, VirtioDevice* source);
	virtual void startDevice(ConfigChangeAction action = nullptr, OSObject* target = nullptr, IOWorkLoop* workloop = nullptr) = 0;
	
	virtual IOReturn submitBuffersToVirtqueue(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion) = 0;
	virtual unsigned pollCompletedRequestsInVirtqueue(uint16_t queue_index, unsigned completion_limit = 0) = 0;
	
	virtual uint8_t readDeviceConfig8(uint16_t offset) = 0;

	virtual uint16_t readDeviceConfig16Native(uint16_t offset) = 0;
	virtual uint32_t readDeviceConfig32Native(uint16_t offset) = 0;
	virtual uint64_t readDeviceConfig64Native(uint16_t offset) = 0;

	// deliberately non-virtual so they compile away to native call on LE arches.
	uint16_t readDeviceConfig16LE(uint16_t offset)
	{ return OSSwapLittleToHostInt16(this->readDeviceConfig16Native(offset)); };
	uint32_t readDeviceConfig32LE(uint16_t offset)
	{ return OSSwapLittleToHostInt32(this->readDeviceConfig32Native(offset)); };
	uint64_t readDeviceConfig64LE(uint16_t offset)
	{ return OSSwapLittleToHostInt64(this->readDeviceConfig64Native(offset)); };

	// Read fields which vary in endianness in legacy vs standardised devices.
	virtual uint16_t readDeviceConfig16LETransitional(uint16_t offset) = 0;
	virtual uint32_t readDeviceConfig32LETransitional(uint16_t offset) = 0;
	virtual uint64_t readDeviceConfig64LETransitional(uint16_t offset) = 0;

	virtual void writeDeviceConfig8(uint16_t offset, uint8_t value_to_write) = 0;

	virtual void writeDeviceConfig16Native(uint16_t offset, uint16_t value_to_write) = 0;
	virtual void writeDeviceConfig32Native(uint16_t offset, uint32_t value_to_write) = 0;

	// deliberately non-virtual so they compile away to native call on LE arches.
	void writeDeviceConfig16LE(uint16_t offset, uint16_t value)
	{ this->writeDeviceConfig16Native(offset, OSSwapHostToLittleInt16(value)); };
	void writeDeviceConfig32LE(uint16_t offset, uint32_t value)
	{ this->writeDeviceConfig32Native(offset, OSSwapHostToLittleInt32(value)); };

	virtual void writeDeviceConfig16LETransitional(uint16_t offset, uint16_t value_to_write) = 0;
	virtual void writeDeviceConfig32LETransitional(uint16_t offset, uint32_t value_to_write) = 0;

	uint32_t getVirtioDeviceType() { return virtio_device_type; }

private:
	OSMetaClassDeclareReservedUnused(VirtioDevice, 0);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 1);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 2);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 3);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 4);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 5);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 6);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 7);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 8);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 9);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 10);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 11);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 12);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 13);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 14);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 15);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 16);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 17);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 18);
	OSMetaClassDeclareReservedUnused(VirtioDevice, 19);

};

class IODMACommand;

typedef void(*VirtioCompletionAction)(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
struct VirtioCompletion
{
	VirtioCompletionAction action;
	OSObject* target;
	void* ref;
};

struct VirtioBuffer
{
	/// Pre-allocated DMA command.
	/** Only up to 2 DMA commands in a chain will be used, dma_cmd_used will
	 * indicate which ones need cleaning up after completion. */
	IODMACommand* dma_cmd;
	/// Mechanism for notifying the client that submitted the request.
	/** Only the completion of the first descriptor in the chain for a request is used. */
	VirtioCompletion completion;
	/// Next descriptor used in the chain. -1 to indicate last descriptor in chain.
	/** Also used for maintaining the list of unused descriptors. */
	int16_t next_desc;
	bool dma_cmd_used;
	IOBufferMemoryDescriptor* indirect_descriptors;
	IODMACommand* dma_indirect_descriptors;
	IODMACommand* dma_cmd_2;
};

struct VirtioVirtqueue
{
	unsigned num_entries;

	struct VirtioVringDesc* descriptor_table;
	struct VirtioVringAvail* available_ring;
	/* Position in used_ring after which the device should send an interrupt. */
	uint16_t* used_ring_interrupt_index;
	struct VirtioVringUsed* used_ring;
	/* Position in avail_ring, after which the driver should notify the device
	 * of insertions. (if available_ring->index becomes greater than this, post
	 * a notification) */
	uint16_t* avail_ring_notify_index;
	
	/// Value of used_ring->head_index last time the used ring was checked for activity.
	uint16_t used_ring_last_head_index;
	
	VirtioBuffer* descriptor_buffers;

	/// Whether or not the client driver would like interrupts on request completion
	bool interrupts_requested;
	
	bool indirect_descriptors;

	/// If >= 0, an unused descriptor table entry, with all others chained along next_desc
	int16_t first_unused_descriptor_index;
	unsigned num_unused_descriptors;
};

namespace VirtioVringDescFlag
{
	enum VirtioVringDescFlags
	{
		NEXT = 1,
		DEVICE_WRITABLE = 2,
		INDIRECT = 4
	};
}

struct VirtioVringDesc
{
	uint64_t phys_address;
	uint32_t length_bytes;
	uint16_t flags;
	uint16_t next;
};
struct VirtioVringAvail
{
	uint16_t flags;
	uint16_t head_index;
	uint16_t ring[];
};
namespace VirtioVringAvailFlag
{
	enum VirtioVringAvailFlags : uint16_t
	{
		NO_INTERRUPT = 1
	};
}

struct VirtioVringUsedElement
{
	uint32_t descriptor_id;
	uint32_t written_bytes;
};

namespace VirtioVringUsedFlag
{
	enum VirtioVringUsedFlags
	{
		NO_NOTIFY = 1
	};
}
struct VirtioVringUsed
{
	uint16_t flags;
	uint16_t head_index;
	VirtioVringUsedElement ring[];
};

namespace VirtioDeviceGenericFeature
{
  enum VirtioDeviceGenericFeatures
	{
		VIRTIO_F_RING_EVENT_IDX = (1u << 29u),
		VIRTIO_F_RING_INDIRECT_DESC = (1u << 28u),
	};
}

#endif /* defined(__virtio_osx__VirtioDevice__) */
