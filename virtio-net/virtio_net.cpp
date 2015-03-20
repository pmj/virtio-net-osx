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


#include "virtio_net.h"
#include "PJMbufMemoryDescriptor.h"
#include "SSDCMultiSubrangeMemoryDescriptor.h"
#include <IOKit/pci/IOPCIDevice.h>
#include "virtio_ring.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IOMbufMemoryCursor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <net/ethernet.h>

// darwin doesn't have inttypes.h TODO: build an inttypes.h for the kernel
#ifndef PRIuPTR
#define PRIuPTR "lu"
#endif
#ifndef PRIXPTR
#define PRIXPTR "lX"
#endif
#ifndef PRIX64
#define PRIX64 "llX"
#endif
#ifndef PRId64
#define PRId64 "lld"
#endif

OSDefineMetaClassAndStructors(PJVirtioNet, IOEthernetController);
#define super IOEthernetController

//#define PJ_VIRTIO_NET_VERBOSE
#define VIOLog IOLog


#ifndef PJ_VIRTIO_NET_VERBOSE
#define PJLogVerbose(...) ({ (void)0; })
#else
#define PJLogVerbose(...) VIOLog(__VA_ARGS__)
#endif

/*
#undef OSSafeReleaseNULL
#define OSSafeReleaseNULL(inst)   do { if (inst) { if (inst->getRetainCount() == 1) { VIOLog("virtio-net OSSafeReleaseNULL %s:%u: releasing %p (%lu+ bytes)\n", __FILE__, __LINE__, (inst), sizeof(*(inst))); } (inst)->release(); } (inst) = NULL; } while (0)
//*/

template <typename T> static T* PJZMallocArray(size_t length)
{
	const size_t bytes = sizeof(T) * length;
	void* const mem = IOMalloc(bytes);
	if (!mem) return NULL;
	PJLogVerbose("virtio-net: allocated %lu bytes at %p\n", bytes, mem);
	memset(mem, 0, bytes);
	return static_cast<T*>(mem);
}

template <typename T> static void PJFreeArray(T* array, size_t length)
{
	const size_t bytes = sizeof(T) * length;
	PJLogVerbose("virtio-net: freeing %lu bytes at %p\n", bytes, array);
	IOFree(array, bytes);
}

template <typename T> static T* PJZMalloc()
{
	const size_t bytes = sizeof(T);
	void* const mem = IOMalloc(bytes);
	if (!mem) return NULL;
	memset(mem, 0, bytes);
	return static_cast<T*>(mem);
}
template <typename T> static void PJFree(T* obj)
{
	const size_t bytes = sizeof(T);
	IOFree(obj, bytes);
}


static inline bool is_pow2(uint16_t num)
{
	return 0u == (num & (num - 1));
}

static void virtio_net_log_property_dict(OSDictionary* props)
{
	VIOLog("virtio-net: begin property dictionary:\n");
	if (props)
	{
		OSCollectionIterator* it = OSCollectionIterator::withCollection(props);
		if (it)
		{
			while (true)
			{
				OSObject* key = it->getNextObject();
				if (!key)
					break;
				OSString* keystr = OSDynamicCast(OSString, key);
				OSObject* val = props->getObject(keystr);
				OSString* str = OSDynamicCast(OSString, val);
				OSNumber* num = OSDynamicCast(OSNumber, val);
				if (str)
				{
					VIOLog("%s -> '%s'\n", keystr->getCStringNoCopy(), str->getCStringNoCopy());
				}
				else if (num)
				{
					VIOLog("%s -> %llu\n", keystr->getCStringNoCopy(), num->unsigned64BitValue());
				}
				else if (val)
				{
					VIOLog("%s -> [%s]\n", keystr->getCStringNoCopy(), val->getMetaClass()->getClassName());
				}
				else
				{
					VIOLog("%s -> null\n", keystr->getCStringNoCopy());
				}
			}
			it->release();
		}
	}
	VIOLog("virtio-net: end property dictionary\n");
}

#ifdef VIRTIO_NET_SINGLE_INSTANCE
static SInt32 instances = 0;
#endif

bool PJVirtioNet::init(OSDictionary* properties)
{
#ifdef VIRTIO_NET_SINGLE_INSTANCE
	if (OSIncrementAtomic(&instances) > 0)
		return false;
#endif
	static bool has_shown_copyright_notice = false;
	if (!has_shown_copyright_notice)
	{
		VIOLog("virtio-net driver: Copyright 2011, 2013 Phil Jordan <phil@philjordan.eu>; all rights reserved. Built %s %s.\n"
			"virtio specification and header: Copyright 2007, 2009, IBM Corporation and Copyright 2011, Red Hat, Inc; all rights reserved.\n"
			"For details, see the LICENSE and readme.md files in virtio-net KEXT bundle.\n", __DATE__, __TIME__);
		has_shown_copyright_notice = true;
	}

	PJLogVerbose("virtio-net init()\n");
	bool ok = super::init(properties);
	if (!ok)
		return false;
		
	OSBoolean* allow_offloading_val = NULL;
	if (properties && ((allow_offloading_val = OSDynamicCast(OSBoolean, properties->getObject("PJVirtioNetAllowOffloading")))))
	{
		pref_allow_offloading = allow_offloading_val->getValue();
		VIOLog("virtio-net: Offloading checksumming and segmentation %sALLOWED by plist preferences.\n", pref_allow_offloading ? "" : "DIS");
	}
	else
	{
		pref_allow_offloading = pref_allow_offloading_default;
	}
	//virtio_net_log_property_dict(properties);
	
	transmit_packets_to_free = NULL;
	driver_state = kDriverStateInitial;
	
	packet_bufdesc_pool = OSSet::withCapacity(16);
	if (!packet_bufdesc_pool)
		return false;
		
	pci_dev = NULL;
	rx_queue.buf = NULL;
	this->pci_virtio_header_iomap = NULL;
	return true;
}

static int64_t pci_id_data_to_uint(OSObject* property_obj)
{
	OSData* data = OSDynamicCast(OSData, property_obj);
	if (!data)
		return -1;
	
	int64_t id = -1;
	const void* mem = data->getBytesNoCopy();
	if (data->getLength() >= 4)
	{
		id = static_cast<uint32_t>(OSReadLittleInt32(mem, 0));
	}
	else if (data->getLength() == 2)
	{
		id = static_cast<uint16_t>(OSReadLittleInt16(mem, 0));
	}
	return id;
}

namespace
{
	extern const size_t VIRTIO_PCI_HEADER_MIN_LEN;
}

IOService* PJVirtioNet::probe(IOService* provider, SInt32* score)
{
	PJLogVerbose("virtio-net probe()\n");
	IOPCIDevice* pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (!pci_dev)
		return NULL;
	
	if (driver_state != kDriverStateInitial)
		VIOLog("virtio-net probe(): Warning: Unexpected driver state %d\n", driver_state);
	
	OSObject* vendor_id = pci_dev->getProperty("vendor-id");
	OSObject* device_id = pci_dev->getProperty("device-id");
	OSObject* revision_id = pci_dev->getProperty("revision-id");
	OSObject* subsystem_vendor_id = pci_dev->getProperty("subsystem-vendor-id");
	OSObject* subsystem_id = pci_dev->getProperty("subsystem-id");
	
	int64_t vid = -1, did = -1, revid = -1, sub_id = -1, sub_vid = -1;
	vid = pci_id_data_to_uint(vendor_id);
	PJLogVerbose("virtio-net probe(): vendor ID = %lld (0x%llX)\n", vid, vid);
	did = pci_id_data_to_uint(device_id);
	PJLogVerbose("virtio-net probe(): device ID = %lld (0x%llX)\n", did, did);
	revid = pci_id_data_to_uint(revision_id);
	PJLogVerbose("virtio-net probe(): revision ID = %lld (0x%llX)\n", revid, revid);
	sub_id = pci_id_data_to_uint(subsystem_id);
	PJLogVerbose("virtio-net probe(): subsystem ID = %lld (0x%llX)\n", sub_id, sub_id);
	sub_vid = pci_id_data_to_uint(subsystem_vendor_id);
	PJLogVerbose("virtio-net probe(): subsystem vendor ID = %lld (0x%llX)\n", sub_vid, sub_vid);
		
	if (vid != 0x1AF4)
	{
		VIOLog("virtio-net probe(): Vendor ID does not match 0x1AF4, device unsupported.\n");
		return NULL;
	}
	if (did < 0x1000 || did > 0x103F)
	{
		VIOLog("virtio-net probe(): Device ID does not lie in the range 0x1000 to 0x103F (inclusive), device unsupported.\n");
		return NULL;
	}
	if (revid != 0)
	{
		VIOLog("virtio-net probe(): Only virtio devices with revision ID 0 are supported by this driver, this one has revision %" PRId64 ".\n", revid);
		return NULL;
	}
	if (sub_id != 1)
	{
		VIOLog("Subsystem ID for device is %" PRId64 "Only virtio devices with subsystem ID 1 (= network card) are supported by this driver.\n", sub_id);
		return NULL;
	}
	if (sub_vid != vid)
	{
		VIOLog("Warning: subsystem vendor ID (0x%04X) should normally match device vendor ID (0x%04X).\n", (unsigned)sub_vid, (unsigned)vid);
	}

	// check the BAR0 range is in the I/O space and has the right minimum length
	if (0 == (kIOPCIIOSpace & pci_dev->configRead32(kIOPCIConfigBaseAddress0))) // is there a higher-level way of doing this?
	{
		VIOLog("virtio-net probe(): BAR0 indicates the first device range is in the memory address space, this driver expects an I/O range.\n");
		return NULL;
	}
	if (IODeviceMemory* header_range = pci_dev->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0))
	{
		size_t header_len = header_range->getLength();
		if (header_len < VIRTIO_PCI_HEADER_MIN_LEN)
		{
			IOLog("virtio-net probe(): Virtio header I/O range too short. Expected at least %lu bytes, got %lu\n", VIRTIO_PCI_HEADER_MIN_LEN, header_len);
			return NULL;
		}
	}
	else
	{
		IOLog("virtio-net probe(): Failed to get virtio header I/O range\n");
		return NULL;
	}

	return this;
}

enum VirtioPCIHeaderOffsets
{
	VIRTIO_PCI_CONF_OFFSET_DEVICE_FEATURE_BITS_0_31 = 0,
	VIRTIO_PCI_CONF_OFFSET_GUEST_FEATURE_BITS_0_31 = 4 + VIRTIO_PCI_CONF_OFFSET_DEVICE_FEATURE_BITS_0_31,
	VIRTIO_PCI_CONF_OFFSET_QUEUE_ADDRESS = 4 + VIRTIO_PCI_CONF_OFFSET_GUEST_FEATURE_BITS_0_31,
	VIRTIO_PCI_CONF_OFFSET_QUEUE_SIZE = 4 + VIRTIO_PCI_CONF_OFFSET_QUEUE_ADDRESS,
	VIRTIO_PCI_CONF_OFFSET_QUEUE_SELECT = 2 + VIRTIO_PCI_CONF_OFFSET_QUEUE_SIZE,
	VIRTIO_PCI_CONF_OFFSET_QUEUE_NOTIFY = 2 + VIRTIO_PCI_CONF_OFFSET_QUEUE_SELECT,
	VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS = 2 + VIRTIO_PCI_CONF_OFFSET_QUEUE_NOTIFY,
	VIRTIO_PCI_CONF_OFFSET_ISR_STATUS = 1 + VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS,
	VIRTIO_PCI_CONF_OFFSET_END_HEADER = 1 + VIRTIO_PCI_CONF_OFFSET_ISR_STATUS
};

namespace {
	const size_t VIRTIO_PCI_HEADER_MIN_LEN = VIRTIO_PCI_CONF_OFFSET_END_HEADER;
}
#define VIRTIO_PCI_DEVICE_ISR_USED 0x01
#define VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE 0x02

/// Virtio Spec 0.9.5, section 2.2.2.1, "Device Status".
/** To be used in the "Device Status" configuration field. */
enum VirtioPCIDeviceStatus
{
	VIRTIO_PCI_DEVICE_STATUS_RESET = 0x00,
	VIRTIO_PCI_DEVICE_STATUS_ACKNOWLEDGE = 0x01,
	VIRTIO_PCI_DEVICE_STATUS_DRIVER = 0x02,
	VIRTIO_PCI_DEVICE_STATUS_DRIVER_OK = 0x04,
	VIRTIO_PCI_DEVICE_STATUS_FAILED = 0x80
};

