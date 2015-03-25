//
//  VirtioMemBalloonDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 19/03/2015.
//
//

#include "VirtioMemBalloonDevice.h"
#include "VirtioDevice.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

OSDefineMetaClassAndStructors(VirtioMemBalloonDevice, IOService);

static const uint32_t VIRTIO_SUPPORTED_MEMORY_BALLOON_FEATURES = 0;


bool VirtioMemBalloonDevice::start(IOService* provider)
{
	if (!IOService::start(provider))
	{
		return false;
	}
	VirtioDevice* virtio = OSDynamicCast(VirtioDevice, provider);
	if (virtio == NULL)
		return false;
	
	if (!virtio->open(this))
		return false;
	
	virtio->resetDevice();
	
	uint32_t dev_features = virtio->supportedFeatures();
	uint32_t use_features = dev_features & VIRTIO_SUPPORTED_MEMORY_BALLOON_FEATURES;
	
	bool ok = virtio->requestFeatures(use_features);
	if (!ok)
	{
		virtio->failDevice();
		virtio->close(this);
		return false;
	}
	
	IOReturn result = virtio->setupVirtqueues(2);
	if (result != kIOReturnSuccess)
	{
		virtio->failDevice();
		virtio->close(this);
		return false;
	}
	
	IOWorkLoop* work_loop = this->getWorkLoop();
	this->command_gate = IOCommandGate::commandGate(this);
	this->command_gate->setWorkLoop(work_loop);
	
	this->virtio_device = virtio;
	size_t maxArraySize = BIG_CHUNK_PAGES * sizeof(uint32_t);
	this->page_address_array = IOBufferMemoryDescriptor::inTaskWithOptions(
		kernel_task, kIODirectionOut, maxArraySize, alignof(uint32_t));
	
	virtio->startDevice(&deviceConfigChangeAction, this);
	
	this->pageBuffers = OSArray::withCapacity(0);
	this->bigChunkBuffers = OSArray::withCapacity(0);
	this->inflateDeflateInProgress = false;
	this->command_gate->runAction(
		[](OSObject* mem_balloon, void* arg0, void* arg1, void* arg2, void* arg3)
		{
			VirtioMemBalloonDevice* me = static_cast<VirtioMemBalloonDevice*>(mem_balloon);
			
			uint32_t num_pages = me->virtio_device->readDeviceSpecificConfig32LE(CONFIG_NUM_REQUESTED_PAGES_OFFSET);
			uint32_t actual = me->virtio_device->readDeviceSpecificConfig32LE(CONFIG_ACTUAL_PAGES_OFFSET);
			IOLog("VirtioMemBalloonDevice::start(): num_pages = %u, actual = %u\n", num_pages, actual);
			me->inflateDeflateIfNecessary(num_pages);
			return kIOReturnSuccess;
		},
		this);
	
	return true;
}

void VirtioMemBalloonDevice::deviceConfigChangeAction(OSObject* target, VirtioDevice* source)
{
	OSDynamicCast(VirtioMemBalloonDevice, target)->deviceConfigChangeAction(source);
}

void VirtioMemBalloonDevice::deviceConfigChangeAction(VirtioDevice* source)
{
	uint32_t num_pages = this->virtio_device->readDeviceSpecificConfig32LE(CONFIG_NUM_REQUESTED_PAGES_OFFSET);
	uint32_t actual = this->virtio_device->readDeviceSpecificConfig32LE(CONFIG_ACTUAL_PAGES_OFFSET);
	IOLog("VirtioMemBalloonDevice::deviceConfigChangeAction(): num_pages = %u, actual = %u\n", num_pages, actual);
	this->inflateDeflateIfNecessary(num_pages);
}

void VirtioMemBalloonDevice::endDeviceOperation()
{
	if (this->virtio_device != nullptr)
	{
		this->virtio_device->failDevice();
		this->virtio_device->close(this);
		this->virtio_device = nullptr;
	}
}


void VirtioMemBalloonDevice::stop(IOService* provider)
{
	VLTLog("VirtioMemBalloonDevice[%p]::stop() (virtio_device = %p)\n", this, this->virtio_device);
	this->endDeviceOperation();
	if (this->command_gate)
		this->command_gate->setWorkLoop(nullptr);
	OSSafeReleaseNULL(this->command_gate);
	
	IOService::stop(provider);
	VLTLog("VirtioMemBalloonDevice::stop(): done\n");
}

