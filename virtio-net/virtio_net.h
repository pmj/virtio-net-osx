/* Copyright 2011 Phil Jordan <phil@philjordan.eu>
 * Dual-licensed under the 3-clause BSD License and the zLib license */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <IOKit/network/IOEthernetController.h>
#include "virtio_ring.h"

class IOBufferMemoryDescriptor;
class IOPCIDevice;

struct virtio_net_virtqueue : vring
{
	IOBufferMemoryDescriptor* buf;	
};
void virtqueue_init(virtio_net_virtqueue& queue, IOBufferMemoryDescriptor* buf, uint16_t queue_size);

class eu_philjordan_virtio_net : public IOEthernetController
{
	OSDeclareDefaultStructors(eu_philjordan_virtio_net);

public:
	/// Constructor. Will eventually initialise various internal state.
	virtual bool init(OSDictionary * properties);
	/// Called after device matching. Verifies that the device (the provider) is in fact a device supported by this driver.
	virtual IOService* probe(IOService* provider, SInt32* score);

	/// Initialises the device.
	/** Right now, this is only basic PCI stuff, eventually this should also bring
	 * up an ethernet interface.
	 */
	virtual bool start(IOService* provider);
	virtual void stop(IOService* provider);
	
	/// Destructor, frees internal state
	virtual void free();
	
	virtual IOReturn getHardwareAddress(IOEthernetAddress* addrP);

protected:
	bool setupVirtqueue(uint16_t queue_id, virtio_net_virtqueue& queue);
	void failDevice();
	void configWrite8(uint16_t offset, uint8_t val);
	void configWrite16(uint16_t offset, uint16_t val);
	void configWrite32(uint16_t offset, uint32_t val);
	uint8_t configRead8(uint16_t offset);
	uint16_t configRead16(uint16_t offset);
	uint32_t configRead32(uint16_t offset);
	void configWriteLE32(uint16_t offset, uint32_t val);
	uint32_t configReadLE32(uint16_t offset);
	void configWriteLE16(uint16_t offset, uint16_t val);
	uint16_t configReadLE16(uint16_t offset);
	
	/// Overwrite the device/driver status register with the given value
	void setVirtioDeviceStatus(uint8_t status);
	/// Bitwise-or the device/driver status register with the given value
	void updateVirtioDeviceStatus(uint8_t status);
	
	/// Read network device status register; returns negative value if unsupported
	int32_t readStatus();
	
	/// The provider device. NOT retained.
	IOPCIDevice* pci_dev;
	/// Memory mapping of the virtio PCI configuration registers
	IOMemoryMap* pci_config_mmap;
	
	virtio_net_virtqueue rx_queue;
	virtio_net_virtqueue tx_queue;
	
	IOEthernetAddress mac_address;
	/// 0 if reading status is not supported
	uint16_t status_field_offset;
};

#endif