/// Virtio Spec 0.9, Appendix B, "Reserved Feature Bits".
/** We retain the names used in the spec as bitfield constants. */
enum VirtioPCIFeatureBits
{
	// virtio-net features
	VIRTIO_NET_F_CSUM = (1u << 0u),       // Device handles packets with partial checksum
	VIRTIO_NET_F_GUEST_CSUM = (1u << 1u), // Guest handles packets with partial checksum
	VIRTIO_NET_F_MAC = (1u << 5u),        // Device has given MAC address.
	VIRTIO_NET_F_GSO = (1u << 6u),        // (Deprecated) device handles packets with any GSO type.
	
	VIRTIO_NET_F_GUEST_TSO4 = (1u << 7u), // Guest can receive TSOv4.
	VIRTIO_NET_F_GUEST_TSO6 = (1u << 8u), // Guest can receive TSOv6.
	VIRTIO_NET_F_GUEST_ECN = (1u << 9u),  // Guest can receive TSO with ECN.
	VIRTIO_NET_F_GUEST_UFO = (1u << 10),  //Guest can receive UFO.
	VIRTIO_NET_F_HOST_TSO4 = (1u << 11),  // Device can receive TSOv4.
	
	VIRTIO_NET_F_HOST_TSO6 = (1u << 12),  // Device can receive TSOv6.
	VIRTIO_NET_F_HOST_ECN = (1u << 13),   // Device can receive TSO with ECN.
	VIRTIO_NET_F_HOST_UFO = (1u << 14),   // Device can receive UFO.
	VIRTIO_NET_F_MRG_RXBUF = (1u << 15),  // Guest can merge receive buffers.
	VIRTIO_NET_F_STATUS = (1u << 16),     // Configuration status field is available.
	VIRTIO_NET_F_CTRL_VQ = (1u << 17),    // Control channel is available.
	VIRTIO_NET_F_CTRL_RX = (1u << 18),    // Control channel RX mode support.
	VIRTIO_NET_F_CTRL_VLAN = (1u << 19),  // Control channel VLAN filtering.
	
	VIRTIO_NET_F_CTRL_RX_EXTRA = (1u << 20),  // Not in spec, "Extra RX mode control support"
	VIRTIO_NET_F_GUEST_ANNOUNCE = (1u << 21), // Guest can send gratuitous packets (announce itself upon request)
	
	// generic virtio features
	VIRTIO_F_NOTIFY_ON_EMPTY = (1u << 24u),
	VIRTIO_F_RING_INDIRECT_DESC = (1u << 28u),
	VIRTIO_F_RING_EVENT_IDX = (1u << 29u),
	
	// the following values have disappeared from the spec between 0.9 and 0.9.5 but are kept here for logging purposes
	VIRTIO_F_BAD_FEATURE = (1u << 30u),
	/// "high" features are supported
	VIRTIO_F_FEATURES_HIGH = (1u << 31u),
	
	VIRTIO_ALL_KNOWN_FEATURES =
		VIRTIO_NET_F_CSUM | VIRTIO_NET_F_GUEST_CSUM | VIRTIO_NET_F_MAC
		| VIRTIO_NET_F_GSO | VIRTIO_NET_F_GUEST_TSO4 | VIRTIO_NET_F_GUEST_TSO6
		| VIRTIO_NET_F_GUEST_ECN | VIRTIO_NET_F_GUEST_UFO | VIRTIO_NET_F_HOST_TSO4
		| VIRTIO_NET_F_HOST_TSO6 | VIRTIO_NET_F_HOST_ECN | VIRTIO_NET_F_HOST_UFO
		| VIRTIO_NET_F_MRG_RXBUF | VIRTIO_NET_F_STATUS | VIRTIO_NET_F_CTRL_VQ
		| VIRTIO_NET_F_CTRL_RX | VIRTIO_NET_F_CTRL_VLAN
		| VIRTIO_NET_F_CTRL_RX_EXTRA | VIRTIO_NET_F_GUEST_ANNOUNCE
		|	VIRTIO_F_NOTIFY_ON_EMPTY | VIRTIO_F_RING_INDIRECT_DESC
		| VIRTIO_F_RING_EVENT_IDX | VIRTIO_F_BAD_FEATURE | VIRTIO_F_FEATURES_HIGH
};

// bitfield value for virtio_net_config::status
static const uint16_t VIRTIO_NET_S_LINK_UP = 1;
struct virtio_net_config
{
	uint8_t  mac[6];
	uint16_t status;
};


#define VIRTIO_NET_HDR_F_NEEDS_CSUM 1
#define VIRTIO_NET_HDR_GSO_NONE 0 
#define VIRTIO_NET_HDR_GSO_TCPV4 1
#define VIRTIO_NET_HDR_GSO_UDP 3
#define VIRTIO_NET_HDR_GSO_TCPV6 4
#define VIRTIO_NET_HDR_GSO_ECN 0x80

struct virtio_net_hdr
{
	uint8_t flags;
	uint8_t gso_type;
	uint16_t hdr_len;
	uint16_t gso_size;
	uint16_t csum_start;
	uint16_t csum_offset;
	/* Only if	VIRTIO_NET_F_MRG_RXBUF: */
	uint16_t num_buffers[];
};

struct virtio_net_packet
{
	union
	{
		// Used as the first virtqueue buffer.
		virtio_net_hdr header;
		// When dequeued by the debugger, the packet is not freed but simply linked to the
		virtio_net_packet* next_free;
	};
	// The mbuf used for the packet body
	mbuf_t mbuf;
	// The memory descriptor holding this packet structure
	IOBufferMemoryDescriptor* mem;
	/// Memory descriptor for the mbuf's data
	PJMbufMemoryDescriptor* mbuf_md;
	/// Memory descriptor combining the tx/rx header buffer and mbuf
	SSDCMultiSubrangeMemoryDescriptor* dma_md;
	
	IODMACommand* dma_cmd;
	
	SSDCMemoryDescriptorSubrange dma_md_subranges[2];
};


static void log_feature(uint32_t feature_bitmap, uint32_t feature, const char* feature_name)
{
	if (feature_bitmap & feature)
	{
		VIOLog("%s\n", feature_name);
	}
}


/// Virtqueue size calculation, see section 2.3 in virtio spec
static const size_t VIRTIO_PAGE_SIZE = 4096;
static inline size_t virtio_page_align(size_t size)
{
	return (size + VIRTIO_PAGE_SIZE - 1u) & ~(VIRTIO_PAGE_SIZE - 1u);
}
static inline size_t vring_size(size_t qsz)
{
	return virtio_page_align(sizeof(vring_desc) * qsz + sizeof(uint16_t) * (2 + qsz))
		+ virtio_page_align(sizeof(vring_used_elem) * qsz);
}

#define LOG_FEATURE(FEATURES, FEATURE) \
log_feature(FEATURES, FEATURE, #FEATURE)

static void virtio_log_supported_features(uint32_t dev_features)
{
	VIOLog("virtio-net: Device reports LOW feature bitmap 0x%08x.\n", dev_features);
	VIOLog("virtio-net: Recognised generic virtio features:\n");
	LOG_FEATURE(dev_features, VIRTIO_F_NOTIFY_ON_EMPTY);    // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_F_RING_INDIRECT_DESC); // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_F_RING_EVENT_IDX);     // Supported by Qemu 1.3

	// legacy bits, no longer in the 0.9.5 spec, but log them if they do turn up
	LOG_FEATURE(dev_features, VIRTIO_F_BAD_FEATURE);        // Must mask this out
	LOG_FEATURE(dev_features, VIRTIO_F_FEATURES_HIGH);
	
	VIOLog("virtio-net: Recognised virtio-net specific features:\n");
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CSUM);           // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_CSUM);     // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_MAC);            // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GSO);            // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_TSO4);     // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_TSO6);     // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_ECN);      // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_UFO);      // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_TSO4);      // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_TSO6);      // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_ECN);       // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_UFO);       // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_MRG_RXBUF);      // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_STATUS);         // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_VQ);        // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_RX);        // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_VLAN);      // Supported by VBox 4.1.0, Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_RX_EXTRA);  // Supported by Qemu 1.3
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_ANNOUNCE);

	
	
	uint32_t unrecognised = dev_features & ~static_cast<uint32_t>(VIRTIO_ALL_KNOWN_FEATURES);
	if (unrecognised > 0)
	{
		VIOLog("Feature bits not recognised by this driver: 0x%08x\n", unrecognised);
	}
}

// Helper functions for reading/writing the virtio header registers


void PJVirtioNet::virtioHeaderWrite8(uint16_t offset, uint8_t val)
{
	pci_dev->ioWrite8(offset, val, pci_virtio_header_iomap);
}
void PJVirtioNet::virtioHeaderWrite16(uint16_t offset, uint16_t val)
{
	pci_dev->ioWrite16(offset, val, pci_virtio_header_iomap);
}
void PJVirtioNet::virtioHeaderWrite32(uint16_t offset, uint32_t val)
{
	pci_dev->ioWrite32(offset, val, pci_virtio_header_iomap);
}
uint8_t PJVirtioNet::virtioHeaderRead8(uint16_t offset)
{
	return pci_dev->ioRead8(offset, pci_virtio_header_iomap);
}
uint16_t PJVirtioNet::virtioHeaderRead16(uint16_t offset)
{
	return pci_dev->ioRead16(offset, pci_virtio_header_iomap);
}
uint32_t PJVirtioNet::virtioHeaderRead32(uint16_t offset)
{
	return pci_dev->ioRead32(offset, pci_virtio_header_iomap);
}

void PJVirtioNet::virtioHeaderWriteLE32(uint16_t offset, uint32_t val)
{
	virtioHeaderWrite32(offset, OSSwapHostToLittleInt32(val));
}
uint32_t PJVirtioNet::virtioHeaderReadLE32(uint16_t offset)
{
	return OSSwapLittleToHostInt32(virtioHeaderRead32(offset));
}
void PJVirtioNet::virtioHeaderWriteLE16(uint16_t offset, uint16_t val)
{
	virtioHeaderWrite16(offset, OSSwapHostToLittleInt16(val));
}
uint16_t PJVirtioNet::virtioHeaderReadLE16(uint16_t offset)
{
	return OSSwapLittleToHostInt16(virtioHeaderRead16(offset));
}


void PJVirtioNet::setVirtioDeviceStatus(uint8_t status)
{
	virtioHeaderWrite8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, status);
}
void PJVirtioNet::updateVirtioDeviceStatus(uint8_t status)
{
	uint8_t old_status = virtioHeaderRead8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS);
	setVirtioDeviceStatus(status | old_status);
}

void PJVirtioNet::failDevice()
{
	if (pci_dev)
	{
		if (this->intr_event_source)
			endHandlingInterrupts();
		if (this->pci_virtio_header_iomap)
		{
			updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_FAILED);
			OSSafeReleaseNULL(this->pci_virtio_header_iomap);
		}
		pci_dev->close(this);
	}
}

bool PJVirtioNet::interruptFilter(OSObject* me, IOFilterInterruptEventSource* source)
{
	// deliberately minimalistic function, as it will be called from an interrupt
	PJVirtioNet* virtio_net = OSDynamicCast(PJVirtioNet, me);
	if (!virtio_net || source != virtio_net->intr_event_source)
		return false; // this isn't really for us
	
	// check if anything interesting has happened, record status register
	uint8_t isr = virtio_net->virtioHeaderRead8(VIRTIO_PCI_CONF_OFFSET_ISR_STATUS);
	virtio_net->last_isr = isr;
	if (isr & VIRTIO_PCI_DEVICE_ISR_USED)
	{
		virtio_net->last_isr = isr;
		if (isr & VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE)
		{
			OSTestAndSet(0, &virtio_net->received_config_change);
		}
		
		// disable further interrupts until the handler has run
		virtio_net->rx_queue.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
		virtio_net->tx_queue.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
		return true;
	}
	return false;
}

void PJVirtioNet::interruptAction(OSObject* me, IOInterruptEventSource* source, int count)
{
	PJVirtioNet* virtio_net = OSDynamicCast(PJVirtioNet, me);
	if (!virtio_net || source != virtio_net->intr_event_source)
		return;
	
	virtio_net->interruptAction(source, count);
}

bool PJVirtioNet::updateLinkStatus()
{
	// Link status may have changed
	int32_t status = readStatus();
	bool link_is_up = (status & VIRTIO_NET_S_LINK_UP) != 0;

	const OSDictionary* dict = getMediumDictionary();
	IONetworkMedium* medium = dict ? IONetworkMedium::getMediumWithType(dict, kIOMediumEthernetAuto) : 0;
	if (!medium)
		VIOLog("virtio-net updateLinkStatus: Warning, no medium found!\n");
	setLinkStatus((link_is_up ? kIONetworkLinkActive : 0) | kIONetworkLinkValid, medium);
	return link_is_up;
}

