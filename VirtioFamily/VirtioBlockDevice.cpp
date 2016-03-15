//
//  VirtioBlockDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 30/03/2015.
//
//

#include "VirtioBlockDevice.h"
#include "VirtioDevice.h"
#include "../virtio-net/SSDCMultiSubrangeMemoryDescriptor.h"
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDriver.h>

OSDefineMetaClassAndStructors(VirtioBlockDevice, IOBlockStorageDevice);

static VirtioBlockDeviceRequest* virtio_block_device_request_create();
static void virtio_block_device_request_free(VirtioBlockDeviceRequest* request);

namespace VirtioBlockDeviceFeatures
{
	enum VirtioBlockDeviceFeatureBits
	{
		VIRTIO_BLK_F_SIZE_MAX = (1u << 1),
		VIRTIO_BLK_F_SEG_MAX = (1u << 2),
		VIRTIO_BLK_F_GEOMETRY = (1u << 4),
		VIRTIO_BLK_F_RO = (1u << 5),
		VIRTIO_BLK_F_BLK_SIZE = (1u << 6),
		VIRTIO_BLK_F_TOPOLOGY = (1u << 10),
		
	};
	
	static const uint32_t SUPPORTED_FEATURES =
		VirtioBlockDeviceFeatures::VIRTIO_BLK_F_SEG_MAX
		| VirtioBlockDeviceFeatures::VIRTIO_BLK_F_BLK_SIZE
		| VirtioBlockDeviceFeatures::VIRTIO_BLK_F_TOPOLOGY
		| VirtioBlockDeviceFeatures::VIRTIO_BLK_F_RO
		| VirtioDeviceGenericFeature::VIRTIO_F_RING_INDIRECT_DESC;
}

struct VirtioBlockDeviceRequest;
typedef IOReturn(*VirtioBlockDeviceRequestSubmitFn)(VirtioBlockDevice* device, VirtioBlockDeviceRequest* request);

struct VirtioBlockDeviceRequest
{
	genc_slist_head_t head;
	
	IOBufferMemoryDescriptor* header;
	SSDCMultiSubrangeMemoryDescriptor* subrange_md;
	IOBufferMemoryDescriptor* status;
	
	SSDCMemoryDescriptorSubrange subranges[2];
	
	union
	{
		struct
		{
			IOStorageCompletion storage_completion;
			uint64_t length;
		};
		struct
		{
			IOReturn sync_result;
		};
	};
	
	VirtioBlockDeviceRequestSubmitFn submit_fn;
};



