/* Copyright 2011, 2013 Phil Jordan <phil@philjordan.eu>
 * This code made available under the GNU LGPL
 * (GNU Library/Lesser General Public License)
 * A copy of this license can be found in the LICENSE file provided together
 * with this source file, or at https://www.gnu.org/licenses/lgpl.html if
 * that file is missing.
 *
 * For practical purposes, what does this mean?
 * - You may use, modify and/or run this code freely
 * - You may redistribute this code and the compiled kext, provided that
 *   copyright notices remain intact and that you make available the full source
 *   code, including any modifications.
 * - You may create and distribute other kexts with different licenses which
 *   depend upon this kext, as long as this kext remains independently loadable
 *   and modifiable by all users.
 *
 * If you require additional permissions not covered by this license, please
 * contact the author at phil@philjordan.eu - other licensing options are available.
 */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IODMACommand.h>
#include "../VirtioFamily/VirtioDevice.h"
#include "virtio_ring.h"

class IOBufferMemoryDescriptor;
class IOPCIDevice;
class IOEthernetInterface;
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

struct virtio_net_packet;

class PJVirtioNet : public IOEthernetController
{
	OSDeclareDefaultStructors(PJVirtioNet);

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
	virtual IOReturn enable(IOKernelDebugger *debugger);
	virtual IOReturn disable(IOKernelDebugger *debugger);
	virtual void receivePacket(void *pkt, UInt32 *pktSize, UInt32 timeout);
	virtual void sendPacket(void *pkt, UInt32 pktSize);

	virtual bool configureInterface(IONetworkInterface *netif);
	virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const;
	
	virtual UInt32 getFeatures() const;	
	virtual IOReturn getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput); 
	
	virtual IOReturn selectMedium(const IONetworkMedium* medium);