void PJVirtioNet::interruptAction(IOInterruptEventSource* source, int count)
{
	//VIOLog("Last ISR value: 0x%02X\n", last_isr);
	uint8_t unknown_isr = last_isr & ~(VIRTIO_PCI_DEVICE_ISR_USED | VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE);
	if (unknown_isr)
	{
		VIOLog("virtio-net interruptAction(): Unknown bits set in ISR status register: %02X\n", unknown_isr);
	}
	
	bool has_reenabled_interrupts = false;
	
	while (true)
	{
		bool config_change = !OSTestAndClear(0, &received_config_change);
		if (config_change)
		{
			if (status_field_offset > 0)
			{
				bool up = updateLinkStatus();
				VIOLog("virtio-net interruptAction: Link change detected, link is now %s.\n", up ? "up" : "down");
			}
			else
			{
				VIOLog("virtio-net interruptAction(): received a configuration change! (currently unhandled)\n");
			}
		}

		// Handle received packets
		if (rx_queue.last_used_idx != rx_queue.used->idx)
		{
			handleReceivedPackets();
			/*
			IOLog("virtio-net interruptAction(): Populated receive buffers: %u free descriptors left, avail idx %u\n",
				rx_queue.num_free_desc, rx_queue.avail->idx);
			*/
		}
		
		// Ensure there are plenty of receive buffers
		populateReceiveBuffers();
		
		// Dispose of any completed sent packets
		if (tx_queue.last_used_idx != tx_queue.used->idx)
		{
			/*kprintf("TX queue last used idx: %u, tx_queue used: %p, desc: %p, avail: %p, free: %u, head: %u\n",
				tx_queue.last_used_idx, tx_queue.used, tx_queue.desc, tx_queue.avail, tx_queue.num_free_desc, tx_queue.free_desc_head);
			*/
			//IOLog("TX queue last used idx (%u) differs from current (%u), freeing packets.\n", tx_queue.last_used_idx, tx_queue.used->idx);
			
			releaseSentPackets();
		}
		
		if (has_reenabled_interrupts)
			break;
		
		// re-enable interrupts, then re-check for used buffers to avoid a race condition
		rx_queue.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
		// only enable transmission interrupts if we won't be notified for an empty queue anyway
		if (!feature_notify_on_empty)
			tx_queue.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
		OSSynchronizeIO();
		
		has_reenabled_interrupts = true;
	}
}

static void virtio_net_log_bad_provider(IOService* provider)
{
	if (!provider)
	{
		VIOLog("virtio-net start(): Error! Got NULL provider!?\n");
		return;
	}
	const OSMetaClass* meta = provider->getMetaClass();
	VIOLog("virtio-net start(): Provider (%p) has wrong type: %s (expected IOPCIDevice)\n", provider, meta->getClassName());
}

bool PJVirtioNet::mapVirtioConfigurationSpace()
{
	assert(pci_dev);
	PJLogVerbose("virtio-net mapConfigurationSpace(): attempting to map device memory with register 0\n");
	IOMemoryMap* iomap = pci_dev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!iomap)
	{
		VIOLog("virtio-net mapConfigurationSpace(): Error! Memory-Mapping configuration space failed.\n");
		return false;
	}
	PJLogVerbose("virtio-net mapConfigurationSpace(): Mapped %llu bytes of device memory at %llX. (physical address %llX)\n",
		static_cast<uint64_t>(iomap->getLength()), iomap->getAddress(), pci_dev->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0)->getPhysicalSegment(0, NULL, 0));
	this->pci_virtio_header_iomap = iomap;
	return true;
}

void PJVirtioNet::virtioResetDevice()
{
	assert(pci_dev);
	assert(this->pci_virtio_header_iomap);
	setVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_RESET);
}

uint32_t PJVirtioNet::virtioResetInitAndReadFeatureBits()
{
	assert(pci_dev);
	assert(this->pci_virtio_header_iomap);
	// Reset the device just in case it was previously opened and left in a weird state.
	virtioResetDevice();
	// Acknowledge the device, then tell it we're the driver
	updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_ACKNOWLEDGE);
	updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_DRIVER);
	
	dev_feature_bitmap = virtioHeaderReadLE32(VIRTIO_PCI_CONF_OFFSET_DEVICE_FEATURE_BITS_0_31);
	return dev_feature_bitmap;
}

uint16_t PJVirtioNet::virtioReadOptionalConfigFieldsGetDeviceSpecificOffset()
{
	assert(pci_dev);
	assert(this->dev_feature_bitmap > 0); // if the features have been read, at the very least the BAD_FEATURE bit will be set
	assert(this->pci_virtio_header_iomap);
	
	/* Read out the flexible config space */
	size_t config_offset = VIRTIO_PCI_CONF_OFFSET_END_HEADER;

	// TODO: find out how to detect if MSI-X is enabled (I don't think it ever is on Mac OS X)
	// if (msix_enabled) config_offset += 4;
	
	// wherever we are now is the offset for the device-specific configuration space
	return config_offset;
}

UInt32 PJVirtioNet::getFeatures() const
{
	if (driver_state == kDriverStateInitial)
		VIOLog("virtio-net getFeatures(): Warning! System asked about driver features before they could be detected.\n");
	return (feature_tso_v4 ? kIONetworkFeatureTSOIPv4 : 0);
}

bool PJVirtioNet::start(IOService* provider)
{
	PJLogVerbose("virtio-net start(%p)\n", provider);
	if (driver_state != kDriverStateInitial)
	{
		if (driver_state == kDriverStateStopped)
		{
			VIOLog("virtio-net start(): Warning! Driver was re-start()ed after being stop()ped. This normally doesn't happen and is untested.\n");
		}
		else
		{
			VIOLog("virtio-net start(): Error! Unexpected driver state (%d), aborting.\n", driver_state);
			return false;
		}
	}
	
	driver_state = kDriverStateStartFailed;
	if (!super::start(provider))
		return false;

	UInt32 mtu = 0;
	if (kIOReturnSuccess != getMaxPacketSize(&mtu))
	{	
		VIOLog("Failed to determine MTU!\n");
	}
	else
	{
		PJLogVerbose("Reported MTU: %lu bytes\n", static_cast<size_t>(mtu));
	}

	IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
	{
		driver_state = kDriverStateStartFailedUnsupportedDevice;
		virtio_net_log_bad_provider(provider);
		return false;
	}

	if (!pci->open(this))
		return false;
	this->pci_dev = pci;	

	work_loop = getWorkLoop();
	if (!work_loop)
		return false;
	work_loop->retain();
	
	this->should_disable_io = !pci->setIOEnable(true);
	
	if (!this->startWithIOEnabled())
	{
		if (this->should_disable_io)
			pci->setIOEnable(false);
		this->should_disable_io = false;
		return false;
	}
	
	return true;
}

bool PJVirtioNet::startWithIOEnabled()
{
	if (!mapVirtioConfigurationSpace())
		return false;
	
	PJLogVerbose("virtio-net start(): Device Initialisation Sequence\n");
	
	// partially start up the device
	uint32_t dev_features = virtioResetInitAndReadFeatureBits();
#ifdef PJ_VIRTIO_NET_VERBOSE
	virtio_log_supported_features(dev_features);
#endif
	
	// We can use the notify-on-empty feature to permanently disable transmission interrupts
	feature_notify_on_empty = (0 != (dev_features & VIRTIO_F_NOTIFY_ON_EMPTY));
	
	/* If supported, enable checksum offloading and IPv4 TCP segmentation, as this
	 * is necessary to enable TSO - we won't actually use the checksum offload
	 * mechanism itself as OSX won't provide us with a partial checksum for the
	 * pseudo header.
	 */
	feature_checksum_offload = false;
	feature_tso_v4 = false;
	if (pref_allow_offloading)
	{
		feature_checksum_offload = (0 != (dev_features & VIRTIO_NET_F_CSUM));
		if (feature_checksum_offload)
		{
			feature_tso_v4 = (0 != (dev_features & VIRTIO_NET_F_HOST_TSO4));
		}
	}

	size_t device_specific_offset = virtioReadOptionalConfigFieldsGetDeviceSpecificOffset();
	
	determineMACAddress(device_specific_offset);
	detectLinkStatusFeature(device_specific_offset);
	
	// we're not actually interested in the device for now until it's enable()d
	virtioResetDevice();
	OSSafeReleaseNULL(pci_virtio_header_iomap);
	pci_dev->close(this);
	
	if (!getOutputQueue())
	{
		VIOLog("virtio-net start(): failed to get output queue\n");
		return false;
	}
	
	// create the interface nub
	interface = NULL;
	if (!attachInterface((IONetworkInterface **)&interface, false))
	{
		VIOLog("virtio-net start(): attachInterface() failed, interface = %p [%s].\n",
			interface, interface ? interface->getMetaClass()->getClassName() : "null");
		return false;
	}
	driver_state = kDriverStateStarted;

	interface->registerService();
	PJLogVerbose("virtio-net start(): interface registered.\n");
	
	// now try to set up the debugger
	// allocate a reserved packet for transmission so we don't have to allocate in sendPacket()
	mbuf_t packet_mbuf = allocatePacket(kIOEthernetMaxPacketSize);
	virtio_net_packet* packet_mem = packet_mbuf ? allocPacket() : NULL;
	if (packet_mem)
	{
		debugger_transmit_packet = packet_mem;
		debugger_transmit_packet->mbuf = packet_mbuf;
	}
	else if (packet_mbuf)
	{
		freePacket(packet_mbuf);
		packet_mbuf = NULL;
	}
	// if that worked, try to attach the debugger
	if (!debugger_transmit_packet || !attachDebuggerClient(&debugger))
	{
		VIOLog("virtio-net start(): Warning! Failed to instantiate %s. Continuing anyway, but debugger will be unavailable.\n",
			debugger_transmit_packet ? "debugger client" : "transmission packet reserved for debugger");
	}
	else
	{
		PJLogVerbose("virtio-net start(): Debug client attached successfully.\n");
	}
	return true;
}

void PJVirtioNet::determineMACAddress(uint16_t device_specific_offset)
{
	// sort out mac address
	if (dev_feature_bitmap & VIRTIO_NET_F_MAC)
	{
		for (unsigned i = 0; i < sizeof(mac_address); ++i)
		{
			mac_address.bytes[i] = virtioHeaderRead8(device_specific_offset + offsetof(virtio_net_config, mac[i]));
		}
		PJLogVerbose("virtio-net start(): Determined MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
			mac_address.bytes[0], mac_address.bytes[1], mac_address.bytes[2],
			mac_address.bytes[3], mac_address.bytes[4], mac_address.bytes[5]);
	}
	else
	{
		// generate random MAC address
		uint32_t rnd1 = random();
		uint32_t rnd2 = random();
		mac_address.bytes[0] = (rnd1 & 0xfe) | 0x02; // ensure multicast bit is off and local assignment bit is on
		mac_address.bytes[1] = (rnd1 >> 8) & 0xff;
		mac_address.bytes[2] = ((rnd1 >> 16) ^ rnd2) & 0xff;
		mac_address.bytes[3] = ((rnd1 >> 24) ^ (rnd2 >> 8)) & 0xff;
		mac_address.bytes[4] = (rnd2 >> 16) & 0xff;
		mac_address.bytes[5] = (rnd2 >> 24) & 0xff;
		
		VIOLog("virtio-net start(): Device does not specify its MAC address, randomly generated %02X:%02X:%02X:%02X:%02X:%02X\n",
			mac_address.bytes[0], mac_address.bytes[1], mac_address.bytes[2],
			mac_address.bytes[3], mac_address.bytes[4], mac_address.bytes[5]);
	}
	mac_address_is_valid = true;
}

// check link status, if possible, and record its configuration space offset for later updates
void PJVirtioNet::detectLinkStatusFeature(uint16_t device_specific_offset)
{
	status_field_offset = 0;
	bool link_is_up = true;
	if (dev_feature_bitmap & VIRTIO_NET_F_STATUS)
	{
		status_field_offset = device_specific_offset + offsetof(virtio_net_config, status);
		uint16_t status = readStatus();
		PJLogVerbose("virtio-net start(): Link status field 0x%04X (link %s)\n",
			status, (status & VIRTIO_NET_S_LINK_UP) ? "up" : "down");
		link_is_up = (status & VIRTIO_NET_S_LINK_UP) != 0;
	}
	setLinkStatus((link_is_up ? kIONetworkLinkActive : 0) | kIONetworkLinkValid);
}