struct virtio_blk_req_header
{
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

static VirtioBlockDeviceRequest* virtio_block_device_request_create()
{
	IOBufferMemoryDescriptor* header = IOBufferMemoryDescriptor::inTaskWithOptions(
		kernel_task, kIODirectionOut, sizeof(virtio_blk_req_header), alignof(virtio_blk_req_header));
	IOBufferMemoryDescriptor* status = IOBufferMemoryDescriptor::inTaskWithOptions(
		kernel_task, kIODirectionIn, sizeof(uint8_t), alignof(uint8_t));
	SSDCMultiSubrangeMemoryDescriptor* subrange_md = SSDCMultiSubrangeMemoryDescriptor::withDescriptorRanges(
		nullptr, 0, kIODirectionNone, false);
	VirtioBlockDeviceRequest* request = static_cast<VirtioBlockDeviceRequest*>(
		IOMallocAligned(sizeof(struct VirtioBlockDeviceRequest), alignof(struct VirtioBlockDeviceRequest)));
	if (header == nullptr || status == nullptr || subrange_md == nullptr || request == nullptr)
	{
		OSSafeReleaseNULL(header);
		OSSafeReleaseNULL(status);
		OSSafeReleaseNULL(subrange_md);
		if (request != nullptr)
			IOFreeAligned(request, sizeof(*request));
		return nullptr;
	}
	
	memset(request, 0, sizeof(*request));
	request->header = header;
	request->status = status;
	request->subrange_md = subrange_md;
	
	return request;
}

static void virtio_block_device_request_free(VirtioBlockDeviceRequest* request)
{
	OSSafeReleaseNULL(request->header);
	OSSafeReleaseNULL(request->status);
	OSSafeReleaseNULL(request->subrange_md);
	IOFreeAligned(request, sizeof(*request));
}


enum VirtioBlockRequestType
{
	VIRTIO_BLK_T_IN = 0,
	VIRTIO_BLK_T_OUT = 1,
	VIRTIO_BLK_T_FLUSH = 4,
	VIRTIO_BLK_T_FLUSH_OUT = 5,
};

enum VirtioBlockRequestStatus
{
	 VIRTIO_BLK_S_OK = 0,
	 VIRTIO_BLK_S_IOERR = 1,
	 VIRTIO_BLK_S_UNSUPP = 2,
};


bool VirtioBlockDevice::start(IOService* provider)
{
	kprintf("VirtioBlockDevice::start \n");
	if (!IOBlockStorageDevice::start(provider))
	{
		return false;
	}
	VirtioDevice* virtio = OSDynamicCast(VirtioDevice, provider);
	if (virtio == NULL)
		return false;
	
	if (!virtio->open(this))
		return false;
	
	virtio->resetDevice();
	//kprintf("VirtioBlockDevice::start() resetDevice\n");
	
	uint32_t dev_features = virtio->supportedFeatures();
	uint32_t use_features = dev_features & VirtioBlockDeviceFeatures::SUPPORTED_FEATURES;
	this->active_features = use_features;
	//kprintf("VirtioBlockDevice::start() use Features\n");

	genc_slq_init(&this->pending_requests);
	
	bool ok = virtio->requestFeatures(use_features);
	if (!ok)
	{
		virtio->failDevice();
		virtio->close(this);
		return false;
	}
	//kprintf("VirtioBlockDevice::start() bool ok\n");

	uint32_t seg_max = 0;
	if (this->active_features & VirtioBlockDeviceFeatures::VIRTIO_BLK_F_SEG_MAX)
	{
		seg_max = virtio->readDeviceConfig32LE(CONFIG_SEG_MAX_OFFSET);
	}
	else
	{
		//512 - header - status
		seg_max = 510;
	}
	//kprintf("VirtioBlockDevice::start() active features\n");

	unsigned queue_size = 0;
	const unsigned indirect_desc_per_request[1] = {seg_max + 2};
	IOReturn result = virtio->setupVirtqueues(1, nullptr, &queue_size, indirect_desc_per_request);
	if (result != kIOReturnSuccess)
	{
		virtio->failDevice();
		virtio->close(this);
		return false;
	}
	//kprintf("VirtioBlockDevice::start() setup virtqueues\n");

	
	if((this->active_features & VirtioDeviceGenericFeature::VIRTIO_F_RING_INDIRECT_DESC) == 0)
	{
		//indirct feature not supported
		if(seg_max > queue_size-2)
		{
			seg_max = queue_size - 2;
		}
		max_concurrent_requests = queue_size / 3;
	}
	else
	{
		max_concurrent_requests = queue_size;
	}
	//kprintf("VirtioBlockDevice::start() indirect features\n");

	this->max_request_segments = seg_max;
	//kprintf("VirtioBlockDevice::start() request segments\n");

	IOWorkLoop* work_loop = this->getWorkLoop();
	this->command_gate = IOCommandGate::commandGate(this);
	this->command_gate->setWorkLoop(work_loop);
	//kprintf("VirtioBlockDevice::start() command gate\n");

	unsigned pool_size = queue_size + 32;
	for (unsigned i = 0; i < pool_size; ++i)
	{
		VirtioBlockDeviceRequest* request = virtio_block_device_request_create();
		this->returnRequestToPool(request);
	}
	//kprintf("VirtioBlockDevice::start() pool\n");

	this->virtio_device = virtio;
	virtio->startDevice(&deviceConfigChangeAction, this);
	//kprintf("VirtioBlockDevice::start() start Device\n");

	this->command_gate->runAction(
		[](OSObject* block_dev, void* arg0, void* arg1, void* arg2, void* arg3)
		{
			VirtioBlockDevice* me = static_cast<VirtioBlockDevice*>(block_dev);
			
			uint64_t capacity = me->virtio_device->readDeviceConfig64LETransitional(CONFIG_CAPCITY_OFFSET);
			me->capacity_in_bytes = capacity*512;
			
			uint32_t size_max = me->virtio_device->readDeviceConfig32LE(CONFIG_SIZE_MAX_OFFSET);
			uint32_t block_size = 512;
			if(me->active_features & VirtioBlockDeviceFeatures::VIRTIO_BLK_F_BLK_SIZE)
			{
				block_size = me->virtio_device->readDeviceConfig32LE(CONFIG_BLK_SIZE_OFFSET);
			}
			
			me->block_size = block_size;
			me->sectors_per_block = block_size / 512;
			IOLog("VirtioBlockDevice::start(): capacity = %llu, size_max = %u, block_size = %u\n", capacity, size_max, block_size);
			kprintf("VirtioBlockDevice::start(): capacity = %llu, size_max = %u, block_size = %u\n", capacity, size_max, block_size);

			return kIOReturnSuccess;
		});
	
	this->registerService();
	//kprintf("VirtioBlockDevice::start() register service\n");

	kprintf("VirtioBlockDevice::start done!\n");
	return true;

}

bool VirtioBlockDevice::handleOpen(IOService* forClient, IOOptionBits options, void* arg)
{
	bool ok = IOBlockStorageDevice::handleOpen(forClient, options, arg);
	if (ok)
	{
		IOBlockStorageDriver* client_driver = OSDynamicCast(IOBlockStorageDriver, forClient);
		if (client_driver != nullptr)
		{
			client_driver->setProperty(kIOMaximumSegmentCountReadKey, this->max_request_segments, 32);
			client_driver->setProperty(kIOMaximumSegmentCountWriteKey, this->max_request_segments, 32);
			client_driver->setProperty(kIOCommandPoolSizeKey, this->max_concurrent_requests, 32);
		}
	}
	return ok;
}


void VirtioBlockDevice::deviceConfigChangeAction(OSObject* target, VirtioDevice* source)
{
	OSDynamicCast(VirtioBlockDevice, target)->deviceConfigChangeAction(source);
}

void VirtioBlockDevice::deviceConfigChangeAction(VirtioDevice* source)
{
/*
	uint32_t num_pages = this->virtio_device->readDeviceConfig32LE(CONFIG_NUM_REQUESTED_PAGES_OFFSET);
	uint32_t actual = this->virtio_device->readDeviceConfig32LE(CONFIG_ACTUAL_PAGES_OFFSET);
	IOLog("VirtioMemBalloonDevice::deviceConfigChangeAction(): num_pages = %u, actual = %u\n", num_pages, actual);
	this->inflateDeflateIfNecessary(num_pages);
	*/
}

void VirtioBlockDevice::endDeviceOperation()
{
	if (this->virtio_device != nullptr)
	{
		this->virtio_device->failDevice();
		this->virtio_device->close(this);
		this->virtio_device = nullptr;
	}
}


void VirtioBlockDevice::stop(IOService* provider)
{
	VLTLog("VirtioBlockDevice[%p]::stop() (virtio_device = %p)\n", this, this->virtio_device);
	this->endDeviceOperation();
	if (this->command_gate)
		this->command_gate->setWorkLoop(nullptr);
	OSSafeReleaseNULL(this->command_gate);
	
	while (this->request_pool != nullptr)
	{
		VirtioBlockDeviceRequest* request = genc_slist_remove_object_at(
			&this->request_pool, VirtioBlockDeviceRequest, head);
		virtio_block_device_request_free(request);
	}
	
	IOBlockStorageDevice::stop(provider);
	VLTLog("VirtioMemBalloonDevice::stop(): done\n");
}

bool VirtioBlockDevice::didTerminate( IOService * provider, IOOptionBits options, bool * defer )
{
	this->endDeviceOperation();
	VLTLog("VirtioBlockDevice[%p]::didTerminate() provider = %p, options = %x, defer = %s [%p]\n", this, provider, options, defer ? ((*defer) ? "true" : "false") : "NULL", defer);
	bool res = IOBlockStorageDevice::didTerminate(provider, options, defer);
	VLTLog("VirtioBlockDevice[%p]::didTerminate() done: %s, defer = %s [%p]\n", this, res ? "true" : "false", defer ? ((*defer) ? "true" : "false") : "NULL", defer);
	return res;
}

VirtioBlockDeviceRequest* VirtioBlockDevice::requestFromPool()
{
	VirtioBlockDeviceRequest* request = genc_slist_remove_object_at(&this->request_pool, VirtioBlockDeviceRequest, head);
	while (request == nullptr)
	{
		this->command_gate->commandSleep(&this->request_pool, THREAD_UNINT);
	}
	return request;
}
void VirtioBlockDevice::returnRequestToPool(VirtioBlockDeviceRequest* request)
{
	genc_slist_insert_at(&request->head, &this->request_pool);
	this->command_gate->commandWakeup(&this->request_pool, true);
}

IOReturn VirtioBlockDevice::doSynchronizeCache(void)
{
	return this->command_gate->runAction(&doSynchronizeCacheOnWorkLoop);
}

IOReturn VirtioBlockDevice::doSynchronizeCacheOnWorkLoop(OSObject* block_dev, void* arg0, void* arg1, void* arg2, void* arg3)
{
	return static_cast<VirtioBlockDevice*>(block_dev)->doSynchronizeCacheOnWorkLoop();
}
static IOReturn virtioBlockDeviceGetRequestResult(VirtioBlockDeviceRequest* request, bool device_reset);

IOReturn VirtioBlockDevice::doSynchronizeCacheOnWorkLoop()
{
	VirtioBlockDeviceRequest* request = this->requestFromPool();
	virtio_blk_req_header* header = static_cast<virtio_blk_req_header*>(request->header->getBytesNoCopy());

	header->type = VIRTIO_BLK_T_FLUSH;
	header->reserved = 0;
	header->sector = 0;
	
	request->submit_fn =
		[](VirtioBlockDevice* device, VirtioBlockDeviceRequest* request)
		{
			VirtioCompletion my_completion = { &flushRequestCompleted, device, request };
			return device->virtio_device->submitBuffersToVirtqueue(0, request->header, request->status, my_completion);
		};

	IOReturn submit_result = kIOReturnNoSpace;
	if (genc_slq_is_empty(&this->pending_requests))
		submit_result = request->submit_fn(this, request);

	if (submit_result == kIOReturnNoSpace || submit_result == kIOReturnBusy)
	{
		//kprintf("VirtioBlockDevice[%p]::doAsyncReadWriteOnWorkLoop(): deferring request %p to queue (submit_result = %x)\n", this, request, submit_result);
		// not enough space in virtqueue
		genc_slq_push_back(&this->pending_requests, &request->head);
		submit_result = kIOReturnSuccess;
	}
	
	if (submit_result != kIOReturnSuccess)
	{
		this->returnRequestToPool(request);
		return submit_result;
	}

	this->command_gate->commandSleep(request, THREAD_UNINT);
	
	IOReturn syncResult = request->sync_result;
	this->returnRequestToPool(request);
	
	return syncResult;
}
void VirtioBlockDevice::flushRequestCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written)
{
	VirtioBlockDevice* me = static_cast<VirtioBlockDevice*>(target);
	me->flushRequestCompleted(static_cast<VirtioBlockDeviceRequest*>(ref), device_reset);

}