bool VirtioMemBalloonDevice::didTerminate( IOService * provider, IOOptionBits options, bool * defer )
{
	this->endDeviceOperation();
	VLTLog("VirtioMemBalloonDevice[%p]::didTerminate() provider = %p, options = %x, defer = %s [%p]\n", this, provider, options, defer ? ((*defer) ? "true" : "false") : "NULL", defer);
	bool res = IOService::didTerminate(provider, options, defer);
	VLTLog("VirtioMemBalloonDevice[%p]::didTerminate() done: %s, defer = %s [%p]\n", this, res ? "true" : "false", defer ? ((*defer) ? "true" : "false") : "NULL", defer);
	return res;
}
uint32_t VirtioMemBalloonDevice::totalPagesAllocated()
{
	uint32_t num_pages_allocated = this->pageBuffers->getCount();
	uint32_t num_big_chunks_allocated = this->bigChunkBuffers->getCount();
	uint32_t total_pages_allocated = num_big_chunks_allocated * BIG_CHUNK_PAGES + num_pages_allocated;
	return total_pages_allocated;
}
void VirtioMemBalloonDevice::inflateDeflateIfNecessary(uint32_t num_pages_requested)
{
	// check if an inflate/deflate request is in progress, and if so, return immediately
	if(this->inflateDeflateInProgress)
	{
		IOLog("VirtioMemBalloonDevice::inflateDeflateIfNecessary: Inflate/deflate already in progress\n");
		return;
	}
	// calculate number of pages already allocated
	
	// compare to requested and decide if we need to inflate or deflate, or neither
	uint32_t total_pages_allocated = this->totalPagesAllocated();
	if (total_pages_allocated == num_pages_requested)
	{
		//same size so dont do anything
	}
	else if (total_pages_allocated < num_pages_requested)
	{
		//need to inflate the balloon
		uint32_t num_pages_to_inflate_by = num_pages_requested - total_pages_allocated;
		this->inflateMemBalloon(num_pages_to_inflate_by);
	}
	else
	{
		//deflate the balloon
		uint32_t num_pages_to_deflate_by = total_pages_allocated - num_pages_requested;
		this->deflateMemBalloon(num_pages_to_deflate_by);
	}

}
void VirtioMemBalloonDevice::inflateMemBalloon(uint32_t num_pages_to_inflate_by)
{
	// set the flag indicating that an inflation/deflation is in progress
	this->inflateDeflateInProgress = true;

	const mach_vm_address_t MEM_BALLOON_PHYS_ALLOC_MASK = 0xffffffffull << 12u;
	// decide whether to alloc 1 big chunk or many pages
	if (num_pages_to_inflate_by >= BIG_CHUNK_PAGES)
	{
		// big chunk
		IOBufferMemoryDescriptor* chunk = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
			kernel_task,
			kIODirectionInOut | kIOMemoryMapperNone /* we want CPU-physical addresses, not mapped for DMA */,
			BIG_CHUNK_BYTES, MEM_BALLOON_PHYS_ALLOC_MASK);
		chunk->prepare(kIODirectionInOut);
		
		this->bigChunkBuffers->setObject(chunk);
		
		IOReturn result = kIOReturnSuccess;
		
		// tell the memory balloon the physical addresses of all pages in the chunk
		this->page_address_array->setLength(sizeof(uint32_t) * BIG_CHUNK_PAGES);
		uint32_t* page_addresses = static_cast<uint32_t*>(this->page_address_array->getBytesNoCopy());
		//for (each page in chunk)
		for (unsigned page = 0; page < BIG_CHUNK_PAGES; page++)
		{
			IOByteCount page_offset = page * PAGE_SIZE;
			
			IOByteCount len = 0;
			addr64_t phys_addr = chunk->getPhysicalSegment(page_offset, &len, kIOMemoryMapperNone);
			if (phys_addr == 0 || len < PAGE_SIZE)
			{
				// error
				result = kIOReturnInternalError;
				break;
			}
			page_addresses[page] = static_cast<uint32_t>(phys_addr / 4096);
		}
		
		if (result == kIOReturnSuccess)
		{
			VirtioCompletion completion = { &inflateRequestCompleted, this };
			result = this->virtio_device->submitBuffersToVirtqueue(INFLATE_QUEUE_INDEX, this->page_address_array, nullptr, completion);
			if (result != kIOReturnSuccess)
			{
				IOLog("VirtioMemBalloonDevice::inflateMemBalloon(): submitBuffersToVirtqueue failed for big chunk page addresses - %x\n", result);
			}
		}
		
		if (result != kIOReturnSuccess)
		{
			// error occurred
			memset(page_addresses, 0, sizeof(page_addresses[0]) * BIG_CHUNK_PAGES);
			chunk->complete(kIODirectionInOut);
			this->bigChunkBuffers->removeObject(this->bigChunkBuffers->getCount() - 1);
			this->inflateDeflateInProgress = false;
			
		}
		OSSafeReleaseNULL(chunk);
	}
	else
	{
		this->page_address_array->setLength(sizeof(uint32_t) * num_pages_to_inflate_by);
		uint32_t* page_addresses = static_cast<uint32_t*>(this->page_address_array->getBytesNoCopy());
		IOReturn result = kIOReturnSuccess;
		// allocate lots of PAGE_SIZE buffers in a loop
		unsigned i;
		for(i = 0; i < num_pages_to_inflate_by; i++)
		{
			IOBufferMemoryDescriptor* page_buf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
				kernel_task,
				kIODirectionInOut | kIOMemoryMapperNone /* we want CPU-physical addresses, not mapped for DMA */,
				PAGE_SIZE, MEM_BALLOON_PHYS_ALLOC_MASK);
			
			page_buf->prepare(kIODirectionInOut);
			this->pageBuffers->setObject(page_buf);
		
			// tell the memory balloon the physical addresses of the pages
			
			IOByteCount len = 0;
			addr64_t phys_addr = page_buf->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
			if (phys_addr == 0 || len < PAGE_SIZE)
			{
				// error
				result = kIOReturnInternalError;
				break;
			}
			page_addresses[i] = static_cast<uint32_t>(phys_addr / 4096);
			OSSafeReleaseNULL(page_buf);
		}
		
		if (result == kIOReturnSuccess)
		{
			VirtioCompletion completion = { &inflateRequestCompleted, this };
			result = this->virtio_device->submitBuffersToVirtqueue(INFLATE_QUEUE_INDEX, this->page_address_array, nullptr, completion);
			if (result != kIOReturnSuccess)
			{
				IOLog("VirtioMemBalloonDevice::inflateMemBalloon(): submitBuffersToVirtqueue failed for individual page addresses - %x\n", result);
			}
		}
		
		if (result != kIOReturnSuccess)
		{
			// error occurred
			memset(page_addresses, 0, sizeof(page_addresses[0]) * PAGE_SIZE);
			for(unsigned j = 0; j < i; j++)
			{
				IOBufferMemoryDescriptor* page_buf = static_cast<IOBufferMemoryDescriptor*>(
					this->pageBuffers->getLastObject());
				page_buf->complete(kIODirectionInOut);
				this->pageBuffers->removeObject(this->pageBuffers->getCount() - 1);
			}
			this->inflateDeflateInProgress = false;
			
		}
		

		
	}
}