bool PJVirtioNet::beginHandlingInterrupts()
{
	PJLogVerbose("virtio-net beginHandlingInterrupts()\n");
	if (!pci_dev)
	{
		VIOLog("virtio-net beginHandlingInterrupts(): Error! PCI device must be known for generating interrupts.\n");
		return false;
	}
	
	// Message signaled interrupts (MSI) are more efficient than the normal broadcast ones, so let's try to use them
	int msi_index = -1;
	int intr_index = 0;
	
	// keep trying interrupt source indices until we run out or find an MSI one
	while (intr_index >= 0)
	{
		int intr_type = 0;
		IOReturn ret = pci_dev->getInterruptType(intr_index, &intr_type);
		if (ret != kIOReturnSuccess)
			break;
			
		if (intr_type & kIOInterruptTypePCIMessaged)
		{
			// found MSI interrupt source
			msi_index = intr_index;
			break;
		}
		++intr_index;
	}
	
	if (msi_index >= 0)
	{
		intr_index = msi_index;
		VIOLog("virtio-net beginHandlingInterrupts(): Enabled message signaled interrupts (index %d).\n", intr_index);
	}
	else
	{
		intr_index = 0;
	}

	
	intr_event_source = IOFilterInterruptEventSource::filterInterruptEventSource(this, &interruptAction, &interruptFilter, pci_dev, intr_index);
	if (!intr_event_source)
	{
		VIOLog("virtio-net beginHandlingInterrupts(): Error! %s interrupt event source failed.\n", intr_event_source ? "Initialising" : "Allocating");
		OSSafeReleaseNULL(intr_event_source);
		return false;
	}
	if (kIOReturnSuccess != work_loop->addEventSource(intr_event_source))
	{
		VIOLog("virtio-net beginHandlingInterrupts(): Error! Adding interrupt event source to work loop failed.\n");
		OSSafeReleaseNULL(intr_event_source);
		return false;
	}
	intr_event_source->enable();
	PJLogVerbose("virtio-net beginHandlingInterrupts(): now handling interrupts, good to go.\n");	
	return true;
}

void PJVirtioNet::endHandlingInterrupts()
{
	if (!intr_event_source)
	{
		VIOLog("virtio-net endHandlingInterrupts(): Warning! Interrupt event source does not exist.\n");
		return;
	}
	
	this->intr_event_source->disable();
	work_loop->removeEventSource(intr_event_source);
	OSSafeReleaseNULL(intr_event_source);
}



bool PJVirtioNet::configureInterface(IONetworkInterface *netif)
{
	PJLogVerbose("virtio-net configureInterface([%s] @ %p)\n", netif ? netif->getMetaClass()->getClassName() : "null", netif);
	if (!super::configureInterface(netif))
	{
		VIOLog("virtio-net configureInterface(): super failed\n");
		return false;
	}
	return true;
}

IOReturn PJVirtioNet::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
	PJLogVerbose("virtio-net getPacketFilters()\n");
	return super::getPacketFilters(group, filters);
}



int32_t PJVirtioNet::readStatus()
{
	if (!status_field_offset) return -1;
	return virtioHeaderRead16(status_field_offset);
}

bool PJVirtioNet::setupVirtqueue(
	uint16_t queue_id, virtio_net_virtqueue& queue)
{
	/* virtqueues must be aligned to 4096 bytes (last 12 bits 0) and the shifted
	 * address will be written to a 32-bit register 
	 */
	const mach_vm_address_t VIRTIO_RING_ALLOC_MASK = 0xffffffffull << 12u;
	// detect and allocate virtqueues - expect 2 plus 1 if VIRTIO_NET_F_CTRL_VQ is set
	// queue 0 is receive queue (Appendix C, Configuration)
	// queue 1 is transmit queue
	// queue 2 is control queue (if present)
	virtioHeaderWriteLE16(VIRTIO_PCI_CONF_OFFSET_QUEUE_SELECT, queue_id);
	uint16_t queue_size = virtioHeaderReadLE16(VIRTIO_PCI_CONF_OFFSET_QUEUE_SIZE);
	if (queue_size == 0)
	{
		VIOLog("virtio-net setupVirtqueue(): Queue size for queue %u is 0.\n", queue_id);
		return false;
	}
	else if (!is_pow2(queue_size))
	{
		VIOLog("virtio-net setupVirtqueue(): Queue size for queue %u is %u, which is not a power of 2. Aborting.\n", queue_id, queue_size);
		return false;
	}
	PJLogVerbose("virtio-net setupVirtqueue(): Reported queue size for queue %u: %u\n", queue_id, queue_size);
	// allocate an appropriately sized DMA buffer. As per the spec, this must be physically contiguous.
	size_t queue_size_bytes = vring_size(queue_size);
	IOBufferMemoryDescriptor* queue_buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOInhibitCache, queue_size_bytes, VIRTIO_RING_ALLOC_MASK);
	if (!queue_buffer)
	{
		VIOLog("virtio-net setupVirtqueue(): Failed to allocate queue buffer with %" PRIuPTR " contiguous bytes and mask %" PRIX64 ".\n",
			queue_size_bytes, VIRTIO_RING_ALLOC_MASK);
		return false;
	}
	memset(queue_buffer->getBytesNoCopy(), 0, queue_size_bytes);
				
	virtqueue_init(queue, queue_buffer, queue_size, queue_id);

	virtioHeaderWriteLE32(VIRTIO_PCI_CONF_OFFSET_QUEUE_ADDRESS, static_cast<uint32_t>(queue_buffer->getPhysicalSegment(0, NULL, 0) >> 12u));
	
	return true;
}
void virtqueue_init(virtio_net_virtqueue& queue, IOBufferMemoryDescriptor* buf, uint16_t queue_size, uint16_t queue_id)
{
	queue.index = queue_id;
	queue.buf = buf;
	vring_init(&queue, queue_size, buf->getBytesNoCopy(), VIRTIO_PAGE_SIZE);

	// disable interrupts to begin with
	queue.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;

	queue.packets_for_descs = PJZMallocArray<virtio_net_packet*>(queue_size);
	queue.num_free_desc = queue_size;
	queue.free_desc_head = 0;
	queue.last_used_idx = 0;
	// 
	for (uint16_t i = 1; i < queue_size; ++i)
	{
		queue.desc[i - 1].flags = VRING_DESC_F_NEXT;
		queue.desc[i - 1].next = i;
	}
}

void virtqueue_free(virtio_net_virtqueue& queue)
{
	OSSafeReleaseNULL(queue.buf);
	if (queue.packets_for_descs)
	{
		PJFreeArray(queue.packets_for_descs, queue.num);
		queue.packets_for_descs = NULL;
	}
	memset(&queue, 0, sizeof(queue));
}


IOOutputQueue* PJVirtioNet::createOutputQueue()
{
	/* For now, go with a gated output queue, as this is the simplest option. Later
	 * on, we can provide more granular access to the virtqueue mechanism. */
	IOGatedOutputQueue* queue = IOGatedOutputQueue::withTarget(this, this->getWorkLoop(), 0 /* capacity = 0: Initially, we can't yet send packets */);
	PJLogVerbose("virtio-net createOutputQueue(): %p\n", queue);
	return queue;
}

IOReturn PJVirtioNet::enable(IOKernelDebugger *debugger)
{
	return runInCommandGate<IOKernelDebugger, &PJVirtioNet::gatedEnableDebugger>(debugger);
}

IOReturn PJVirtioNet::gatedEnableDebugger(IOKernelDebugger* debugger)
{
	if (!this->debugger || !this->debugger_transmit_packet)
		return kIOReturnError;
	if (driver_state == kDriverStateEnabled)
	{
		// already fully up and running anyway
		driver_state = kDriverStateEnabledBoth;
		VIOLog("virtio-net enable(): already enabled for normal interface clients, now also enabled for debugger client.\n");
		return kIOReturnSuccess;
	}
	else if (driver_state == kDriverStateEnabledBoth || driver_state == kDriverStateEnabledDebugging)
	{
		VIOLog("virtio-net enable(): already enabled for debugging, enable() called a second time.\n");
		return kIOReturnSuccess;
	}
	
	if (driver_state != kDriverStateStarted)
	{
		VIOLog("virtio-net enable(): Invalid state (%d) for enabling debugger.\n", driver_state);
		return kIOReturnInvalid;
	}
	
	bool ok = enablePartial();
#ifndef PJ_VIRTIO_NET_VERBOSE
	if (!ok)
#endif
		VIOLog("virtio-net enable(): Starting debugger %s.\n", ok ? "succeeded" : "failed");
	driver_state = ok ? kDriverStateEnabledDebugging : kDriverStateEnableFailed;
	return ok ? kIOReturnSuccess : kIOReturnError;
}

bool PJVirtioNet::enablePartial()
{
	if (!pci_dev->open(this))
	{
		VIOLog("virtio-net enable(): Opening PCI device failed.\n");
		return false;
	}
	if (!mapVirtioConfigurationSpace())
		return false;

	// Re-initialise the device
	uint32_t dev_features = virtioResetInitAndReadFeatureBits();

	// Initialise the receive and transmit virtqueues
	if (!setupVirtqueue(0, rx_queue))
		return failDevice(), false;
	PJLogVerbose("virtio-net enable(): Initialised virtqueue 0 (receive queue) with %llu bytes (%u entries) at %llX\n",
		static_cast<uint64_t>(rx_queue.buf->getLength()), rx_queue.num, rx_queue.buf->getPhysicalSegment(0, NULL, 0));

	if (!setupVirtqueue(1, tx_queue))
		return failDevice(), kIOReturnError;
	PJLogVerbose("virtio-net enable(): Initialised virtqueue 1 (transmit queue) with %llu bytes (%u entries) at %llX\n",
		static_cast<uint64_t>(tx_queue.buf->getLength()), tx_queue.num, tx_queue.buf->getPhysicalSegment(0, NULL, 0));
	// Don't support VIRTIO_NET_F_CTRL_VQ for now
	
			
	// write back supported features
	uint32_t supported_features = dev_features &
		(VIRTIO_F_NOTIFY_ON_EMPTY | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS | (feature_checksum_offload ? (VIRTIO_NET_F_CSUM | VIRTIO_NET_F_HOST_TSO4) : 0));
	virtioHeaderWriteLE32(VIRTIO_PCI_CONF_OFFSET_GUEST_FEATURE_BITS_0_31, supported_features);
	PJLogVerbose("virtio-net enable(): Wrote driver-supported feature bits: 0x%08X\n", supported_features);
	
	// tell device we're ready
	updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_DRIVER_OK);
	PJLogVerbose("virtio-net enable(): Device set to 'driver ok' state.\n");

	// The virtqueues can now be used
	
	// fill receive queue with as many empty packets as possible
	if (!populateReceiveBuffers())
	{
		// even if we couldn't fill all of it because we ran out of memory, treat partial success as OK.
		if (rx_queue.num_free_desc >= rx_queue.num)
		{
			// really memory-starved, sorry!
			driver_state = kDriverStateEnableFailedOutOfMemory;
			VIOLog("virtio-net enable(): Failed to populate receive buffers: out of memory.\n");
			failDevice(); // stop producing interrupts
			return kIOReturnNoMemory;
		}
	}
	PJLogVerbose("virtio-net enable(): Populated receive buffers: %u free descriptors left, avail idx %u\n",
		rx_queue.num_free_desc, rx_queue.avail->idx);

	return true;
}

IOReturn PJVirtioNet::enable(IONetworkInterface* interface)
{
	return runInCommandGate<IONetworkInterface, &PJVirtioNet::gatedEnableInterface>(interface);
}

IOReturn PJVirtioNet::selectMedium(const IONetworkMedium* medium)
{
	setSelectedMedium(medium);
	return kIOReturnSuccess;
}

static bool virtio_net_add_medium(OSDictionary* medium_dict, IOMediumType type, uint64_t speed)
{
	IONetworkMedium* medium = IONetworkMedium::medium(type, 0);
	if (!medium)
		return false;
	bool ok = IONetworkMedium::addMedium(medium_dict, medium);
	medium->release();
	return ok;
}

bool PJVirtioNet::createMediumTable()
{
	OSDictionary* dict = OSDictionary::withCapacity(2);
	if (!dict)
	{
		VIOLog("virtio-net createMediumTable: Failed to allocate dictionary.\n");
		return false;
	}
	
	bool added = true;
	added = added && virtio_net_add_medium(dict, kIOMediumEthernetNone, 0);
	added = added && virtio_net_add_medium(dict, kIOMediumEthernetAuto, 0);
	if (!added)
	{
		VIOLog("virtio-net createMediumTable: Failed to allocate and add media to table.\n");
		dict->release();
		return false;
	}
	
	if (!publishMediumDictionary(dict))
	{
		VIOLog("virtio-net createMediumTable: Failed to publish medium dictionary.\n");
		dict->release();
		return false;
	}
	dict->release();

	const OSDictionary* media = getMediumDictionary();
	IONetworkMedium* medium = media ? IONetworkMedium::getMediumWithType(media, kIOMediumEthernetAuto) : NULL;
	if (medium)
		setCurrentMedium(medium);
	else
		VIOLog("virtio-net createMediumTable: Warning! Failed to locate current medium in table.");
	
	return true;
}