void VirtioBlockDevice::flushRequestCompleted(VirtioBlockDeviceRequest* request, bool device_reset)
{
	IOReturn result = virtioBlockDeviceGetRequestResult(request, device_reset);
	request->sync_result = result;
	this->handlePendingRequests(device_reset);
	this->command_gate->commandWakeup(request, true);
}

struct args
{
	IOMemoryDescriptor* buffer;
	UInt64 block;
	UInt64 nblks;
	IOStorageAttributes* attributes;
	IOStorageCompletion* completion;
};

IOReturn VirtioBlockDevice::doAsyncReadWrite(IOMemoryDescriptor* buffer, UInt64 block, UInt64 nblks, IOStorageAttributes* attributes, IOStorageCompletion* completion)
{
	//kprintf("VirtioBlockDevice[%p]::doAsyncReadWrite(%p, %llu, %llu, %p, %p);\n", this, buffer, block, nblks, attributes, completion);
	args a = { buffer, block, nblks, attributes, completion };
	return this->command_gate->runAction(&doAsyncReadWriteOnWorkLoop, &a);
}

IOReturn VirtioBlockDevice::doAsyncReadWriteOnWorkLoop(
	OSObject* block_dev, void* arg0, void* arg1, void* arg2, void* arg3)
{
	args* a = static_cast<args*>(arg0);
	VirtioBlockDevice* me = static_cast<VirtioBlockDevice*>(block_dev);
	return me->doAsyncReadWriteOnWorkLoop(a->buffer, a->block, a->nblks, a->attributes, a->completion);
}


