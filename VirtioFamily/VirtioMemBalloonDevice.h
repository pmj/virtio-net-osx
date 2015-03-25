//
//  VirtioMemBalloonDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 19/03/2015.
//
//

#ifndef __virtio_osx__VirtioMemBalloonDevice__
#define __virtio_osx__VirtioMemBalloonDevice__

#include "VirtioDevice.h"
#define VirtioMemBalloonDevice eu_dennis__jordan_driver_VirtioMemBalloonDevice

class IOBufferMemoryDescriptor;
class VirtioMemBalloonDevice : public IOService
{
	OSDeclareDefaultStructors(VirtioMemBalloonDevice);
protected:
	VirtioDevice* virtio_device;
	OSArray* pageBuffers;
	OSArray* bigChunkBuffers;
	IOCommandGate* command_gate;
	
	static const unsigned BIG_CHUNK_BYTES = 2*1024*1024;
	static const unsigned BIG_CHUNK_PAGES = BIG_CHUNK_BYTES/PAGE_SIZE;
	IOBufferMemoryDescriptor* page_address_array;
	static const unsigned INFLATE_QUEUE_INDEX = 0;
	static const unsigned DEFLATE_QUEUE_INDEX = 1;
	bool inflateDeflateInProgress;
	
	static const unsigned CONFIG_NUM_REQUESTED_PAGES_OFFSET = 0;
	static const unsigned CONFIG_ACTUAL_PAGES_OFFSET = 4;
	
	uint32_t totalPagesAllocated();
	
public:
	virtual bool start(IOService* provider) override;
	virtual void stop(IOService* provider) override;

	virtual void endDeviceOperation();
    virtual bool didTerminate( IOService * provider, IOOptionBits options, bool * defer ) override;
	
	static void deviceConfigChangeAction(OSObject* target, VirtioDevice* source);
	virtual void deviceConfigChangeAction(VirtioDevice* source);
	
	virtual void inflateDeflateIfNecessary(uint32_t num_pages_requested);
	virtual void inflateMemBalloon(uint32_t num_pages_to_inflate_by);
	virtual void deflateMemBalloon(uint32_t num_pages_to_deflate_by);
	
	static void inflateRequestCompleted(OSObject* target, void* ref, bool device_reset);
	virtual void inflateRequestCompleted(bool device_reset);
	
#ifdef VIRTIO_LOG_TERMINATION
	virtual bool requestTerminate( IOService * provider, IOOptionBits options ) override;
    virtual bool willTerminate( IOService * provider, IOOptionBits options ) override;
	virtual bool terminate( IOOptionBits options = 0 ) override;
	virtual bool terminateClient(IOService * client, IOOptionBits options) override;
#endif
};

#endif /* defined(__virtio_osx__VirtioMemBalloonDevice__) */
