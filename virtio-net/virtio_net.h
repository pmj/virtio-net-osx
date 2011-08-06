/* Copyright 2011 Phil Jordan <phil@philjordan.eu>
 * Dual-licensed under the 3-clause BSD License and the zLib license */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <IOKit/network/IOEthernetController.h>
#include "virtio_ring.h"

class IOBufferMemoryDescriptor;
class IOPCIDevice;
class IOEthernetInterface;
class IOMbufMemoryCursor;
class IOFilterInterruptEventSource;
class IOInterruptEventSource;

struct virtio_net_packet;

struct virtio_net_virtqueue : vring
{
	IOBufferMemoryDescriptor* buf;
	/* Number of free descriptors in the queue */
	uint16_t num_free_desc;
	/* The index of the head of the linked list of free descriptors. */
	uint16_t free_desc_head;
	
	uint16_t index;
	
	// When this differs from the queue's used index, pop buffers off the used queue to handle/recycle them
	uint16_t last_used_idx;
	
	// Array of pointers to packet structures corresponding to each live buffer descriptor.
	/* Length is virtqueue size. Used for retrieving the packet corresponding to
	 * a buffer chain as it comes off the 'used' ring. */
	virtio_net_packet** packets_for_descs;
};
/* Initialise the queue structure, then mark all descriptors as free (in a
 * linked list). Allocates the packets_for_descs array. */
void virtqueue_init(virtio_net_virtqueue& queue, IOBufferMemoryDescriptor* buf, uint16_t queue_size, uint16_t queue_id);
/// Releases the buffer, frees the packets_for_descs array and clears fields.
void virtqueue_free(virtio_net_virtqueue& queue);

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
	virtual const OSString* newVendorString() const;
	virtual const OSString* newModelString() const;

	virtual IOOutputQueue* createOutputQueue();

	virtual IOReturn enable(IONetworkInterface* interface);
	virtual IOReturn disable(IONetworkInterface* interface);
	
	virtual UInt32 outputPacket(mbuf_t, void *param);
	
	// polled-mode versions for kernel debugger
	virtual void receivePacket(void *pkt, UInt32 *pktSize, UInt32 timeout);
	virtual void sendPacket(void *pkt, UInt32 pktSize);

	virtual bool configureInterface(IONetworkInterface *netif);
	virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const;
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
	
	bool populateReceiveBuffers();

	bool notifyQueueAvailIdx(virtio_net_virtqueue& queue, uint16_t new_avail_idx);
	/// Overwrite the device/driver status register with the given value
	void setVirtioDeviceStatus(uint8_t status);
	/// Bitwise-or the device/driver status register with the given value
	void updateVirtioDeviceStatus(uint8_t status);
	
	/// Read network device status register; returns negative value if unsupported
	int32_t readStatus();
	
	/// Interrupt filter that checks whether the interrupt means work needs to be done
	static bool interruptFilter(OSObject* me, IOFilterInterruptEventSource* source);
	
	static void interruptAction(OSObject* me, IOInterruptEventSource* source, int count);
	void interruptAction(IOInterruptEventSource* source, int count);
	
	void handleReceivedPackets();
	
	/// The provider device. NOT retained.
	IOPCIDevice* pci_dev;
	/// Memory mapping of the virtio PCI configuration registers
	IOMemoryMap* pci_config_mmap;
	
	IOEthernetInterface* interface;
	
	virtio_net_virtqueue rx_queue;
	virtio_net_virtqueue tx_queue;
	
	IOEthernetAddress mac_address;
	/// 0 if reading status is not supported
	uint16_t status_field_offset;
	
	/// ISR status register value in last successful interrupt
	uint8_t last_isr;
	/// Toggled on if the configuration change bit was detected in the interrupt handler
	bool received_config_change;
	
	IOWorkLoop* work_loop;
	IOFilterInterruptEventSource* intr_event_source;
	
	/// Set of IOBufferMemoryDescriptor objects to be used as network packet header buffers
	OSSet* packet_bufdesc_pool;
};

#endif