IOReturn VirtioBlockDevice::doAsyncReadWriteOnWorkLoop(IOMemoryDescriptor* buffer, UInt64 block, UInt64 nblks, IOStorageAttributes* attributes, IOStorageCompletion* completion)
{
	IODirection direction = buffer->getDirection();
	uint64_t request_length = (nblks * this->block_size);
	if(buffer->getLength() < request_length)
	{
		//buffer too small
		return kIOReturnBadArgument;
	}
	
	if((block + nblks) * this->block_size > this->capacity_in_bytes)
	{
		//bigger than the block
		return kIOReturnBadArgument;
	}
	
	VirtioBlockDeviceRequest* request = this->requestFromPool();
	//kprintf("VirtioBlockDevice[%p]::doAsyncReadWriteOnWorkLoop(%p, %llu, %llu, %p, %p), request = %p, direction = %u\n", this, buffer, block, nblks, attributes, completion, request, direction);
	request->length = request_length;
	virtio_blk_req_header* header = static_cast<virtio_blk_req_header*>(request->header->getBytesNoCopy());

	if(direction == kIODirectionIn)
	{
		//Disk read
		header->type = VIRTIO_BLK_T_IN;
		header->reserved = 0;
		header->sector = block * this->sectors_per_block;
		
		request->subranges[0].md = buffer;
		request->subranges[0].length = request_length;
		request->subranges[0].offset = 0;
		
		request->subranges[1].md = request->status;
		request->subranges[1].length = request->status->getLength();
		request->subranges[1].offset = 0;
		
		request->subrange_md->initWithDescriptorRanges(request->subranges, 2, direction, false);
		
		request->storage_completion = *completion;
		
		request->submit_fn =
			[](VirtioBlockDevice* device, VirtioBlockDeviceRequest* request)
			{
				VirtioCompletion my_completion = { &blockRequestCompleted, device, request };
				return device->virtio_device->submitBuffersToVirtqueue(0, request->header, request->subrange_md, my_completion);
			};
		
	}
	else if (direction == kIODirectionOut)
	{

		//Disk write
		header->type = VIRTIO_BLK_T_OUT;
		header->reserved = 0;
		header->sector = block * this->sectors_per_block;
		
		request->subranges[0].md = request->header;
		request->subranges[0].length = request->header->getLength();
		request->subranges[0].offset = 0;
		
		request->subranges[1].md = buffer;
		request->subranges[1].length = request_length;
		request->subranges[1].offset = 0;
		
		request->subrange_md->initWithDescriptorRanges(request->subranges, 2, direction, false);
		
		request->storage_completion = *completion;

		request->submit_fn =
			[](VirtioBlockDevice* device, VirtioBlockDeviceRequest* request)
			{
				VirtioCompletion my_completion = { &blockRequestCompleted, device, request };
				return device->virtio_device->submitBuffersToVirtqueue(0, request->subrange_md, request->status, my_completion);
			};
	}
	else
	{
		//can only be a read or a write cannot be both
		return kIOReturnBadArgument;
	}

	IOReturn submit_result = kIOReturnNoSpace;
	if (genc_slq_is_empty(&this->pending_requests))
		submit_result = request->submit_fn(this, request);

	if (submit_result == kIOReturnNoSpace || submit_result == kIOReturnBusy)
	{
		//kprintf("VirtioBlockDevice[%p]::doAsyncReadWriteOnWorkLoop(): deferring request %p to queue (submit_result = %x)\n", this, request, submit_result);
		// not enough space in virtqueue
		genc_slq_push_back(&this->pending_requests, &request->head);
		submit_result = kIOReturnSuccess;
	}
	
	if (submit_result != kIOReturnSuccess)
	{
		request->subrange_md->initWithDescriptorRanges(nullptr, 0, kIODirectionNone, false);
		this->returnRequestToPool(request);
	}
	
	//kprintf("VirtioBlockDevice[%p]::doAsyncReadWriteOnWorkLoop(): submit_result = %x\n", this, submit_result);
	//VirtioCompletion completion = { &blockRequestCompleted, this };
	return submit_result;
}

