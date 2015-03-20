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

OSDefineMetaClassAndStructors(VirtioLegacyPCIDevice, IOService);

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

	this->registerService();
	return true;
}

bool VirtioLegacyPCIDevice::matchPropertyTable(OSDictionary* table, SInt32* score)
{
	if (!IOService::matchPropertyTable(table, score))
	{
		IOLog("VirtioLegacyPCIDevice::matchPropertyTable(): IOService::matchPropertyTable() returned false\n");
		return false;
	}
	OSObject* deviceTypeIDValue = table->getObject("VirtioDeviceTypeID");
	if (deviceTypeIDValue != NULL)
	{
		OSNumber* deviceTypeIDNumber = OSDynamicCast(OSNumber, deviceTypeIDValue);
		if (deviceTypeIDNumber != NULL)
		{
			//have a number
			uint32_t requestedDeviceTypeID = deviceTypeIDNumber->unsigned32BitValue();
			if(this->virtio_device_type != requestedDeviceTypeID)
			{
				return false;
			}
			
		}
		else
		{
			//not a number
			IOLog("VirtioLegacyPCIDevice::matchPropertyTable: VirtioDeviceTypeID in IOKitPersonality must be number, a number was not found\n");
			return false;
		}
	}
	return true;
}


