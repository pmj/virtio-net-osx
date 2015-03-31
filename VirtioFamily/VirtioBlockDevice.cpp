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
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

OSDefineMetaClassAndStructors(VirtioBlockDevice, IOBlockStorageDevice);

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
		| VirtioBlockDeviceFeatures::VIRTIO_BLK_F_RO;
}


bool VirtioBlockDevice::start(IOService* provider)
{
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
	
	uint32_t dev_features = virtio->supportedFeatures();
	uint32_t use_features = dev_features & VirtioBlockDeviceFeatures::SUPPORTED_FEATURES;
	this->active_features = use_features;
	
	
	
	bool ok = virtio->requestFeatures(use_features);
	if (!ok)
	{
		virtio->failDevice();
		virtio->close(this);
		return false;
	}
	
	IOReturn result = virtio->setupVirtqueues(1);
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
	virtio->startDevice(&deviceConfigChangeAction, this);
	
	this->command_gate->runAction(
		[](OSObject* block_dev, void* arg0, void* arg1, void* arg2, void* arg3)
		{
			VirtioBlockDevice* me = static_cast<VirtioBlockDevice*>(block_dev);
			
			uint64_t capacity = me->virtio_device->readDeviceSpecificConfig64LE(CONFIG_CAPCITY_OFFSET);
			me->capacity_in_bytes = capacity*512;
			uint32_t size_max = me->virtio_device->readDeviceSpecificConfig32LE(CONFIG_SIZE_MAX_OFFSET);
			uint32_t seg_max = me->virtio_device->readDeviceSpecificConfig32LE(CONFIG_SEG_MAX_OFFSET);
			uint32_t block_size = 512;
			if(me->active_features & VirtioBlockDeviceFeatures::VIRTIO_BLK_F_BLK_SIZE)
			{
				block_size = me->virtio_device->readDeviceSpecificConfig32LE(CONFIG_BLK_SIZE_OFFSET);
			}
			
			me->block_size = block_size;
			IOLog("VirtioBlockDevice::start(): capacity = %llu, size_max = %u, seg_max = %u, block_size = %u\n", capacity, size_max, seg_max, block_size);
			return kIOReturnSuccess;
		},
		this);
	
	this->registerService();
	
	return true;

}

void VirtioBlockDevice::deviceConfigChangeAction(OSObject* target, VirtioDevice* source)
{
	OSDynamicCast(VirtioBlockDevice, target)->deviceConfigChangeAction(source);
}

void VirtioBlockDevice::deviceConfigChangeAction(VirtioDevice* source)
{
/*
	uint32_t num_pages = this->virtio_device->readDeviceSpecificConfig32LE(CONFIG_NUM_REQUESTED_PAGES_OFFSET);
	uint32_t actual = this->virtio_device->readDeviceSpecificConfig32LE(CONFIG_ACTUAL_PAGES_OFFSET);
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

void VirtioBlockDevice::blockRequestCompleted(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written)
{
	VirtioBlockDevice* me = static_cast<VirtioBlockDevice*>(target);
	me->blockRequestCompleted();

}

void VirtioBlockDevice::blockRequestCompleted()
{

}

IOReturn VirtioBlockDevice::doSynchronizeCache(void)
{
	return kIOReturnError;
}

struct VirtioBlockDeviceRequest
{
	IOBufferMemoryDescriptor* header;
	SSDCMultiSubrangeMemoryDescriptor* buffer;
	IOBufferMemoryDescriptor* status;
	
};

IOReturn VirtioBlockDevice::doAsyncReadWrite(IOMemoryDescriptor* buffer, UInt64 block, UInt64 nblks, IOStorageAttributes* attributes, IOStorageCompletion* completion)
{
	IODirection direction = buffer->getDirection();
	if(buffer->getLength() < (nblks * block_size))
	{
		//buffer too small
		return kIOReturnBadArgument;
	}
	
	if((block + nblks)* block_size > capacity_in_bytes)
	{
		//bigger than the block
		return kIOReturnBadArgument;
	}
	
	if(direction == kIODirectionIn)
	{
		//Disk read
	}
	else if (direction == kIODirectionOut)
	{
		//Disk write
	}
	else
	{
		//can only be a read or a write cannot be both
		return kIOReturnBadArgument;
	}

	//VirtioCompletion completion = { &blockRequestCompleted, this };
	return kIOReturnError;
}

IOReturn VirtioBlockDevice::doEjectMedia(void)
{
	return kIOReturnSuccess;
}
IOReturn VirtioBlockDevice::doFormatMedia(UInt64 byteCapacity)
{
	return kIOReturnUnsupported;
}

UInt32 VirtioBlockDevice::doGetFormatCapacities(UInt64* capacities, UInt32 capacitiesMaxCount) const
{
	return 0;
}

IOReturn VirtioBlockDevice::doLockUnlockMedia(bool doLock)
{
	doLock = false;
	return kIOReturnSuccess;
}

char* VirtioBlockDevice::getVendorString(void)
{
	return (char*)"VirtioBlockDevice::getVendorString";
}

char* VirtioBlockDevice::getProductString(void)
{
	return (char*)"VirtioBlockDevice::getProductString";
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