void VirtioBlockDevice::blockRequestCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written)
{
	VirtioBlockDevice* me = static_cast<VirtioBlockDevice*>(target);
	me->blockRequestCompleted(static_cast<VirtioBlockDeviceRequest*>(ref), device_reset);

}

static IOReturn virtioBlockDeviceGetRequestResult(VirtioBlockDeviceRequest* request, bool device_reset)
{
	IOReturn result;
	if(device_reset)
	{
		//device shutdown
		result = kIOReturnAborted;
	}
	else
	{
		uint8_t status = *static_cast<uint8_t*>(request->status->getBytesNoCopy());
		if(status == VIRTIO_BLK_S_OK)
		{
			result = kIOReturnSuccess;
		}
		else if(status == VIRTIO_BLK_S_IOERR)
		{
			result = kIOReturnIOError;
		}
		else if (status == VIRTIO_BLK_S_UNSUPP)
		{
			result = kIOReturnUnsupported;
		}
		else
		{
			result = kIOReturnDeviceError;
		}
	}
	return result;
	
}

void VirtioBlockDevice::blockRequestCompleted(VirtioBlockDeviceRequest* request, bool device_reset)
{
	

	IOReturn result = virtioBlockDeviceGetRequestResult(request, device_reset);
	uint64_t actual_bytes = 0;
	if(result == kIOReturnSuccess)
		actual_bytes = request->length;

	request->subrange_md->initWithDescriptorRanges(nullptr, 0, kIODirectionNone, false);
	IOStorageCompletion completion = request->storage_completion;
	this->returnRequestToPool(request);
	completion.action(completion.target, completion.parameter, result, actual_bytes);
	this->handlePendingRequests(device_reset);
}