IOReturn PJVirtioNet::gatedEnableInterface(IONetworkInterface* interface)
{
	PJLogVerbose("virtio-net enable()\n");
	if (driver_state == kDriverStateEnabledBoth || driver_state == kDriverStateEnabled)
		return kIOReturnSuccess;
	if (driver_state != kDriverStateStarted && driver_state != kDriverStateEnabledDebugging)
	{
		VIOLog("virtio-net enable(): Bad driver state %d (expected %d or %d), aborting.\n", driver_state, kDriverStateStarted, kDriverStateEnabledDebugging);
		return kIOReturnInvalid;
	}
	bool has_debugger = (driver_state == kDriverStateEnabledDebugging);
	if (driver_state != kDriverStateEnabledDebugging)
		driver_state = kDriverStateEnableFailed;
	if (interface != this->interface)
	{
		VIOLog("virtio-net enable(): unknown interface %p (expected %p)\n", interface, this->interface);
		return kIOReturnBadArgument;
	}
	
	if (driver_state != kDriverStateEnabledDebugging && !enablePartial())
	{
		driver_state = kDriverStateEnableFailed;
		VIOLog("virtio-net enable(): Basic device initialisation failed.\n");
		return kIOReturnError;
	}
	driver_state = kDriverStateEnableFailed;
	
	if (!createMediumTable())
	{
		VIOLog("virtio-net enable(): Failed to set up interface media table\n");
		return kIOReturnNoMemory;
	}

	// start handling interrupts now that the internal data structures are set up
	if (!beginHandlingInterrupts())
		return failDevice(), false;

	// enable interrupts on the appropriate queues
	rx_queue.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
	if (!feature_notify_on_empty)
		tx_queue.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
	OSSynchronizeIO();
	
	// enable the output queue
	IOOutputQueue* output_queue = getOutputQueue();
	if (!output_queue)
		return failDevice(), kIOReturnError;
	uint32_t capacity = max(16, tx_queue.num / 4); // each packet takes 2 buffers, try to always fill up half the virtqueue, hence a quarter
	output_queue->setCapacity(capacity);
	output_queue->start();
	
	updateLinkStatus();
	
	driver_state = has_debugger ? kDriverStateEnabledBoth : kDriverStateEnabled;

	return kIOReturnSuccess;
}

void PJVirtioNet::clearVirtqueuePackets(virtio_net_virtqueue& queue)
{
	if (!queue.packets_for_descs)
		return;
	for (uint16_t i = 0; i < queue.num; ++i)
	{
		virtio_net_packet* packet = queue.packets_for_descs[i];
		if (!packet || packet == debugger_transmit_packet)
			continue;

		if (packet->dma_cmd)
			packet->dma_cmd->clearMemoryDescriptor();
		OSSafeReleaseNULL(packet->dma_cmd);
		OSSafeReleaseNULL(packet->dma_md);
		OSSafeReleaseNULL(packet->mbuf_md);
		if (packet->mbuf)
		{
			freePacket(packet->mbuf);
			packet->mbuf = NULL;
		}
		PJLogVerbose("clearVirtqueuePackets (queue %p): Freeing packet buffer %p (%llu bytes) - descriptor %p\n", &queue, packet, packet->mem ? packet->mem->getLength() : 0, packet->mem);
		IOBufferMemoryDescriptor* md = packet->mem;
		
		memset(packet, 0, sizeof(*packet));
		if (md)
			md->release();
		
		queue.packets_for_descs[i] = NULL;
	}
}

IOReturn PJVirtioNet::disable(IOKernelDebugger *debugger)
{
	PJLogVerbose("virtio-net disable(): Disabling debugger.\n");
	if (driver_state != kDriverStateEnabledDebugging && driver_state != kDriverStateEnabledBoth)
	{
		VIOLog("virtio-net disable(): Bad driver state %d, aborting.\n", driver_state);
		return kIOReturnInvalid;
	}
	
	if (driver_state == kDriverStateEnabledDebugging)
	{
		disablePartial();
		driver_state = kDriverStateStarted;
		PJLogVerbose("virtio-net disable(): Disabled device altogether.\n");
	}
	else
	{
		driver_state = kDriverStateEnabled;
		PJLogVerbose("virtio-net disable(): Disabled debugger, interface client still active.\n");
	}
	return kIOReturnSuccess;
}

IOReturn PJVirtioNet::disable(IONetworkInterface* interface)
{
	PJLogVerbose("virtio-net disable()\n");
	if (driver_state != kDriverStateEnabled && driver_state != kDriverStateEnabledBoth)
	{
		VIOLog("virtio-net disable(): Bad driver state %d (expected %d), aborting.\n", driver_state, kDriverStateEnabled);
		return kIOReturnInvalid;
	}
	
	// disable the output queue
	IOOutputQueue* output_queue = getOutputQueue();
	if (output_queue)
	{
		output_queue->stop();
		output_queue->setCapacity(0);
		output_queue->flush();
	}
	
	// disable interrupts again
	rx_queue.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;
	if (!feature_notify_on_empty)
		tx_queue.avail->flags |= VRING_AVAIL_F_NO_INTERRUPT;

	// interrupts not needed for debugger-only operation
	endHandlingInterrupts();

	if (driver_state == kDriverStateEnabledBoth)
	{
		driver_state = kDriverStateEnabledDebugging;
		PJLogVerbose("virtio-net disable(): Transitioned to debugger-only state.\n");
	}
	else
	{
		disablePartial();
		driver_state = kDriverStateStarted;
	}
	
	return kIOReturnSuccess;
}

void PJVirtioNet::disablePartial()
{
	PJLogVerbose("virtio-net disablePartial()\n");
	handleReceivedPackets();
	releaseSentPackets();

	// disable the device to stop any more interrupts from occurring
	virtioResetDevice();

	// free any remaining packet resources
	clearVirtqueuePackets(rx_queue);
	clearVirtqueuePackets(tx_queue);

	// unmap and close device
	OSSafeReleaseNULL(this->pci_virtio_header_iomap);
	pci_dev->close(this);
	
	// Free any pooled packet headers
	flushPacketPool();

	// Deallocate virtqueue resources and reset descriptors
	virtqueue_free(rx_queue);
	virtqueue_free(tx_queue);
	
	driver_state = kDriverStateStarted;
	PJLogVerbose("virtio-net disablePartial() done\n");
}


UInt32 PJVirtioNet::outputPacket(mbuf_t buffer, void *param)
{
	// try to clear any completed packets from the queue
	runInCommandGate<bool, &PJVirtioNet::releaseSentPackets>(false);

	if (tx_queue.num_free_desc < 3)
	{
		//IOLog("virtio-net outputPacket(): Transmit queue really full, pipeline stalled.\n");
		// activate interrupt for when a packet is sent if we're currently only being notified when empty
		if (feature_notify_on_empty)
		{
			tx_queue.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
			OSSynchronizeIO();
		}
		was_stalled = true;
		return kIOReturnOutputStall;
	}
			
	uint16_t avail_idx = tx_queue.avail->idx;
	IOReturn add_ret = addPacketToQueue(buffer, tx_queue, false /* packet to be read by device */, avail_idx);
	if (add_ret != kIOReturnSuccess)
	{
		if (add_ret == kIOReturnOutputStall)
		{
			if (feature_notify_on_empty)
			{
				tx_queue.avail->flags &= ~VRING_AVAIL_F_NO_INTERRUPT;
				OSSynchronizeIO();
			}
			was_stalled = true;
			return kIOReturnOutputStall;
		}
		kprintf("virtio-net outputPacket(): failed to add packet (length: %lu, return value %X) to queue, dropping it.\n", mbuf_len(buffer), add_ret);
		freePacket(buffer);
		return kIOReturnOutputDropped;
	}
	notifyQueueAvailIdx(tx_queue, avail_idx);
	return kIOReturnOutputSuccess;
}
	
void PJVirtioNet::receivePacket(void *pkt, UInt32 *pktSize, UInt32 timeout)
{
	// note: timeout seems to be 3ms in OSX 10.6.8
	uint64_t timeout_us = timeout * 1000ull;
	uint64_t waited = 0;
	
	//kprintf("virtio-net receivePacket(): Willing to wait %lu ms\n", timeout);

	while (true)
	{
		// check for packets in the rx queue
		uint16_t last_used = rx_queue.last_used_idx;
		for (uint16_t used_idx = last_used; used_idx != rx_queue.used->idx; ++used_idx)
		{
			vring_used_elem& used = rx_queue.used->ring[used_idx % rx_queue.num];
			uint16_t desc = used.id;
			if (desc == UINT16_MAX || desc >= rx_queue.num || used.len < sizeof(virtio_net_hdr))
				continue;
			
			virtio_net_packet* packet = rx_queue.packets_for_descs[desc];
			if (!packet)
			{
				kprintf("virtio-net receivePacket(): Warning! No packet for descriptor %u, skipping.\n", desc);
				continue;
			}
			
			rx_queue.last_used_idx = used_idx + 1;
			
			uint32_t len = *pktSize = static_cast<uint32_t>(used.len - sizeof(virtio_net_hdr));
			//kprintf("virtio-net receivePacket(): Copying received debugger packet (length %u).\n", len);
			memcpy(pkt, mbuf_data(packet->mbuf), len);
			used.id = UINT16_MAX;
			used.len = 0;

			// immediately re-queue into available ring
			uint16_t avail_idx = rx_queue.avail->idx;
			rx_queue.avail->ring[avail_idx % rx_queue.num] = desc;
			++avail_idx;
			notifyQueueAvailIdx(rx_queue, avail_idx);
			return;
		}
		
		if (waited >= timeout_us)
		{
			//kprintf("virtio-net receivePacket(): out of time\n");
			break;
		}
		
		IODelay(20);
		waited += 20;
	}
}

static void vring_push_free_desc(virtio_net_virtqueue& queue, uint16_t free_idx);
static int32_t vring_pop_free_desc(virtio_net_virtqueue& queue);

IOReturn PJVirtioNet::getChecksumSupport(UInt32 *checksumMask, UInt32 checksumFamily, bool isOutput)
{
	*checksumMask = 0;
	if (checksumFamily != kChecksumFamilyTCPIP)
		return kIOReturnUnsupported;
	if (feature_checksum_offload)
		*checksumMask = kChecksumTCP;
	return kIOReturnSuccess;
}

void PJVirtioNet::sendPacket(void *pkt, UInt32 pktSize)
{
	//kprintf("virtio-net sendPacket(): %lu bytes\n", pktSize);
	if (pktSize > kIOEthernetMaxPacketSize)
	{
		kprintf("virtio-net sendPacket(): Packet too big, aborting.\n");
		return;
	}
	if (!debugger_transmit_packet)
	{
		kprintf("virtio-net sendPacket(): Driver not ready, aborting.\n");
		return;
	}
	int32_t head_desc = vring_pop_free_desc(tx_queue);
	int32_t main_desc = vring_pop_free_desc(tx_queue);
	if (main_desc < 0)
	{
		if (head_desc >= 0)
			vring_push_free_desc(tx_queue, head_desc);
		kprintf("virtio-net sendPacket(): No free virtqueue descriptors.\n");
		return;
	}
	
	virtio_net_packet* packet = debugger_transmit_packet;
	memcpy(mbuf_data(packet->mbuf), pkt, pktSize);

	packet->header.flags = 0;
	packet->header.gso_type = VIRTIO_NET_HDR_GSO_NONE;
	packet->header.hdr_len = 0;
	packet->header.gso_size = 0;
	packet->header.csum_start = 0;
	packet->header.csum_offset = 0;
	
	// fill out descriptors
	tx_queue.desc[head_desc].addr = packet->mem->getPhysicalSegment(0, NULL, 0);
	tx_queue.desc[head_desc].len = sizeof(packet->header);
	tx_queue.desc[head_desc].flags = VRING_DESC_F_NEXT;
	tx_queue.desc[head_desc].next = main_desc;
	tx_queue.desc[main_desc].addr = mbuf_data_to_physical(mbuf_data(packet->mbuf));
	tx_queue.desc[main_desc].len = pktSize;
	tx_queue.desc[main_desc].flags = 0;
	tx_queue.desc[main_desc].next = UINT16_MAX;
	
	uint16_t last_used_idx = tx_queue.used->idx;
	tx_queue.packets_for_descs[head_desc] = debugger_transmit_packet;
	uint16_t avail_idx = tx_queue.avail->idx;
	tx_queue.avail->ring[avail_idx % tx_queue.num] = head_desc;
	/*kprintf("virtio-net sendPacket(): Adding packet to tx queue in descriptors %u and %u, available index %u.\n",
		head_desc, main_desc, avail_idx);*/
	tx_queue.avail->idx = ++avail_idx;
	notifyQueueAvailIdx(tx_queue, avail_idx);
	
	unsigned wait_us = 1;
	while (true)
	{
		// this means no other thread/interrupt has detected the packet as sent, so let's keep polling it ourselves
		if (last_used_idx != tx_queue.used->idx)
		{
			vring_used_elem& used = tx_queue.used->ring[last_used_idx % tx_queue.num];
			if (used.id == head_desc)
			{
				// this is the packet we just sent!
				used.id = UINT16_MAX;
				used.len = 0;
				//kprintf("virtio-net sendPacket(): Debugger packet was found in used tx ring at index %u, finishing up.\n", last_used_idx);
				freeDescriptorChain(tx_queue, head_desc);
				break;
			}
		}
		IODelay(wait_us);
		if (wait_us < 10000)
			++wait_us;
		OSSynchronizeIO();
	}
	
	//kprintf("virtio-net sendPacket(): done, disposing of any other packets in the queue.\n");
	releaseSentPackets(true);
}

