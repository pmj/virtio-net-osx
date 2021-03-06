//
//  VirtioLegacyPCIDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#ifndef __virtio_osx__VirtioLegacyPCIDevice__
#define __virtio_osx__VirtioLegacyPCIDevice__

#include <IOKit/IODMACommand.h>
#include "VirtioDevice.h"
#include <IOKit/IOFilterInterruptEventSource.h>

#define VirtioLegacyPCIDevice eu_dennis__jordan_driver_VirtioLegacyPCIDevice
class IOPCIDevice;
struct VirtioLegacyPCIVirtqueue;
class VirtioLegacyPCIDevice : public VirtioDevice
{
	OSDeclareDefaultStructors(VirtioLegacyPCIDevice);
protected:
	IOMemoryMap* pci_virtio_header_iomap;
	IOPCIDevice* pci_device;
	bool msix_active;
	uint32_t features;
	uint32_t active_features;
	
	struct VirtioLegacyPCIVirtqueue* virtqueues;
	unsigned num_virtqueues;
	bool eventIndexFeatureEnabled;
	uint16_t deviceSpecificConfigStartHeaderOffset;
	ConfigChangeAction configChangeAction;
	OSObject* configChangeTarget;
	IOFilterInterruptEventSource* intr_event_source;
	IOWorkLoop* work_loop;
	volatile UInt8 received_config_change __attribute__((aligned(32)));

public:
	virtual IOService* probe(IOService* provider, SInt32* score) override;
	virtual bool start(IOService* provider) override;

	virtual bool handleOpen(IOService* forClient, IOOptionBits options, void* arg) override;
	virtual void handleClose(IOService* forClient, IOOptionBits options) override;
	
	virtual bool resetDevice() override;
	virtual uint32_t supportedFeatures() override;
	virtual bool requestFeatures(uint32_t use_features) override;
	virtual void failDevice() override;
	virtual IOReturn setupVirtqueues(uint16_t number_queues, const bool queue_interrupts_enabled[] = nullptr, unsigned out_queue_sizes[] = nullptr, const unsigned indirect_desc_per_request[] = nullptr) override;
	virtual IOReturn setVirtqueueInterruptsEnabled(uint16_t queue_id, bool enabled) override;
	virtual void startDevice(ConfigChangeAction action = nullptr, OSObject* target = nullptr, IOWorkLoop* workloop = nullptr) override;
	
	virtual void closePCIDevice();

	virtual IOReturn submitBuffersToVirtqueue(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion) override;
	unsigned processCompletedRequestsInVirtqueue(VirtioVirtqueue* virtqueue, unsigned completion_limit);
	virtual unsigned pollCompletedRequestsInVirtqueue(uint16_t queue_index, unsigned completion_limit = 0) override;
	
	virtual uint8_t readDeviceConfig8(uint16_t device_specific_offset) override;

	virtual uint16_t readDeviceConfig16LETransitional(uint16_t device_specific_offset) override;
	virtual uint32_t readDeviceConfig32LETransitional(uint16_t device_specific_offset) override;
	virtual uint64_t readDeviceConfig64LETransitional(uint16_t device_specific_offset) override;

	virtual uint16_t readDeviceConfig16Native(uint16_t device_specific_offset) override;
	virtual uint32_t readDeviceConfig32Native(uint16_t device_specific_offset) override;
	virtual uint64_t readDeviceConfig64Native(uint16_t device_specific_offset) override;

	virtual void writeDeviceConfig8(uint16_t offset, uint8_t value_to_write) override;

	virtual void writeDeviceConfig16Native(uint16_t offset, uint16_t value_to_write) override;
	virtual void writeDeviceConfig32Native(uint16_t offset, uint32_t value_to_write) override;

	virtual void writeDeviceConfig16LETransitional(uint16_t device_specific_offset, uint16_t value_to_write) override;
	virtual void writeDeviceConfig32LETransitional(uint16_t device_specific_offset, uint32_t value_to_write) override;

	virtual void stop(IOService* provider) override;
	virtual bool didTerminate(IOService* provider, IOOptionBits options, bool* defer) override;

#ifdef VIRTIO_LOG_TERMINATION
	virtual bool requestTerminate(IOService * provider, IOOptionBits options) override;
	virtual bool willTerminate(IOService * provider, IOOptionBits options) override;
	virtual bool terminate(IOOptionBits options = 0) override;
	virtual bool terminateClient(IOService * client, IOOptionBits options) override;
#endif

	virtual bool beginHandlingInterrupts(IOWorkLoop* workloop);
	static void interruptAction(OSObject* me, IOInterruptEventSource* source, int count);
	static bool interruptFilter(OSObject* me, IOFilterInterruptEventSource* source);
	virtual void interruptAction(IOInterruptEventSource* source, int count);
	virtual bool endHandlingInterrupts();

	virtual IOWorkLoop* getWorkLoop() const override;
private:
	IOReturn setupVirtqueue(VirtioLegacyPCIVirtqueue* queue, uint16_t queue_id, bool interrupts_enabled, unsigned indirect_desc_per_request);
	
	
	bool mapHeaderIORegion();
	
	static bool outputVringDescSegment(
		IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex);
	static bool outputVringDescSegmentForIndirectTable(
		IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex);
	static bool outputIndirectVringDescSegment(
		IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex);

	IOReturn submitBuffersToVirtqueueDirect(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion);
	IOReturn submitBuffersToVirtqueueIndirect(uint16_t queue_index, IOMemoryDescriptor* device_readable_buf, IOMemoryDescriptor* device_writable_buf, VirtioCompletion completion);

};


#endif /* defined(__virtio_osx__VirtioLegacyPCIDevice__) */
