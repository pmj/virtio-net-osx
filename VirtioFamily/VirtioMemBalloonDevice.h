//
//  VirtioMemBalloonDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 19/03/2015.
//
//

#ifndef __virtio_osx__VirtioMemBalloonDevice__
#define __virtio_osx__VirtioMemBalloonDevice__

#include <IOKit/IOService.h>
#define VirtioMemBalloonDevice eu_dennis__jordan_driver_VirtioMemBalloonDevice
class VirtioMemBalloonDevice : public IOService
{
	OSDeclareDefaultStructors(VirtioMemBalloonDevice);

};

#endif /* defined(__virtio_osx__VirtioMemBalloonDevice__) */