static int32_t vring_pop_free_desc(virtio_net_virtqueue& queue)
{
	if (queue.num_free_desc < 1)
		return -1;
	uint16_t free_idx = queue.free_desc_head;
	queue.free_desc_head = queue.desc[free_idx].next;
	--queue.num_free_desc;
	return free_idx;
}

static void vring_push_free_desc(virtio_net_virtqueue& queue, uint16_t free_idx)
{
	assert(free_idx < queue.num);
	queue.desc[free_idx].next = queue.free_desc_head;
	queue.desc[free_idx].flags = queue.num_free_desc > 0 ? VRING_DESC_F_NEXT : 0;
	queue.packets_for_descs[free_idx] = NULL;
	queue.free_desc_head = free_idx;
	++queue.num_free_desc;
}

bool PJVirtioNet::notifyQueueAvailIdx(virtio_net_virtqueue& queue, uint16_t new_avail_idx)
{
	OSSynchronizeIO();
	queue.avail->idx = new_avail_idx;
	OSSynchronizeIO();
	if (0 == (queue.used->flags & VRING_USED_F_NO_NOTIFY))
	{
		virtioHeaderWrite16(VIRTIO_PCI_CONF_OFFSET_QUEUE_NOTIFY, queue.index);
		return true;
	}
	return false;
}

/// Structure for tracking the progress of turning a packet into virtqueue buffers across outputPacketSegment() calls
struct virtio_net_cursor_segment_context
{
	PJVirtioNet* me;
	
	virtio_net_virtqueue& queue;
	/// Array of descriptor indices in the chain
	int32_t* descs;
	/// Number of elements in the descs array
	unsigned max_num_descs;
	/// Index of the highest filled segment
	int32_t max_segment_created;
	/// Bits to OR into the buffer's "flags" field to indicate reading or writing
	uint8_t buffer_direction_flag;
	
	uint32_t total_len;
	
	/// A problem has occurred, further outputPacketSegment() calls will not do anything except increment the segment counter
	bool error;
	/// The error was that descriptor allocation failed
	bool out_of_descriptors;
	
	unsigned descs_allocd;
};
bool PJVirtioNet::outputPacketSegment(IODMACommand* target, IODMACommand::Segment64 segment, void* segment_context, UInt32 segmentIndex)
{
	//IOLog("virtio-net outputPacketSegment(): segment %lu at 0x%08lX, length %lu.\n", segmentIndex, segment.location, segment.length);
	virtio_net_cursor_segment_context* ctx = static_cast<virtio_net_cursor_segment_context*>(segment_context);
	if (ctx->error)
		return false;
	
	if (segmentIndex >= ctx->max_num_descs)
	{
		ctx->error = true;
		return false;
	}
	
	if (segment.fLength < 1)
		VIOLog("virtio-net outputPacketSegment(): Zero length segment!\n");
	
	int32_t desc = ctx->descs[segmentIndex];
	if (desc < 0)
	{
		desc = vring_pop_free_desc(ctx->queue);
		++ctx->descs_allocd;
	}
	else
	{
		ctx->total_len -= ctx->queue.desc[desc].len;
	}
	/*else
		IOLog("virtio-net outputPacketSegment(): Note: rewriting descriptor %u! Prev addr: %llu len: %u, now addr: %lu, len: %lu\n",
			desc, ctx->queue.desc[desc].addr, ctx->queue.desc[desc].len, segment.location, segment.length);
	*/
	if (desc < 0)
	{
		--ctx->descs_allocd;
		VIOLog("virtio-net outputPacketSegment(): failed to allocate descriptor.\n");
		ctx->error = ctx->out_of_descriptors = true;
		return false;
	}

	ctx->descs[segmentIndex] = desc;
	vring_desc& buf = ctx->queue.desc[desc];
	buf.addr = segment.fIOVMAddr;
	buf.len = static_cast<uint32_t>(segment.fLength);
	ctx->total_len += buf.len;
	buf.flags = ctx->buffer_direction_flag;
	buf.next = UINT16_MAX;
	if ((int32_t)segmentIndex > ctx->max_segment_created)
		ctx->max_segment_created = segmentIndex;
	//IOLog("virtio-net outputPacketSegment(): Added segment as buffer in descriptor %u, total packet length so far: %u.\n", desc, ctx->total_length);
	return true;
}

virtio_net_packet* PJVirtioNet::allocPacket()
{
	IOBufferMemoryDescriptor* packet_mem = OSDynamicCast(IOBufferMemoryDescriptor, packet_bufdesc_pool->getAnyObject());
	if (packet_mem)
	{
		packet_mem->retain();
		packet_bufdesc_pool->removeObject(packet_mem);
		return static_cast<virtio_net_packet*>(packet_mem->getBytesNoCopy());
	}
	else
	{
		packet_mem = IOBufferMemoryDescriptor::inTaskWithOptions(
			kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOInhibitCache, sizeof(virtio_net_packet),
			sizeof(void*) /* align to pointer */);
		if (!packet_mem)
			return NULL;
		virtio_net_packet* packet = static_cast<virtio_net_packet*>(packet_mem->getBytesNoCopy());
		packet->mem = packet_mem;
		packet->dma_cmd = IODMACommand::withSpecification(outputPacketSegment, 64, 0);
		if (!packet->dma_cmd)
		{
			packet_mem->release();
			return NULL;
		}
		packet->dma_md = SSDCMultiSubrangeMemoryDescriptor::withDescriptorRanges(NULL, 0, kIODirectionNone, false);
		if (!packet->dma_md)
		{
			packet->dma_cmd->release();
			packet_mem->release();
			return NULL;
		}
		
		packet->mbuf_md = PJMbufMemoryDescriptor::withMbuf(NULL, kIODirectionNone);
		if (!packet->mbuf_md)
		{
			packet->dma_md->release();
			packet->dma_cmd->release();
			packet_mem->release();
			return NULL;
		}
		packet->mbuf = NULL;
		return packet;
	}
}

static void virtio_net_enable_tcp_csum(virtio_net_packet* packet, bool need_partial, mbuf_t packet_mbuf, uint16_t ip_hdr_len, struct ip* ip_hdr)
{
	// calculate the pseudo-header checksum (this will be extended by the data checksum by the "hardware")
	char* ip_start = reinterpret_cast<char*>(ip_hdr);
	struct tcphdr* tcp_hdr = reinterpret_cast<struct tcphdr*>(ip_start + ip_hdr_len);
	if (need_partial)
	{
			unsigned ip_len = ntohs(ip_hdr->ip_len);
			unsigned tcp_len = ip_len - ip_hdr_len;
			uint32_t csum_l = 0;
			union
			{
				uint32_t l;
				uint16_t s[2];
			} tmp;
			
			tmp.l = ip_hdr->ip_src.s_addr;
			csum_l += tmp.s[0];
			csum_l += tmp.s[1];
			
			tmp.l = ip_hdr->ip_dst.s_addr;
			csum_l += tmp.s[0];
			csum_l += tmp.s[1];
			
			csum_l += htons(ip_hdr->ip_p);
			csum_l += htons(tcp_len & 0xffff);
			
			csum_l = (csum_l & 0xffff) + (csum_l >> 16);
			
			uint16_t csum = (csum_l & 0xffff) + (csum_l >> 16);
			
			tcp_hdr->th_sum = csum;
		}
		else
		{
			tcp_hdr->th_sum = 0;
		}

			
			if (ip_hdr->ip_v != 4)
			{
				VIOLog("Warning! IP header says version %u, expected 4 for IPv4!\n", ip_hdr->ip_v);
			}
			if (ip_hdr->ip_p != 6)
			{
				VIOLog("Warning! IP header refers to protocol %u, expected 6 for TCP!\n", ip_hdr->ip_p);
			}
			
			packet->header.flags = VIRTIO_NET_HDR_F_NEEDS_CSUM;
			packet->header.csum_start = ETHER_HDR_LEN + ip_hdr_len;
			packet->header.csum_offset = 16;
}

/* returns kIOReturnOutputStall if there aren't enough descriptors,
 * kIOReturnSuccess if everything went well, kIOReturnNoMemory if alloc failed,
 * and kIOReturnError if something else went wrong.
 */
