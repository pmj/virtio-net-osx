//
//  VirtioPCIDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#ifndef __virtio_osx__VirtioPCIDevice__
#define __virtio_osx__VirtioPCIDevice__

#include <IOKit/IOService.h>

#define VirtioPCIDevice eu_dennis__jordan_driver_VirtioPCIDevice

class VirtioPCIDevice : public IOService
{
	OSDeclareDefaultStructors(VirtioPCIDevice);
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
	
};

#endif /* defined(__virtio_osx__VirtioPCIDevice__) */