void VirtioMemBalloonDevice::inflateRequestCompleted(OSObject* target, void* ref, bool device_reset)
{
	VirtioMemBalloonDevice* me = static_cast<VirtioMemBalloonDevice*>(target);
	me->inflateRequestCompleted(device_reset);
}

void VirtioMemBalloonDevice::inflateRequestCompleted(bool device_reset)
{
	this->inflateDeflateInProgress = false;
	if(device_reset)
	{
		//device shutdown
		return;
	}
	
	uint32_t total_pages_allocated = this->totalPagesAllocated();
	this->virtio_device->writeDeviceSpecificConfig32LE(CONFIG_ACTUAL_PAGES_OFFSET, total_pages_allocated);
	
	uint32_t num_pages = this->virtio_device->readDeviceSpecificConfig32LE(CONFIG_NUM_REQUESTED_PAGES_OFFSET);
	uint32_t actual = this->virtio_device->readDeviceSpecificConfig32LE(CONFIG_ACTUAL_PAGES_OFFSET);
	IOLog("VirtioMemBalloonDevice::deviceConfigChangeAction(): num_pages = %u, actual = %u\n", num_pages, actual);
	this->inflateDeflateIfNecessary(num_pages);
}


void VirtioMemBalloonDevice::deflateMemBalloon(uint32_t num_pages_to_deflate_by)
{
}


// DON'T ADD ANYTHING BELOW THIS

#ifdef VIRTIO_LOG_TERMINATION
bool VirtioMemBalloonDevice::terminateClient(IOService * client, IOOptionBits options)
{
	IOLog("VirtioMemBalloonDevice[%p]::terminateClient() client = %p, options = %x\n", this, client, options);
	bool res = IOService::terminateClient(client, options);
	IOLog("VirtioMemBalloonDevice[%p]::terminateClient() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioMemBalloonDevice::requestTerminate( IOService * provider, IOOptionBits options )
{
	IOLog("VirtioMemBalloonDevice[%p]::requestTerminate() provider = %p, options = %x\n", this, provider, options);
	bool res = IOService::requestTerminate(provider, options);
	IOLog("VirtioMemBalloonDevice[%p]::requestTerminate() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioMemBalloonDevice::willTerminate( IOService * provider, IOOptionBits options )
{
	IOLog("VirtioMemBalloonDevice[%p]::willTerminate() provider = %p, options = %x\n", this, provider, options);
	bool res = IOService::willTerminate(provider, options);
	IOLog("VirtioMemBalloonDevice[%p]::willTerminate() done: %s\n", this, res ? "true" : "false");
	return res;
}

bool VirtioMemBalloonDevice::terminate( IOOptionBits options )
{
	IOLog("VirtioMemBalloonDevice[%p]::terminate() options = %x\n", this, options);
	bool res = IOService::terminate(options);
	IOLog("VirtioMemBalloonDevice[%p]::terminate() done: %s\n", this, res ? "true" : "false");
	return res;
}
#endif //ifdef VIRTIO_LOG_TERMINATION

