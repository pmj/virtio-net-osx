//
//  VirtioNetworkDevice.h
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 27/03/2015.
//
//

#ifndef __virtio_osx__VirtioNetworkDevice__
#define __virtio_osx__VirtioNetworkDevice__


#include "VirtioDevice.h"
#include <IOKit/network/IOEthernetController.h>
#define VirtioNetworkDevice eu_dennis__jordan_driver_VirtioNetworkDevice

class IOBufferMemoryDescriptor;
class VirtioNetworkDevice : public IOEthernetController
{
	OSDeclareDefaultStructors(VirtioNetworkDevice);

protected:
	VirtioDevice* virtio_device;
	OSArray* packet_bufdesc_pool;
	IOCommandGate* command_gate;

public:

	virtual bool start(IOService* provider) override;
	virtual void stop(IOService* provider) override;
	virtual void determineMACAddress(uint16_t device_specific_offset);

/*
	virtual void endDeviceOperation();
    virtual bool didTerminate( IOService * provider, IOOptionBits options, bool * defer ) override;
*/
	virtual bool updateLinkStatus();

	static void deviceConfigChangeAction(OSObject* target, VirtioDevice* source);
	virtual void deviceConfigChangeAction(VirtioDevice* source);
	/// The standard bit map of virtio device features
	uint32_t dev_feature_bitmap;
	
	/*bool should_disable_io;
	IOEthernetInterface* interface;
	virtio_net_virtqueue rx_queue;
	virtio_net_virtqueue tx_queue;
	*/
	IOEthernetAddress mac_address;
	/// Set to true once the mac address has been initialised
	/* The MAC address may be determined either by reading out the hardware register,
	 * or generated randomly. */
	bool mac_address_is_valid;

#ifdef VIRTIO_LOG_TERMINATION
	virtual bool requestTerminate( IOService * provider, IOOptionBits options ) override;
    virtual bool willTerminate( IOService * provider, IOOptionBits options ) override;
	virtual bool terminate( IOOptionBits options = 0 ) override;
	virtual bool terminateClient(IOService * client, IOOptionBits options) override;
#endif
};

#endif /* defined(__virtio_osx__VirtioNetworkDevice__) */