IOReturn PJVirtioNet::addPacketToQueue(mbuf_t packet_mbuf, virtio_net_virtqueue& queue, bool for_writing, uint16_t& at_avail_idx)
{
	// check if there are going to be enough descriptors around
	if (queue.num_free_desc < 2)
		return kIOReturnOutputStall;

	// when transmitting, we may want to request specific "hardware" features
	bool requested_tcp_csum = false;
	bool requested_tsov4 = false;
	bool requested_udp_csum = false;
	mbuf_csum_request_flags_t tso_req = 0;
	uint32_t tso_val = 0;

	if (!for_writing && feature_checksum_offload)
	{
		// deal with any checksum offloading requests
		UInt32 demand_mask = 0;
		getChecksumDemand(packet_mbuf, kChecksumFamilyTCPIP, &demand_mask);
		if (demand_mask != 0 && demand_mask != kChecksumTCP)
		{
			static bool has_warned_bad_demand_mask = false;
			if (!has_warned_bad_demand_mask)
				VIOLog("virtio-net addPacketToQueue(): Warning! Checksum demand mask is %08X\n", (uint32_t)demand_mask);
			has_warned_bad_demand_mask = true;
		}
		if (demand_mask & kChecksumTCP)
		{
			requested_tcp_csum = true;
		}
		
		if (feature_tso_v4)
		{
			// may need to handle tso
			errno_t tso_err = mbuf_get_tso_requested(packet_mbuf, &tso_req, &tso_val);
			if (tso_err != 0)
			{
				static bool has_had_tso_err = false;
				if (!has_had_tso_err)
				{
					has_had_tso_err = true;
					VIOLog("virtio-net addPacketToQueue(): mbuf_get_tso_requested() returned %d\n", tso_err);
				}
			}
			else if (tso_req != 0)
			{
				static bool has_had_tso_req_err = false;
				if (0 != (tso_req & ~(MBUF_TSO_IPV4 | MBUF_TSO_IPV6)) && !has_had_tso_req_err)
				{
					VIOLog("virtio-net addPacketToQueue(): Warning! mbuf_get_tso_requested() unknown TSO bitfield %08X.\n", tso_req);
					has_had_tso_req_err = true;
				}
				tso_req &= (MBUF_TSO_IPV4 | MBUF_TSO_IPV6);

				if (tso_req == MBUF_TSO_IPV4)
				{
					requested_tsov4 = true;
				}
				else
				{
					static bool has_had_tso6 = false;
					//if (!has_had_tso6)
						VIOLog("virtio-net addPacketToQueue(): Warning! mbuf_get_tso_requested() requested unexpected TCPv6 TSO: %08X\n", tso_req);
					has_had_tso6 = true;
				}
			}
		}
	}

	// compact small packets more if descriptors are getting scarce
	uint32_t max_segs = queue.num_free_desc; // add 1 for the virtio net header
	
	if (max_segs < 2)
		return kIOReturnOutputStall;

	// recycle or allocate memory for the packet virtio header buffer
	virtio_net_packet* packet = allocPacket();
	if (!packet)
	{
		VIOLog("virtio-net addPacketToQueue(): Failed to alloc packet\n");
		return kIOReturnOutputDropped;
	}

	packet->mbuf = packet_mbuf;
	IODirection buf_direction = for_writing ? kIODirectionOut : kIODirectionOut;
	if (!packet->mbuf_md->initWithMbuf(packet_mbuf, buf_direction))
	{
		VIOLog("virtio-net addPacketToQueue(): Failed to init mbuf memory descriptor\n");
		packet_bufdesc_pool->setObject(packet->mem);
		packet->mem->release();
		return kIOReturnOutputDropped;
	}
	
	packet->mem->setDirection(buf_direction);
	
	packet->dma_md_subranges[0].length = sizeof(packet->header);
	packet->dma_md_subranges[0].md = packet->mem;
	packet->dma_md_subranges[0].offset = offsetof(virtio_net_packet, header);
	packet->dma_md_subranges[1].length = packet->mbuf_md->getLength();
	packet->dma_md_subranges[1].md = packet->mbuf_md;
	packet->dma_md_subranges[1].offset = 0;
	if (!packet->dma_md->initWithDescriptorRanges(packet->dma_md_subranges, 2, buf_direction, false))
	{
		VIOLog("virtio-net addPacketToQueue(): Failed to init virtqueue multi memory descriptor\n");
		packet->mbuf_md->initWithMbuf(NULL, kIODirectionNone);
		packet_bufdesc_pool->setObject(packet->mem);
		packet->mem->release();
		return kIOReturnOutputDropped;
	}

	size_t head_len = mbuf_len(packet_mbuf);

	// initialise the packet buffer header
	packet->header.flags = 0;
	packet->header.csum_start = 0;
	packet->header.csum_offset = 0;
	packet->header.gso_type = VIRTIO_NET_HDR_GSO_NONE;
	packet->header.hdr_len = 0;
	packet->header.gso_size = 0;
	
	
	struct ip* ip_hdr = NULL;
	unsigned ip_hdr_len = 0;
	if (requested_tcp_csum || requested_tsov4 || requested_udp_csum)
	{
		void* hdr_data = mbuf_data(packet_mbuf);
		char* ip_start = static_cast<char*>(hdr_data) + ETHER_HDR_LEN;
		ip_hdr = reinterpret_cast<struct ip*>(ip_start);
		ip_hdr_len = ip_hdr->ip_hl * 4;
	}
	
	if (requested_tsov4 && !requested_tcp_csum)
	{
		// force checksum offloading if TSO is active, as each segment will need its own checksum
		requested_tcp_csum = true;
	}
	if (requested_tcp_csum)
	{
		// write the appropriate fields to activate checksumming and calculate pseudo-header partial checksum if needed
		virtio_net_enable_tcp_csum(
			packet,
			!requested_tsov4, //Partial checksum needed only for non-TSO packets
			packet_mbuf, ip_hdr_len, ip_hdr);
	}
	
	IOReturn ret = packet->dma_cmd->setMemoryDescriptor(packet->dma_md, true /* prepare */);
	if (ret != kIOReturnSuccess)
	{
		VIOLog("virtio-net addPacketToQueue(): Failed to set memory descriptor for DMA command: %x\n", ret);
		packet->dma_md->initWithDescriptorRanges(NULL, 0, kIODirectionNone, false);
		packet->mbuf_md->initWithMbuf(NULL, kIODirectionNone);
		packet_bufdesc_pool->setObject(packet->mem);
		packet->mem->release();
		return kIOReturnOutputDropped;
	}
	
	const uint16_t direction_flag = for_writing ? VRING_DESC_F_WRITE : 0;

	// array for the allocated descriptors, -1 = not yet allocated
	int32_t descs[max_segs];
	for (unsigned i = 0; i < max_segs; ++i)
		descs[i] = -1;

	struct virtio_net_cursor_segment_context segment_context = {
		this, queue, descs, max_segs, -1, direction_flag, 0, false, false, 0
	};
	
	UInt64 offset = 0;
	UInt32 segments = max_segs;
	ret = packet->dma_cmd->genIOVMSegments(&offset, &segment_context, &segments);
	if (ret != kIOReturnSuccess || offset < packet->dma_md->getLength())
	{
	
		kprintf("virtio-net addPacketToQueue(): an error %x occurred. %u segments produced, offset reached %llu (of %llu) "
			"max index: %d, error: %s, out of descriptors: %s, total length so far %u, descriptors allocated: %u, max segments: %u, free descriptors: %u.\n",
			ret, (uint32_t)segments, offset, (uint64_t)packet->dma_md->getLength(), segment_context.max_segment_created,
			segment_context.error ? "yes" : "no", segment_context.out_of_descriptors ? "yes" : "no", segment_context.total_len,
			segment_context.descs_allocd, max_segs, queue.num_free_desc);
		for (unsigned i = 0; i < max_segs; ++i)
		{
			if (descs[i] >= 0)
			{
				vring_push_free_desc(queue, descs[i]);
			}
		}
		packet->dma_md->initWithDescriptorRanges(NULL, 0, kIODirectionNone, false);
		packet->mbuf_md->initWithMbuf(NULL, kIODirectionNone);
		packet_bufdesc_pool->setObject(packet->mem);
		packet->mem->release();
		return kIOReturnOutputStall;
	}
	
	if (offset != packet->dma_md->getLength())
	{
		VIOLog("virtio-net addPacketToQueue(): genIOVMSegments moved offset to %llu (expected %llu). %u segments created (max %u)\n", offset, (uint64_t)packet->dma_md->getLength(), (uint32_t)segments, max_segs);
	}
	if (segments != segment_context.max_segment_created + 1)
	{
		VIOLog("virtio-net addPacketToQueue(): genPhysicalSegments reports %lu, max index is %d!\n", (unsigned long)segments, segment_context.max_segment_created);
	}

	uint16_t head_desc = descs[0];
	uint16_t prev = head_desc;
	for (unsigned i = 1; i < segments; ++i)
	{
		if (descs[i] < 0)
		{
			VIOLog("virtio-net addPacketToQueue(): no descriptor for index %u (%u total), max seg %d\n", i, (uint32_t)segments, segment_context.max_segment_created);
			continue;
		}
		queue.desc[prev].next = descs[i];
		queue.desc[prev].flags |= VRING_DESC_F_NEXT;
		prev = descs[i];
	}

	// finally, request TSO if necessary
	if (requested_tsov4)
	{
		//IOLog("virtio-net addPacketToQueue(): TSO4 packet.\n");
		static unsigned tso_packets = 0;
		static unsigned tso4_packets = 0;
		tso_packets++;
				
				
		++tso4_packets;
		packet->header.gso_type = VIRTIO_NET_HDR_GSO_TCPV4;
		int max_head_len = kIOEthernetMaxPacketSize - kIOEthernetCRCSize - tso_val;
		if (max_head_len >= 14 + 20 + 20) /* ethernet + IP + TCP */
		{
			if (head_len != max_head_len)
			{
				VIOLog("virtio-net addPacketToQueue(): Warning! head mbuf %lu does not match mtu-seg %d\n", head_len, max_head_len);
			}
		}
		packet->header.hdr_len = head_len; // not sure if this is right...
		packet->header.gso_size = tso_val;
#if 0				
				if (tso_packets % 100 == 1)
				{
					VIOLog("virtio-net addPacketToQueue: %u TSO (%u TSO4) packets processed, current packet's length %u, segment size %u, ethernet+ip hdr len %u, total header len %lu, %u segments.\n",
						tso_packets, tso4_packets, segment_context.total_len, packet->header.gso_size, packet->header.hdr_len, head_len, segments);
//#if 0
					uint16_t desc = head_desc;
					while (true)
					{
						// TODO: Defend against infinite loop (out of range descriptors, circular lists)
						/* note that lacking any driver bugs, this will only defend against a
						 * buggy/malicious hypervisor, which is like fighting windmills.
						 */
						VIOLog("virtio-net addPacketToQueue(): used buffer %u: length %u.\n",
							desc, tx_queue.desc[desc].len, packet);
						if (0 == (tx_queue.desc[desc].flags & VRING_DESC_F_NEXT))
							break;
						desc = tx_queue.desc[desc].next;
					}
//#endif
				}
#endif		
	}

	
	// ensure we can find the packet again
	queue.packets_for_descs[head_desc] = packet;
	
	// add the chain to the available ring
	queue.avail->ring[at_avail_idx % queue.num] = head_desc;
	++at_avail_idx;
	
	return kIOReturnSuccess;
}

/// Fill the receive queue with buffers for the first time and make them available to the device
/** Each packet will have a 10-byte header (virtio_net_hdr) and an mbuf with the
 * maximum ethernet packet size. Separate virtqueue buffers are used for header
 * and packet so that the packet can be handed off to the network subsystem
 * without copying. We might be able to place the header in leading space, but
 * we'll leave that as an optimisation for later.
 * The packet may be split over multiple buffers (max 2 for now in practice) as
 * we need physical addresses.
 */
bool PJVirtioNet::populateReceiveBuffers()
{
	uint16_t avail_idx = rx_queue.avail->idx;
	bool added = false;
	// with less than 2 descriptors remaining, we definitely won't be able to fit another packet
	while (rx_queue.num_free_desc >= 2)
	{
		// allocate data buffer and header memory
		mbuf_t packet_mbuf = allocatePacket(kIOEthernetMaxPacketSize);
		if (!packet_mbuf)
		{
			static int alloc_fail_count = 0;
			if (alloc_fail_count % 10 == 0 && alloc_fail_count < 100)
				VIOLog("virtio-net populateReceiveBuffers(): Warning! Failed to allocate mbuf for receiving (%d).\n", alloc_fail_count);
			++alloc_fail_count;
			
			return (added && notifyQueueAvailIdx(rx_queue, avail_idx)), false;
		}
		
		{
			size_t len = mbuf_len(packet_mbuf);
			if (len != kIOEthernetMaxPacketSize)
				kprintf("virtio-net populateReceiveBuffers(): unexpected new packet length %lu (wanted: %u)\n",
					len, kIOEthernetMaxPacketSize);
			assert(len == kIOEthernetMaxPacketSize);
		}
		
		// this will increment avail_idx if successful
		IOReturn add_ret = addPacketToQueue(packet_mbuf, rx_queue, true /* packet is writeable */, avail_idx);
		if (add_ret != kIOReturnSuccess)
		{
			static int add_fail_count = 0;
			if (add_fail_count % 10 == 0 && add_fail_count < 100)
				VIOLog("virtio-net populateReceiveBuffers(): Warning! Failed to add packet to receive queue (%d).\n", add_fail_count);
			++add_fail_count;
			return freePacket(packet_mbuf), (added && notifyQueueAvailIdx(rx_queue, avail_idx)), false;
		}
		
		added = true;
	}
	if (added)
		notifyQueueAvailIdx(rx_queue, avail_idx); 
	
	return true;
}

