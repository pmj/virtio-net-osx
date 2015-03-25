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
