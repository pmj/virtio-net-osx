//
//  VirtioDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 20/03/2015.
//
//

#include "VirtioDevice.h"
#include <IOKit/IOLib.h>

OSDefineMetaClassAndAbstractStructors(VirtioDevice, IOService);

OSMetaClassDefineReservedUnused(VirtioDevice, 0)
OSMetaClassDefineReservedUnused(VirtioDevice, 1)
OSMetaClassDefineReservedUnused(VirtioDevice, 2)
OSMetaClassDefineReservedUnused(VirtioDevice, 3)
OSMetaClassDefineReservedUnused(VirtioDevice, 4)
OSMetaClassDefineReservedUnused(VirtioDevice, 5)
OSMetaClassDefineReservedUnused(VirtioDevice, 6)
OSMetaClassDefineReservedUnused(VirtioDevice, 7)
OSMetaClassDefineReservedUnused(VirtioDevice, 8)
OSMetaClassDefineReservedUnused(VirtioDevice, 9)
OSMetaClassDefineReservedUnused(VirtioDevice, 10)
OSMetaClassDefineReservedUnused(VirtioDevice, 11)
OSMetaClassDefineReservedUnused(VirtioDevice, 12)
OSMetaClassDefineReservedUnused(VirtioDevice, 13)
OSMetaClassDefineReservedUnused(VirtioDevice, 14)
OSMetaClassDefineReservedUnused(VirtioDevice, 15)
OSMetaClassDefineReservedUnused(VirtioDevice, 16)
OSMetaClassDefineReservedUnused(VirtioDevice, 17)
OSMetaClassDefineReservedUnused(VirtioDevice, 18)
OSMetaClassDefineReservedUnused(VirtioDevice, 19)

bool VirtioDevice::matchPropertyTable(OSDictionary* table, SInt32* score)
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