void VirtioBlockDevice::handlePendingRequests(bool device_reset)
{
	if (!device_reset)
	{
		while (!genc_slq_is_empty(&this->pending_requests))
		{
			VirtioBlockDeviceRequest* next_request =
				genc_slq_pop_front_object(&this->pending_requests, VirtioBlockDeviceRequest, head);
			IOReturn submit_result = next_request->submit_fn(this, next_request);
			if (submit_result != kIOReturnSuccess)
			{
				genc_slq_push_front(&this->pending_requests, &next_request->head);
				break;
			}
		}
	}
	else
	{
		while (!genc_slq_is_empty(&this->pending_requests))
		{
			VirtioBlockDeviceRequest* next_request =
				genc_slq_pop_front_object(&this->pending_requests, VirtioBlockDeviceRequest, head);

			next_request->subrange_md->initWithDescriptorRanges(nullptr, 0, kIODirectionNone, false);
			IOStorageCompletion completion = next_request->storage_completion;
			this->returnRequestToPool(next_request);
			completion.action(completion.target, completion.parameter, kIOReturnAborted, 0);
		}
	}

}

IOReturn VirtioBlockDevice::doEjectMedia(void)
{
	return kIOReturnUnsupported;
}
IOReturn VirtioBlockDevice::doFormatMedia(UInt64 byteCapacity)
{
	return kIOReturnUnsupported;
}