void PJVirtioNet::releaseSentPackets(bool from_debugger)
{
	if (!from_debugger && (!work_loop || !work_loop->inGate()))
	{
		kprintf("virtio-net releaseSentPackets(): Warning! Not holding work-loop gate!\n");
	}
	
	if (!from_debugger)
	{
		// free any packets dequeued by the debugger
		virtio_net_packet* cur = transmit_packets_to_free;
		transmit_packets_to_free = NULL;
		while (cur)
		{
			virtio_net_packet* next = cur->next_free;
			
			freePacket(cur->mbuf);
			if (cur->mem)
			{
				packet_bufdesc_pool->setObject(cur->mem);
				cur->mem->release();
			}
			cur = next;
		}
	}

	while (tx_queue.last_used_idx != tx_queue.used->idx)
	{
		vring_used_elem& used = tx_queue.used->ring[tx_queue.last_used_idx % tx_queue.num];
		uint16_t used_desc = used.id;
		if (used_desc == UINT16_MAX && used.len == 0)
		{
			// this was already dealt with by the debugger
			if (!from_debugger)
				kprintf("virtio-net releaseSentPackets(): Warning! Detected 'used' slot used for debugger packet.\n");
			++tx_queue.last_used_idx;
			continue;
		}
		if (used_desc >= tx_queue.num)
		{
			VIOLog("virtio-net releaseSentPackets(): Warning! Out of range descriptor index %u in used transmit queue.\n", used_desc);
			++tx_queue.last_used_idx;
			continue;
		}
		virtio_net_packet* packet = tx_queue.packets_for_descs[used_desc];

		if (packet)
		{
			packet->dma_cmd->clearMemoryDescriptor();
			packet->dma_md->initWithDescriptorRanges(NULL, 0, kIODirectionNone, false);
			packet->mbuf_md->initWithMbuf(NULL, kIODirectionNone);
			
			if (packet == debugger_transmit_packet)
			{
				// this packet was queued with the debugging API - this is weird
				tx_queue.packets_for_descs[used_desc] = NULL;
				kprintf("virtio-net: releaseSentPackets(): detected KDP transmit packet, this should normally not happen (debugger%s active).\n", from_debugger ? "" : " not");
				freeDescriptorChain(tx_queue, used_desc);
				++tx_queue.last_used_idx;
				continue;
			}
			else if (packet->mbuf)
			{
				if (from_debugger)
				{
					// in the debugger, just put all packets to free in a linked list to avoid memory operations
					packet->next_free = transmit_packets_to_free;
					transmit_packets_to_free = packet;
					freeDescriptorChain(tx_queue, used_desc);
					++tx_queue.last_used_idx;
					continue;
				}
				freePacket(packet->mbuf);
			}
			else
			{
				VIOLog("virtio-net releaseSentPackets(): warning, packet with no mbuf, probably leaking memory.\n");
			}
			packet->mbuf = NULL;
			IOBufferMemoryDescriptor* mem = packet->mem;
			if (mem)
			{
				packet_bufdesc_pool->setObject(mem);
			}
			else
			{
				VIOLog("virtio-net releaseSentPackets(): warning, packet with no memory descriptor, probably leaking memory.\n");
			}
			OSSafeReleaseNULL(mem);
		}
		else
		{
			VIOLog("virtio-net releaseSentPackets(): warning, used transmit buffer chain without matching packet reported. Probably leaking memory.\n");
			VIOLog("virtio-net releaseSentPackets(): tx queue used element: desc %u, length %u\n", used.id, used.len);
			uint16_t desc = used_desc;
			uint16_t desc2 = used_desc;
			while (true)
			{
				// TODO: Defend against infinite loop (out of range descriptors, circular lists)
				/* note that lacking any driver bugs, this will only defend against a
				 * buggy/malicious hypervisor, which is like fighting windmills.
				 */
				VIOLog("virtio-net releaseSentPackets(): used buffer %u: length %u, associated packet: %p\n",
					desc2, tx_queue.desc[desc2].len, packet);
				if (desc2 >= tx_queue.num || 0 == (tx_queue.desc[desc2].flags & VRING_DESC_F_NEXT))
					break;
				desc2 = tx_queue.desc[desc2].next;
				if (desc == desc2)
					break;
				VIOLog("virtio-net releaseSentPackets(): used buffer %u: length %u, associated packet: %p\n",
					desc2, tx_queue.desc[desc2].len, packet);
				if (desc2 >= tx_queue.num || 0 == (tx_queue.desc[desc2].flags & VRING_DESC_F_NEXT))
					break;
				desc2 = tx_queue.desc[desc2].next;
				desc = tx_queue.desc[desc].next;
				if (desc == desc2)
					break;
			}
		}
		
		// recycle the descriptors
		freeDescriptorChain(tx_queue, used_desc);
		++tx_queue.last_used_idx;
	}

	// clear any stall condition
	if (!from_debugger && was_stalled)
	{
		//IOLog("virtio-net: Unsticking the output queue after a stall\n");
		was_stalled = false;
		getOutputQueue()->start();
	}
}

void PJVirtioNet::handleReceivedPackets()
{
	if (!work_loop || !work_loop->inGate())
	{
		kprintf("virtio-net handleReceivedPackets(): Warning! Not holding work-loop gate!\n");
	}
	unsigned packets_submitted = 0;
	//IOLog("virtio-net: handleReceivedPackets(): rx queue used: %u, last time: %u\n",
	//	rx_queue.used->idx, rx_queue.last_used_idx);
	while (rx_queue.last_used_idx != rx_queue.used->idx)
	{
		vring_used_elem& used = rx_queue.used->ring[rx_queue.last_used_idx % rx_queue.num];
		uint16_t used_desc = used.id;
		if (used_desc >= rx_queue.num)
		{
			if (!(used_desc == UINT16_MAX && used.len == 0))
			{
				VIOLog("virtio-net handleReceivedPackets(): Warning! Out of range descriptor %u in used ring, skipping.\n",
					used_desc);
			}
			++rx_queue.last_used_idx;
			continue;
		}
		virtio_net_packet* packet = rx_queue.packets_for_descs[used_desc];
		
		// work out actual packet length, without the header
		uint32_t len = used.len;
		if (len >= sizeof(virtio_net_hdr))
		{
			len -= static_cast<uint32_t>(sizeof(virtio_net_hdr));
		}
		else
		{
			len = 0;
		}
		
		// this buffer should correspond to a packet we queued; pass it to the network system
		if (packet)
		{
			rx_queue.packets_for_descs[used_desc] = NULL;

			packet->dma_cmd->clearMemoryDescriptor();
			packet->dma_md->initWithDescriptorRanges(NULL, 0, kIODirectionNone, false);
			packet->mbuf_md->initWithMbuf(NULL, kIODirectionNone);

			if (interface && len <= kIOEthernetMaxPacketSize && packet->mbuf)
			{
				interface->inputPacket(packet->mbuf, len, IONetworkInterface::kInputOptionQueuePacket);
				++packets_submitted;
			}
			else
			{
				kprintf("virtio-net handleReceivedPackets(): warning, no interface (%p), no mbuf (%p) or bad packet length (%u) reported by device. Ignoring packet.\n",
					interface, packet->mbuf, len);
				kprintf("virtio-net handleReceivedPackets(): used ring entry: id=%u (0x%x), len=%u\n", used.id, used.id, used.len);
				kprintf("virtio-net handleReceivedPackets(): packet dump: flags=0x%02x, gso_type=0x%02x, hdr_len=%u(0x%04x), gso_size=%u(0x%04x) csum_start=%u csum_offset=%u\n",
					packet->header.flags, packet->header.gso_type, packet->header.hdr_len, packet->header.hdr_len,
					packet->header.gso_size, packet->header.gso_size, packet->header.csum_start, packet->header.csum_offset);
				kprintf("virtio-net handleReceivedPackets(): packet dump: mbuf=%p, mem=%p\n", packet->mbuf, packet->mem);
				if (packet->mbuf)
					freePacket(packet->mbuf);
			}
			packet->mbuf = NULL;
			IOBufferMemoryDescriptor* mem = packet->mem;
			if (mem)
				packet_bufdesc_pool->setObject(mem);
			OSSafeReleaseNULL(mem);
		}
		else
		{
			VIOLog("virtio-net handleReceivedPackets(): warning, used receive buffer chain without matching packet reported. Probably leaking memory.\n");
			VIOLog("virtio-net: handleReceivedPackets(): rx queue used element: desc %u, length %u\n", used.id, used.len);
			uint16_t desc = used_desc;
			while (true)
			{
				// TODO: Defend against infinite loop (out of range descriptors, circular lists)
				/* note that lacking any driver bugs, this will only defend against a
				 * buggy/malicious hypervisor, which is like fighting windmills.
				 */
				VIOLog("virtio-net: handleReceivedPackets(): used buffer %u: length %u, associated packet: %p\n",
					desc, rx_queue.desc[desc].len, packet);
				if (0 == (rx_queue.desc[desc].flags & VRING_DESC_F_NEXT))
					break;
				desc = rx_queue.desc[desc].next;
			}
		}
		
		// recycle the descriptors
		freeDescriptorChain(rx_queue, used_desc);
		++rx_queue.last_used_idx;
	}
	if (packets_submitted > 0)
	{
		interface->flushInputQueue();
		//IOLog("virtio-net handleReceivedPackets(): %u received\n", packets_submitted);
	}
}

void PJVirtioNet::freeDescriptorChain(virtio_net_virtqueue& queue, uint16_t desc_chain_head)
{
	uint16_t desc = desc_chain_head; 
	while (true)
	{
		// TODO: Defend against infinite loop (out of range descriptors, circular lists)
		/* note that lacking any driver bugs, this will only defend against a
		 * buggy/malicious hypervisor, which is like fighting windmills.
		 */
		uint16_t next = queue.desc[desc].next;
		bool has_next = (0 != (queue.desc[desc].flags & VRING_DESC_F_NEXT));
		queue.desc[desc].addr = 0;
		queue.desc[desc].len = 0;
		queue.desc[desc].flags = 0;
		queue.desc[desc].next = 0;
		vring_push_free_desc(queue, desc);
		if (!has_next)
			break;
		desc = next;
	}
}


const OSString* PJVirtioNet::newVendorString() const
{
	return OSString::withCStringNoCopy("Virtio");
}


const OSString* PJVirtioNet::newModelString() const
{
	return OSString::withCStringNoCopy("Paravirtual Ethernet Adapter");
}

void PJVirtioNet::flushPacketPool()
{
	if (packet_bufdesc_pool)
	{
		while (OSObject* obj = packet_bufdesc_pool->getAnyObject())
		{
			if (IOBufferMemoryDescriptor* buf = OSDynamicCast(IOBufferMemoryDescriptor, obj))
			{
				virtio_net_packet* packet = static_cast<virtio_net_packet*>(buf->getBytesNoCopy());
				OSSafeReleaseNULL(packet->dma_cmd);
				OSSafeReleaseNULL(packet->dma_md);
				OSSafeReleaseNULL(packet->mbuf_md);
			}
			packet_bufdesc_pool->removeObject(obj);
		}
	}
}

void PJVirtioNet::stop(IOService* provider)
{
	PJLogVerbose("virtio-net stop()\n");
	if (provider != this->pci_dev)
		VIOLog("Warning: stopping virtio-net with a different provider!?\n");

	if (interface)
	{
		detachInterface(interface, true);
	}
	if (driver_state == kDriverStateEnabled || driver_state == kDriverStateEnabledBoth)
	{
		VIOLog("virtio-net stop(): Warning! Device is still enabled. Disabling it.\n");
		disable(interface);
	}
	
	if (debugger)
	{
		detachDebuggerClient(debugger);
		debugger = NULL;
	}

	// we don't want interrupt handlers trying to dereference any zeroed or dangling pointers, so make sure they're off
	if (intr_event_source)
	{
		VIOLog("virtio-net stop(): Warning! Event source still exists, this should have been shut down by now.\n");
		endHandlingInterrupts();
	}
	
	if (debugger_transmit_packet)
	{
		if (debugger_transmit_packet->mbuf)
			freePacket(debugger_transmit_packet->mbuf);
		debugger_transmit_packet->mbuf = NULL;
		OSSafeReleaseNULL(debugger_transmit_packet->dma_cmd);
		OSSafeReleaseNULL(debugger_transmit_packet->dma_md);
		OSSafeReleaseNULL(debugger_transmit_packet->mbuf_md);
		if (debugger_transmit_packet->mem)
			debugger_transmit_packet->mem->release();
		debugger_transmit_packet = NULL;
	}

	if (driver_state == kDriverStateEnabled || driver_state == kDriverStateEnabledBoth || driver_state == kDriverStateEnabledDebugging)
	{
		disablePartial();
	}
	
	flushPacketPool();
	
	OSSafeReleaseNULL(interface);
	clearVirtqueuePackets(rx_queue);
	clearVirtqueuePackets(tx_queue);
	virtqueue_free(rx_queue);
	virtqueue_free(tx_queue);
	
	OSSafeReleaseNULL(this->pci_virtio_header_iomap);

	if (this->pci_dev && this->should_disable_io)
		this->pci_dev->setIOEnable(false);
	this->should_disable_io = false;

	if (pci_dev && pci_dev->isOpen(this))
		pci_dev->close(this);
	this->pci_dev = NULL;
	driver_state = kDriverStateStopped;
	
	PJLogVerbose("virtio-net end stop()\n");
	super::stop(provider);
	PJLogVerbose("virtio-net end super::stop()\n");
}

void PJVirtioNet::free()
{
	PJLogVerbose("virtio-net free()\n");
	
	OSSafeReleaseNULL(packet_bufdesc_pool);

	OSSafeReleaseNULL(this->pci_virtio_header_iomap);

	if (intr_event_source)
	{
		VIOLog("virtio-net free(): Warning! Event source still exists, this should have been shut down by now.\n");
		endHandlingInterrupts();
	}
	OSSafeReleaseNULL(work_loop);
	if (this->pci_dev && this->pci_dev->isOpen(this))
		this->pci_dev->close(this);
	this->pci_dev = NULL;
	
#ifdef VIRTIO_NET_SINGLE_INSTANCE
	OSDecrementAtomic(&instances);
#endif

	super::free();
}


IOReturn PJVirtioNet::getHardwareAddress(IOEthernetAddress* addrP)
{
	if (!mac_address_is_valid)
	{
		VIOLog("virtio-net getHardwareAddress(): Warning! MAC address not ready, this shouldn't normally happen.\n");
		return kIOReturnNotReady;
	}
	*addrP = mac_address;
	return kIOReturnSuccess;
}
