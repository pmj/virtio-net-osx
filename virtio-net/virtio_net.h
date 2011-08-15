/* Copyright 2011 Phil Jordan <phil@philjordan.eu>
 * Triple-licensed under the zLib license, the 3-clause BSD License and the MIT License */

/*
	This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

AND/OR:

Copyright (c) 2011 Phil Jordan <phil@philjordan.eu>

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

AND/OR:

Copyright (c) 2011 Phil Jordan <phil@philjordan.eu>
 All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.
The name of Phil Jordan may NOT be used to endorse or promote products derived
from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <IOKit/network/IOEthernetController.h>
#include <IOKit/IOMemoryCursor.h>
#include <IOKit/IOCommandGate.h>
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
	/// Enable the device far enough to do debugging
	/** Initialises with no output queue, no interrupts - that is done by the
	 * interface version of enable().
	 */
	bool enablePartial();
	/// Revert enablePartial
	void disablePartial();
	/// Creates and publishes the table of possible media
	bool createMediumTable();

	template <typename P, IOReturn(eu_philjordan_virtio_net::*fn)(P* p)> static IOReturn runMemberInCommandGateAction(OSObject* owner, void* param, void*, void*, void*)
	{
		eu_philjordan_virtio_net* array = OSDynamicCast(eu_philjordan_virtio_net, owner);
		assert(array);
		return (array->*fn)(static_cast<P*>(param));
	}
	template <typename P, IOReturn(eu_philjordan_virtio_net::*fn)(P* p)> IOReturn runInCommandGate(P* p)
	{
		return getCommandGate()->runAction(runMemberInCommandGateAction<P, fn>, static_cast<void*>(p));
	}
	template <typename P, void(eu_philjordan_virtio_net::*fn)(P p)> static IOReturn runMemberInCommandGateAction(OSObject* owner, void* param, void*, void*, void*)
	{
		eu_philjordan_virtio_net* array = OSDynamicCast(eu_philjordan_virtio_net, owner);
		assert(array);
		(array->*fn)(*static_cast<P*>(param));
		return kIOReturnSuccess;
	}
	template <typename P, void(eu_philjordan_virtio_net::*fn)(P p)> IOReturn runInCommandGate(P p)
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
	bool setupVirtqueue(uint16_t queue_id, virtio_net_virtqueue& queue);
	/// Sets the "failed" bit in the device's status register
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
	
	// Universal virtio configuration space/status functions:
	
	/// Writes the 'reset' value to the device state register, thus re-inititalising it.
	void virtioResetDevice();
	/// Overwrite the device/driver status register with the given value
	void setVirtioDeviceStatus(uint8_t status);
	/// Bitwise-or the device/driver status register with the given value
	void updateVirtioDeviceStatus(uint8_t status);
	/// Resets the device, then initialise it far enough to read out the device feature register
	uint32_t virtioResetInitAndReadFeatureBits();

	// Virtio configuration space functions called from start():
	
	/// Creates the memory mapping for the virtio configuration space
	bool mapVirtioConfigurationSpace();
	/// Processes the various optional features which affect the layout of the generic virtio configuration header
	/** Returns where the standard configuration ends, and by implication, where
	 * the device-specific region starts. Sets dev_features_hi. */
	uint16_t virtioReadOptionalConfigFieldsGetDeviceSpecificOffset();
	/// Read out or generate the MAC address, depending on what the hardware decides
	void determineMACAddress(uint16_t device_specific_offset);
	/// Checks link status, if possible, and records its configuration space offset for later updates
	void detectLinkStatusFeature(uint16_t device_specific_offset);
	
	// Virtqueue management functions:
	
	/** Allocates/recycles the header buffer, sets up the packet data buffers,
	 * writes the virtqueue descriptors, and the head's packets_for_descs entry.
	 * Then fills out the 'available' ring buffer
	 * entry at the specified index (modulo queue size), incrementing said index
	 * as necessary. Returns true on success, false on failure. The mbuf is not
	 * freed in either case (but referenced as a buffer in case of success).
	 */
	IOReturn addPacketToQueue(mbuf_t packet_mbuf, virtio_net_virtqueue& queue, bool for_writing, uint16_t& at_avail_idx);
	IOBufferMemoryDescriptor* allocPacketHeaderBuffer();
	/// Segment output function for the memory cursor, adds a physical memory segment to the buffer descriptor chain
	static void outputPacketSegment(IOMemoryCursor::PhysicalSegment segment, void* segments, UInt32 segmentIndex);

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
	/// Actually performs
	void interruptAction(IOInterruptEventSource* source, int count);
	
	void handleReceivedPackets();
	
	/// Frees any packets marked as "used" in the transmit queue and frees their descriptors
	void releaseSentPackets(bool from_debugger = false);
	
	DriverState driver_state;
	
	// Settings from the info.plist personality section (set in init())
	/// Maximum number of physical data sections in a transmit packet
	uint16_t pref_max_tx_data_segs;
	static const uint16_t pref_max_tx_data_segs_default = 1;
	
	/// Whether or not the driver is permitted to negotiate any checksumming or offloading features
	bool pref_allow_offloading;
	static const bool pref_allow_offloading_default = true;
	
	/// The provider device. NOT retained.
	IOPCIDevice* pci_dev;
	/// Memory mapping of the virtio PCI configuration registers
	IOMemoryMap* pci_config_mmap;
	
	/// The standard bit map of virtio device features
	uint32_t dev_features_lo;
	/// The extended bit map of virtio device features (or 0 if the high bits aren't available)
	/** Initialised at start() by virtioReadOptionalConfigFieldsGetDeviceSpecificOffset() */
	uint32_t dev_features_hi;
	
	IOEthernetInterface* interface;
	
	virtio_net_virtqueue rx_queue;
	virtio_net_virtqueue tx_queue;
	
	IOEthernetAddress mac_address;
	/// Set to true once the mac address has been initialised
	/* The MAC address may be determined either by reading out the hardware register,
	 * or generated randomly. */
	bool mac_address_is_valid;
	
	/// 0 if reading status is not supported
	uint16_t status_field_offset;
	
	/// ISR status register value in last successful interrupt
	uint8_t last_isr;
	
	/// true if the device supports the VIRTIO_F_NOTIFY_ON_EMPTY feature
	bool feature_notify_on_empty;
	/// Checksum offloading has been negotiated
	bool feature_checksum_offload;
	/// TSO for IPv4 has been negotiated
	bool feature_tso_v4;
	
	
	IOWorkLoop* work_loop;
	IOFilterInterruptEventSource* intr_event_source;
	
	IOMbufMemoryCursor* packet_memory_cursor;
	
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
