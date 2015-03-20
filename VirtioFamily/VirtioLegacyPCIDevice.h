//
//  VirtioLegacyPCIDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#ifndef __virtio_osx__VirtioLegacyPCIDevice__
#define __virtio_osx__VirtioLegacyPCIDevice__

#include <IOKit/IOService.h>

#define VirtioLegacyPCIDevice eu_dennis__jordan_driver_VirtioLegacyPCIDevice

class VirtioLegacyPCIDevice : public IOService
{
	OSDeclareDefaultStructors(VirtioLegacyPCIDevice);
	
	uint32_t virtio_device_type;
	
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
	virtual bool start(IOService* provider) override;
    virtual bool matchPropertyTable(OSDictionary* table, SInt32* score) override;
	
};


#endif /* defined(__virtio_osx__VirtioLegacyPCIDevice__) */