protected:
	bool startWithIOEnabled();

	/// Enable the device far enough to do debugging
	/** Initialises with no output queue, no interrupts - that is done by the
	 * interface version of enable().
	 */
	bool enablePartial();
	/// Revert enablePartial
	void disablePartial();
	/// Creates and publishes the table of possible media
	bool createMediumTable();

	template <typename P, IOReturn(PJVirtioNet::*fn)(P* p)> static IOReturn runMemberInCommandGateAction(OSObject* owner, void* param, void*, void*, void*)
	{
		PJVirtioNet* array = OSDynamicCast(PJVirtioNet, owner);
		assert(array);
		return (array->*fn)(static_cast<P*>(param));
	}
	template <typename P, IOReturn(PJVirtioNet::*fn)(P* p)> IOReturn runInCommandGate(P* p)
	{
		return getCommandGate()->runAction(runMemberInCommandGateAction<P, fn>, static_cast<void*>(p));
	}
	template <typename P, void(PJVirtioNet::*fn)(P p)> static IOReturn runMemberInCommandGateAction(OSObject* owner, void* param, void*, void*, void*)
	{
		PJVirtioNet* array = OSDynamicCast(PJVirtioNet, owner);
		assert(array);
		(array->*fn)(*static_cast<P*>(param));
		return kIOReturnSuccess;
	}
	template <typename P, void(PJVirtioNet::*fn)(P p)> IOReturn runInCommandGate(P p)
	{
		return getCommandGate()->runAction(runMemberInCommandGateAction<P, fn>, static_cast<void*>(&p));
	}

	IOReturn gatedEnableInterface(IONetworkInterface* interface);
	IOReturn gatedEnableDebugger(IOKernelDebugger* debugger);

	
	enum DriverState
	{
		// Error states:
		kDriverStateStartFailed = -10,
		kDriverStateStartFailedUnsupportedDevice,
		kDriverStateStartFailedOutOfMemory,
		kDriverStateEnableFailed,
		kDriverStateEnableFailedOutOfMemory,
		
		// Expected states:
		/// Before start(); we may have talked to the device in probe(), but nothing else
		kDriverStateInitial = 0,
		/// start() completed successfully, but enable() hasn't yet been called (or has been negated by disable())
		kDriverStateStarted,
		/// enable() has been called, and succeeded
		kDriverStateEnabled,
		/// enable() has been called with (only) a debugging client, and succeeded
		kDriverStateEnabledDebugging,
		/// enable() has been called for both a normal interface client and the debugger and succeeded
		kDriverStateEnabledBoth,
		/// stop() was called
		kDriverStateStopped
		
	};
	
	
	// Virtio configuration space functions called from start():
	
	/// Processes the various optional features which affect the layout of the generic virtio configuration header
	/** Returns where the standard configuration ends, and by implication, where
	 * the device-specific region starts. Sets dev_features_hi. */
	uint16_t virtioReadOptionalConfigFieldsGetDeviceSpecificOffset();
	/// Read out or generate the MAC address, depending on what the hardware decides
	void determineMACAddress();
	/// Checks link status, if possible, and records its configuration space offset for later updates
	void detectLinkStatusFeature();
	
	// Virtqueue management functions:
	
	/** Allocates/recycles the header buffer, sets up the packet data buffers,
	 * writes the virtqueue descriptors, and the head's packets_for_descs entry.
	 * Then fills out the 'available' ring buffer
	 * entry at the specified index (modulo queue size), incrementing said index
	 * as necessary. Returns true on success, false on failure. The mbuf is not
	 * freed in either case (but referenced as a buffer in case of success).
	 */
	IOReturn addPacketToQueue(mbuf_t packet_mbuf, virtio_net_virtqueue& queue, bool for_writing, uint16_t& at_avail_idx);
	virtio_net_packet* allocPacket();
	/// Segment output function for the DMA command, adds a physical memory segment to the buffer descriptor chain
	static bool outputPacketSegment(IODMACommand* target, IODMACommand::Segment64 segment, void* segments, UInt32 segmentIndex);

	void freeDescriptorChain(virtio_net_virtqueue& queue, uint16_t desc_chain_head);
	bool populateReceiveBuffers();

	bool notifyQueueAvailIdx(virtio_net_virtqueue& queue, uint16_t new_avail_idx);		
	
	/// Free any packet buffers in the now shut down queue
	void clearVirtqueuePackets(virtio_net_virtqueue& queue);
	

	/// Read network device status register; returns negative value if unsupported
	int32_t readStatus();
	/// Reads the link status and reports it back to the interface
	/** Returns true if the link is up, false if not. */
	bool updateLinkStatus();
	
	/// Creates and activates the interrupt event source
	/** Should be called as soon as the virtqueues become active, i.e. at the end of enable() */
	bool beginHandlingInterrupts();
	/// Deactivates and destroys the interrupt event source
	/** Should be called as soon as the virtqueues have been deactivated, but
	 * before they are cleared. (i.e. in disable()) */
	void endHandlingInterrupts();

	/// Interrupt filter that checks whether the interrupt means work needs to be done
	/** Also disables further interrupts until the initial interrupt has been handled.
	 */
	static bool interruptFilter(OSObject* me, IOFilterInterruptEventSource* source);
	
	/// Secondary interrupt. Forwards to the member function of the same name.
	static void interruptAction(OSObject* me, IOInterruptEventSource* source, int count);
	static void configChangeHandler(OSObject* target, VirtioDevice* source);

	static void receiveQueueCompletion(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
	static void transmitQueueCompletion(OSObject* target, void* ref, bool device_reset, uint32_t num_bytes_written);
	void interruptAction(IOInterruptEventSource* source, int count);
	
	void handleReceivedPacket(virtio_net_packet* packet);
	void handleReceivedPackets();
	
	/// Frees any packets marked as "used" in the transmit queue and frees their descriptors
	void releaseSentPackets(bool from_debugger = false);
	void releaseSentPacket(virtio_net_packet* packet);
	
	void flushPacketPool();

	DriverState driver_state;
	
	/// Whether or not the driver is permitted to negotiate any checksumming or offloading features
	bool pref_allow_offloading;
	static const bool pref_allow_offloading_default = true;
	
	/// The provider device. NOT retained.
	VirtioDevice* virtio_dev;
	
	/// The standard bit map of virtio device features
	uint32_t dev_feature_bitmap;
		
	IOEthernetInterface* interface;
	
	static const unsigned RECEIVE_QUEUE_INDEX = 0;
	static const unsigned TRANSMIT_QUEUE_INDEX = 1;

	virtio_net_virtqueue rx_queue;
	virtio_net_virtqueue tx_queue;
	
	IOEthernetAddress mac_address;
	/// Set to true once the mac address has been initialised
	/* The MAC address may be determined either by reading out the hardware register,
	 * or generated randomly. */
	bool mac_address_is_valid;
	
	/// VIRTIO__NET_F_STATUS feature has been negotiated
	bool feature_status_field;
	
	/// true if the device supports the VIRTIO_F_NOTIFY_ON_EMPTY feature
	bool feature_notify_on_empty;
	/// Checksum offloading has been negotiated
	bool feature_checksum_offload;
	/// TSO for IPv4 has been negotiated
	bool feature_tso_v4;
	
	
	IOWorkLoop* work_loop;
	IOFilterInterruptEventSource* intr_event_source;
	
	//IOMbufMemoryCursor* packet_memory_cursor;
	
	/// Set of IOBufferMemoryDescriptor objects to be used as network packet header buffers
	OSSet* packet_bufdesc_pool;
	
	/// The client object for the debugger
	IOKernelDebugger* debugger;
	/// A packet and associated mbuf reserved for transmitting packets supplied by the debugger
	virtio_net_packet* debugger_transmit_packet;
	/// Linked list of packets to be freed
	/** accumulated by the debugger dequeueing used tx packets */
	struct virtio_net_packet* transmit_packets_to_free;
	
	bool was_stalled;

	/// Low bit is atomically toggled on if the configuration change bit was detected in the interrupt handler
	volatile UInt8 received_config_change __attribute__((aligned(32)));
};

#endif
