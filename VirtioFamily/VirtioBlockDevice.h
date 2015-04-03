//
//  VirtioBlockDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 30/03/2015.
//
//

#ifndef __virtio_osx__VirtioBlockDevice__
#define __virtio_osx__VirtioBlockDevice__

#include "VirtioDevice.h"
#include "../lib/genccont/src/slist_queue.h"
#include <IOKit/storage/IOBlockStorageDevice.h>
#define VirtioBlockDevice eu_dennis__jordan_driver_VirtioBlockDevice

class IOCommandGate;

struct VirtioBlockDeviceRequest;

class IOBufferMemoryDescriptor;
class VirtioBlockDevice : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(VirtioBlockDevice);
protected:
	VirtioDevice* virtio_device;
	IOCommandGate* command_gate;

	static const unsigned CONFIG_CAPCITY_OFFSET = 0;
	static const unsigned CONFIG_SIZE_MAX_OFFSET = 8;
	static const unsigned CONFIG_SEG_MAX_OFFSET = 12;
	static const unsigned CONFIG_BLK_SIZE_OFFSET = 20;
	uint32_t block_size;
	uint32_t active_features;
	uint64_t capacity_in_bytes;
	uint32_t sectors_per_block;
	uint32_t max_request_segments;
	unsigned max_concurrent_requests;
	
	VirtioBlockDeviceRequest* requestFromPool();
	void returnRequestToPool(VirtioBlockDeviceRequest* request);
	
	// Requests received that will not fit into the device's virtqueue
	genc_slist_queue_t pending_requests;
	
	// Pool of unused request data structures
	genc_slist_head_t* request_pool;

	static IOReturn doAsyncReadWriteOnWorkLoop(OSObject* block_dev, void* arg0, void* arg1, void* arg2, void* arg3);
	IOReturn doAsyncReadWriteOnWorkLoop(IOMemoryDescriptor* buffer, UInt64 block, UInt64 nblks, IOStorageAttributes* attributes, IOStorageCompletion* completion);
	
	static IOReturn doSynchronizeCacheOnWorkLoop(OSObject* block_dev, void* arg0, void* arg1, void* arg2, void* arg3);
	IOReturn doSynchronizeCacheOnWorkLoop();
	void handlePendingRequests(bool device_reset);
public:
	virtual bool start(IOService* provider) override;
	virtual void stop(IOService* provider) override;
	static void deviceConfigChangeAction(OSObject* target, VirtioDevice* source);
	virtual void deviceConfigChangeAction(VirtioDevice* source);
	virtual void endDeviceOperation();
    virtual bool didTerminate(IOService* provider, IOOptionBits options, bool* defer ) override;
	
	virtual bool handleOpen(IOService* forClient, IOOptionBits options, void* arg) override;

	virtual IOReturn doEjectMedia(void) override;
	virtual IOReturn doFormatMedia(UInt64 byteCapacity) override;
	virtual UInt32 doGetFormatCapacities(UInt64* capacities, UInt32 capacitiesMaxCount) const override;
	virtual IOReturn doLockUnlockMedia(bool doLock) override;
	virtual IOReturn doSynchronizeCache(void) override;
	virtual char* getVendorString(void) override;
	virtual char* getProductString(void) override;
	virtual char* getRevisionString(void) override;
	virtual char* getAdditionalDeviceInfoString(void) override;
	virtual IOReturn reportBlockSize(UInt64* blockSize) override;
	virtual IOReturn reportEjectability(bool* isEjectable) override;
	virtual IOReturn reportLockability(bool* isLockable) override;
	virtual IOReturn reportMaxValidBlock(UInt64* maxBlock) override;
	virtual IOReturn reportMediaState(bool * mediaPresent, bool * changedState) override;
	virtual IOReturn reportPollRequirements(bool* pollRequired, bool* pollIsExpensive) override;
	virtual IOReturn reportRemovability(bool* isRemovable) override;
	virtual IOReturn reportWriteProtection(bool* isWriteProtected) override;
	virtual IOReturn getWriteCacheState(bool* enabled) override;
	virtual IOReturn setWriteCacheState(bool enabled) override;
	virtual IOReturn doAsyncReadWrite(IOMemoryDescriptor* buffer, UInt64 block, UInt64 nblks, IOStorageAttributes* attributes, IOStorageCompletion* completion) override;
	
	static void blockRequestCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
	virtual void blockRequestCompleted(VirtioBlockDeviceRequest* request, bool device_reset);
	static void flushRequestCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
	virtual void flushRequestCompleted(VirtioBlockDeviceRequest* request, bool device_reset);

#ifdef VIRTIO_LOG_TERMINATION
	virtual bool requestTerminate( IOBlockStorageDevice * provider, IOOptionBits options ) override;
    virtual bool willTerminate( IOBlockStorageDevice * provider, IOOptionBits options ) override;
	virtual bool terminate( IOOptionBits options = 0 ) override;
	virtual bool terminateClient(IOBlockStorageDevice * client, IOOptionBits options) override;
#endif

};
#endif /* defined(__virtio_osx__VirtioBlockDevice__) */