UInt32 VirtioBlockDevice::doGetFormatCapacities(UInt64* capacities, UInt32 capacitiesMaxCount) const
{
	if (capacities != nullptr && capacitiesMaxCount < 1)
		return 0;
	if (capacities != nullptr)
		capacities[0] = capacity_in_bytes;
	kprintf("capacity in bytes: %llu\n", capacities[0]);
	return 1;
}

IOReturn VirtioBlockDevice::doLockUnlockMedia(bool doLock)
{
	return kIOReturnUnsupported;
}

char* VirtioBlockDevice::getVendorString(void)
{
	return (char*)"Virtio";
}

char* VirtioBlockDevice::getProductString(void)
{
	return (char*)"Virtio Block Device";
}

char* VirtioBlockDevice::getRevisionString(void)
{
	return (char*)"VirtioBlockDevice::getRevisionString";
}

char* VirtioBlockDevice::getAdditionalDeviceInfoString(void)
{
	return (char*)"VirtioBlockDevice::getAdditionalDeviceInfoString";
}

IOReturn VirtioBlockDevice::reportBlockSize(UInt64* blockSize)
{
	*blockSize = this->block_size;
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportEjectability(bool* isEjectable)
{
	*isEjectable = false;
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportLockability(bool* isLockable)
{
	*isLockable = false;
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportMaxValidBlock(UInt64* maxBlock)
{
	*maxBlock = (this->capacity_in_bytes/this->block_size)-1;
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportMediaState(bool* mediaPresent, bool* changedState)
{
	*mediaPresent = true;
	*changedState = false;
	return  kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportPollRequirements(bool* pollRequired, bool* pollIsExpensive)
{
	*pollRequired = false;
	*pollIsExpensive = false;
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportRemovability(bool* isRemovable)
{
	*isRemovable = false;
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::reportWriteProtection(bool* isWriteProtected)
{
	if(this->active_features & VirtioBlockDeviceFeatures::VIRTIO_BLK_F_RO)
	{
		*isWriteProtected = true;
	}
	else
	{
		*isWriteProtected = false;
	}
	return kIOReturnSuccess;
}

IOReturn VirtioBlockDevice::getWriteCacheState(bool* enabled)
{
	return kIOReturnUnsupported;
}

IOReturn VirtioBlockDevice::setWriteCacheState(bool enabled)
{
	return kIOReturnUnsupported;
}





// DON'T ADD ANYTHING BELOW THIS

#ifdef VIRTIO_LOG_TERMINATION
bool VirtioBlockDevice::terminateClient(IOBlockStorageDevice * client, IOOptionBits options)
{
	IOLog("VirtioBlockDevice[%p]::terminateClient() client = %p, options = %x\n", this, client, options);
	bool res = IOBlockStorageDevice::terminateClient(client, options);
	IOLog("VirtioBlockDevice[%p]::terminateClient() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioBlockDevice::requestTerminate( IOBlockStorageDevice * provider, IOOptionBits options )
{
	IOLog("VirtioBlockDevice[%p]::requestTerminate() provider = %p, options = %x\n", this, provider, options);
	bool res = IOBlockStorageDevice::requestTerminate(provider, options);
	IOLog("VirtioBlockDevice[%p]::requestTerminate() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioBlockDevice::willTerminate( IOBlockStorageDevice * provider, IOOptionBits options )
{
	IOLog("VirtioBlockDevice[%p]::willTerminate() provider = %p, options = %x\n", this, provider, options);
	bool res = IOBlockStorageDevice::willTerminate(provider, options);
	IOLog("VirtioBlockDevice[%p]::willTerminate() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioBlockDevice::terminate( IOOptionBits options )
{
	IOLog("VirtioBlockDevice[%p]::terminate() options = %x\n", this, options);
	bool res = IOBlockStorageDevice::terminate(options);
	IOLog("VirtioBlockDevice[%p]::terminate() done: %s\n", this, res ? "true" : "false");
	return res;
}
#endif //ifdef VIRTIO_LOG_TERMINATION

