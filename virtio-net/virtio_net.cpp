/* Copyright 2011 Phil Jordan <phil@philjordan.eu>
 * Dual-licensed under the 3-clause BSD License and the zLib license */

#include "virtio_net.h"
#include <IOKit/pci/IOPCIDevice.h>
#include "virtio_ring.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>
#include <IOKit/network/IOEthernetInterface.h>
#include <IOKit/network/IOGatedOutputQueue.h>
#include <IOKit/network/IOMbufMemoryCursor.h>
#include <IOKit/IOFilterInterruptEventSource.h>

// darwin doesn't have inttypes.h TODO: build an inttypes.h for the kernel
#ifndef PRIuPTR
#define PRIuPTR "%lu"
#endif
#ifndef PRIXPTR
#define PRIXPTR "%lX"
#endif
#ifndef PRIX64
#define PRIX64 "%llX"
#endif

OSDefineMetaClassAndStructors(eu_philjordan_virtio_net, IOEthernetController);
#define super IOEthernetController

template <typename T> static T* PJZMallocArray(size_t length)
{
	const size_t bytes = sizeof(T) * length;
	void* const mem = IOMalloc(bytes);
	if (!mem) return NULL;
	memset(mem, 0, bytes);
	return static_cast<T*>(mem);
}

template <typename T> static void PJFreeArray(T* array, size_t length)
{
	const size_t bytes = sizeof(T) * length;
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

bool eu_philjordan_virtio_net::init(OSDictionary* properties)
{
	IOLog("virtio-net init()\n");
	bool ok = super::init(properties);
	if (!ok)
		return false;
	
	packet_bufdesc_pool = OSSet::withCapacity(16);
	if (!packet_bufdesc_pool)
		return false;
	
	pci_dev = NULL;
	rx_queue.buf = NULL;
	this->pci_config_mmap = NULL;
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

template <typename T> void release_obj(T*& obj)
{
	OSObject* const o = obj;
	if (o)
	{
		o->release();
		obj = NULL;
	}
}

IOService* eu_philjordan_virtio_net::probe(IOService* provider, SInt32* score)
{
	IOLog("virtio-net probe()\n");
	IOPCIDevice* pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (!pci_dev)
		return NULL;
	
	OSObject* vendor_id = pci_dev->getProperty("vendor-id");
	OSObject* device_id = pci_dev->getProperty("device-id");
	OSObject* revision_id = pci_dev->getProperty("revision-id");
	OSObject* subsystem_vendor_id = pci_dev->getProperty("subsystem-vendor-id");
	OSObject* subsystem_id = pci_dev->getProperty("subsystem-id");
	
	int64_t vid = -1, did = -1, revid = -1, sub_id = -1, sub_vid = -1;
	vid = pci_id_data_to_uint(vendor_id);
	IOLog("virtio-net probe(): vendor ID = %lld (0x%llX)\n", vid, vid);
	did = pci_id_data_to_uint(device_id);
	IOLog("virtio-net probe(): device ID = %lld (0x%llX)\n", did, did);
	revid = pci_id_data_to_uint(revision_id);
	IOLog("virtio-net probe(): revision ID = %lld (0x%llX)\n", revid, revid);
	sub_id = pci_id_data_to_uint(subsystem_id);
	IOLog("virtio-net probe(): subsystem ID = %lld (0x%llX)\n", sub_id, sub_id);
	sub_vid = pci_id_data_to_uint(subsystem_vendor_id);
	IOLog("virtio-net probe(): subsystem vendor ID = %lld (0x%llX)\n", sub_vid, sub_vid);
		
	if (vid != 0x1AF4)
	{
		IOLog("Vendor ID does not match 0x1AF4\n");
		return NULL;
	}
	if (did < 0x1000 || did > 0x103F)
	{
		IOLog("Device ID does not lie in the range 0x1000 to 0x103F (inclusive)\n");
		return NULL;
	}
	if (revid != 0)
	{
		IOLog("Only virtio devices with revision ID 0 are supported by this driver\n");
		return NULL;
	}
	if (sub_id != 1)
	{
		IOLog("Only virtio devices with subsystem ID 1 (= network card) are supported by this driver\n");
		return NULL;
	}
	if (sub_vid != vid)
	{
		IOLog("Warning: subsystem vendor ID should normally match device vendor ID.\n");
	}
	return this;
}

enum VirtioPCIConfigHeaderOffsets
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

#define VIRTIO_PCI_DEVICE_ISR_USED 0x01
#define VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE 0x02

/// Virtio Spec 0.9, section 2.2.2.1, "Device Status".
/** To be used in the "Device Status" configuration field. */
enum VirtioPCIDeviceStatus
{
	VIRTIO_PCI_DEVICE_STATUS_RESET = 0x00,
	VIRTIO_PCI_DEVICE_STATUS_ACKNOWLEDGE = 0x01,
	VIRTIO_PCI_DEVICE_STATUS_DRIVER = 0x02,
	/* Confusingly, the spec gives BIT 3 for driver_ok and VALUE 128 (bit 8) for failed: */
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
	
	
	// generic virtio features
	VIRTIO_F_NOTIFY_ON_EMPTY = (1u << 24u),
	VIRTIO_F_RING_INDIRECT_DESC = (1u << 28u),
	VIRTIO_F_RING_EVENT_IDX = (1u << 29u),
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
#define VIRTIO_NET_HDR_GSO_TCPV4 2
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
	// Used as the first virtqueue buffer.
	virtio_net_hdr header;
	// The mbuf used for the packet body
	mbuf_t mbuf;
	// The memory descriptor holding this packet structure
	IOBufferMemoryDescriptor* mem;
};


static void log_feature(uint32_t feature_bitmap, uint32_t feature, const char* feature_name)
{
	if (feature_bitmap & feature)
	{
		IOLog("%s\n", feature_name);
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
	IOLog("virtio-net start(): Device reports LOW feature bitmap 0x%08x.\n", dev_features);
	IOLog("virtio-net start(): Recognised generic virtio features:\n");
	LOG_FEATURE(dev_features, VIRTIO_F_NOTIFY_ON_EMPTY);    // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_F_RING_INDIRECT_DESC);
	LOG_FEATURE(dev_features, VIRTIO_F_RING_EVENT_IDX);
	LOG_FEATURE(dev_features, VIRTIO_F_BAD_FEATURE);        // Must mask this out
	LOG_FEATURE(dev_features, VIRTIO_F_FEATURES_HIGH);
	
	IOLog("virtio-net start(): Recognised virtio-net specific features:\n");
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CSUM);           // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_CSUM);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_MAC);            // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GSO);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_TSO4);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_TSO6);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_ECN);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_GUEST_UFO);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_TSO4);      // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_TSO6);      // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_ECN);
	LOG_FEATURE(dev_features, VIRTIO_NET_F_HOST_UFO);       // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_MRG_RXBUF);      // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_STATUS);         // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_VQ);        // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_RX);        // Supported by VBox 4.1.0
	LOG_FEATURE(dev_features, VIRTIO_NET_F_CTRL_VLAN);      // Supported by VBox 4.1.0
	
	
	uint32_t unrecognised = dev_features & ~static_cast<uint32_t>(VIRTIO_ALL_KNOWN_FEATURES);
	if (unrecognised > 0)
	{
		IOLog("Feature bits not recognised by this driver: 0x%08x\n", unrecognised);
	}
}

ssize_t virtio_hi_feature_bitmap_offset(uint32_t dev_features, size_t& config_offset)
{
	ssize_t hi_features_offset = -1;
	if (dev_features & VIRTIO_F_FEATURES_HIGH)
	{
		// Has extended feature table
		hi_features_offset = config_offset;
		config_offset += 2 * 4;
	}
	return hi_features_offset;
}

void eu_philjordan_virtio_net::configWrite8(uint16_t offset, uint8_t val)
{
	pci_dev->ioWrite8(offset, val, pci_config_mmap);
}
void eu_philjordan_virtio_net::configWrite16(uint16_t offset, uint16_t val)
{
	pci_dev->ioWrite16(offset, val, pci_config_mmap);
}
void eu_philjordan_virtio_net::configWrite32(uint16_t offset, uint32_t val)
{
	pci_dev->ioWrite32(offset, val, pci_config_mmap);
}
uint8_t eu_philjordan_virtio_net::configRead8(uint16_t offset)
{
	return pci_dev->ioRead8(offset, pci_config_mmap);
}
uint16_t eu_philjordan_virtio_net::configRead16(uint16_t offset)
{
	return pci_dev->ioRead16(offset, pci_config_mmap);
}
uint32_t eu_philjordan_virtio_net::configRead32(uint16_t offset)
{
	return pci_dev->ioRead32(offset, pci_config_mmap);
}

void eu_philjordan_virtio_net::configWriteLE32(uint16_t offset, uint32_t val)
{
	configWrite32(offset, OSSwapHostToLittleInt32(val));
}
uint32_t eu_philjordan_virtio_net::configReadLE32(uint16_t offset)
{
	return OSSwapLittleToHostInt32(configRead32(offset));
}
void eu_philjordan_virtio_net::configWriteLE16(uint16_t offset, uint16_t val)
{
	configWrite16(offset, OSSwapHostToLittleInt16(val));
}
uint16_t eu_philjordan_virtio_net::configReadLE16(uint16_t offset)
{
	return OSSwapLittleToHostInt16(configRead16(offset));
}


void eu_philjordan_virtio_net::setVirtioDeviceStatus(uint8_t status)
{
	configWrite8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, status);
}
void eu_philjordan_virtio_net::updateVirtioDeviceStatus(uint8_t status)
{
	uint8_t old_status = configRead8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS);
	setVirtioDeviceStatus(status | old_status);
}

void eu_philjordan_virtio_net::failDevice()
{
	if (pci_dev && pci_config_mmap)
	{
		updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_FAILED);
	}
}

bool eu_philjordan_virtio_net::interruptFilter(OSObject* me, IOFilterInterruptEventSource* source)
{
	eu_philjordan_virtio_net* virtio_net = OSDynamicCast(eu_philjordan_virtio_net, me);
	if (!virtio_net || source != virtio_net->intr_event_source)
		return true; // this isn't really for us
	
	// check if anything interesting has happened
	uint8_t isr = virtio_net->configRead8(VIRTIO_PCI_CONF_OFFSET_ISR_STATUS);
	virtio_net->last_isr = isr;
	if (isr & VIRTIO_PCI_DEVICE_ISR_USED)
	{
		virtio_net->last_isr = isr;
		if (isr & VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE)
		{
			virtio_net->received_config_change = true;
		}
#warning TODO: disable further interrupts until the handler has run
		return true;
	}
	return false;
}

void eu_philjordan_virtio_net::interruptAction(OSObject* me, IOInterruptEventSource* source, int count)
{
	eu_philjordan_virtio_net* virtio_net = OSDynamicCast(eu_philjordan_virtio_net, me);
	if (!virtio_net || source != virtio_net->intr_event_source)
		return;
	
	virtio_net->interruptAction(source, count);
}

void eu_philjordan_virtio_net::interruptAction(IOInterruptEventSource* source, int count)
{
	//IOLog("Last ISR value: 0x%02X\n", last_isr);
	uint8_t unknown_isr = last_isr & ~(VIRTIO_PCI_DEVICE_ISR_USED | VIRTIO_PCI_DEVICE_ISR_CONF_CHANGE);
	if (unknown_isr)
	{
		IOLog("virtio-net interruptAction(): Unknown bits set in ISR status register: %02X\n", unknown_isr);
	}
	
	// Handle received packets
	if (rx_queue.last_used_idx != rx_queue.used->idx)
	{
		handleReceivedPackets();
		populateReceiveBuffers();
		/*
		IOLog("virtio-net interruptAction(): Populated receive buffers: %u free descriptors left, avail idx %u\n",
			rx_queue.num_free_desc, rx_queue.avail->idx);
		*/
	}
	// Dispose of any completed sent packets
	if (tx_queue.last_used_idx != tx_queue.used->idx)
	{
		kprintf("TX queue last used idx: %u, tx_queue used: %p, desc: %p, avail: %p, free: %u, head: %u\n",
			tx_queue.last_used_idx, tx_queue.used, tx_queue.desc, tx_queue.avail, tx_queue.num_free_desc, tx_queue.free_desc_head);
		IOLog("TX queue last used idx (%u) differs from current (%u)\n", tx_queue.last_used_idx, tx_queue.used->idx);
		
		
		
	}
}




bool eu_philjordan_virtio_net::start(IOService* provider)
{
	IOLog("virtio-net start(%p)\n", provider);
	IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
	{
		if (!provider)
			return false;
		const OSMetaClass* meta = provider->getMetaClass();
		IOLog("virtio-net start(): Provider (%p) has wrong type: %s (expected IOPCIDevice)\n", provider, meta->getClassName());
		return false;
	}
	
	if (!super::start(provider))
		return false;

	if (!pci->open(this))
		return false;
	this->pci_dev = pci;	

	work_loop = getWorkLoop();
	if (!work_loop)
		return false;
	work_loop->retain();
	
	
	//IOLog("virtio-net start(): attempting to map device memory with register 0\n");
	IODeviceMemory* devmem = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!devmem)
	{
		IOLog("virtio-net start(): Getting memory descriptor failed.\n");
		return false;
	}
	IOMemoryMap* iomap = devmem->map();
	devmem = NULL; // no longer needed - do not release, however (see getDeviceMemoryWithRegister() docs)
	if (!iomap)
	{
		IOLog("virtio-net start(): Mapping failed.\n");
		return false;
	}
	
	this->pci_config_mmap = iomap;
	
	IOLog("virtio-net start(): Device Initialisation Sequence\n");
	
	// Reset the device just in case it was previously opened and left in a weird state.
	setVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_RESET);

	// IOLog("virtio-net start(): Device reset\n");

	// Acknowledge the device, then tell it we're the driver
	updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_ACKNOWLEDGE);
	// IOLog("virtio-net start(): Device acknowledged\n");

	updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_DRIVER);
	// IOLog("virtio-net start(): Device informed of driver\n");
	
	uint32_t dev_features = configReadLE32(VIRTIO_PCI_CONF_OFFSET_DEVICE_FEATURE_BITS_0_31);
	virtio_log_supported_features(dev_features);
	
	if (!(dev_features & VIRTIO_NET_F_MAC))
	{
#warning TODO: find out how Linux driver handles this situation
		IOLog("virtio-net start(): Device does not support VIRTIO_NET_F_MAC feature. Don't know how to proceed, aborting.\n");
		return failDevice(), false;
	}
	
	/* Read out the flexible config space */
	size_t config_offset = VIRTIO_PCI_CONF_OFFSET_END_HEADER;

#warning TODO: find out how to detect if MSI-X is enabled (I don't think it ever is on Mac OS X)
	// if (msix_enabled) config_offset += 4;
	
	ssize_t hi_features_offset = virtio_hi_feature_bitmap_offset(dev_features, config_offset);
	uint32_t hi_dev_features = 0;
	if (hi_features_offset >= 0)
	{
		hi_dev_features = configReadLE32(hi_features_offset);
		IOLog("virtio-net start(): Devices reports high feature bit mask %08X.\n", hi_dev_features);
	}
	
	// offset for the device-specific configuration space
	size_t device_specific_offset = config_offset;
	
	if (!setupVirtqueue(0, rx_queue))
		return failDevice(), false;
	IOLog("Initialised virtqueue 0 (receive queue) with " PRIuPTR " bytes (%u entries) at " PRIXPTR "\n",
		rx_queue.buf->getLength(), rx_queue.num, rx_queue.buf->getPhysicalAddress());

	if (!setupVirtqueue(1, tx_queue))
		return failDevice(), false;
	IOLog("Initialised virtqueue 1 (transmit queue) with " PRIuPTR " bytes (%u entries) at " PRIXPTR "\n",
		tx_queue.buf->getLength(), tx_queue.num, tx_queue.buf->getPhysicalAddress());

	// sort out mac address
	if (dev_features & VIRTIO_NET_F_MAC)
	{
		for (unsigned i = 0; i < sizeof(mac_address); ++i)
		{
			mac_address.bytes[i] = configRead8(device_specific_offset + offsetof(virtio_net_config, mac[i]));
		}
		IOLog("virtio-net start(): Determined MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
			mac_address.bytes[0], mac_address.bytes[1], mac_address.bytes[2],
			mac_address.bytes[3], mac_address.bytes[4], mac_address.bytes[5]);
	}
	else
	{
		// generate private MAC address?
	}
	
	// Don't support VIRTIO_NET_F_CTRL_VQ for now
	
	// check link status, if possible
	status_field_offset = 0;
	bool link_is_up = true;
	if (dev_features & VIRTIO_NET_F_STATUS)
	{
		status_field_offset = device_specific_offset + offsetof(virtio_net_config, status);
		uint16_t status = readStatus();
		IOLog("virtio-net start(): Link status field 0x%04X (link %s)\n",
			status, (status & VIRTIO_NET_S_LINK_UP) ? "up" : "down");
		link_is_up = (status & VIRTIO_NET_S_LINK_UP) != 0;
	}
	setLinkStatus((link_is_up ? kIONetworkLinkActive : 0) | kIONetworkLinkValid);
	
	// write back supported features
	uint32_t supported_features = dev_features &
		(VIRTIO_F_NOTIFY_ON_EMPTY | VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS);
	configWriteLE32(VIRTIO_PCI_CONF_OFFSET_DEVICE_FEATURE_BITS_0_31, supported_features);
	IOLog("virtio-net start(): Wrote driver-supported feature bits: 0x%08X\n", supported_features);
	
	// tell device we're ready
	updateVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_DRIVER_OK);
	IOLog("virtio-net start(): Device set to 'driver ok' state.\n");

	// fill receive buffer
	#warning TODO: move this and some of the above to enable()
	if (!populateReceiveBuffers())
	{
		if (rx_queue.num_free_desc >= rx_queue.num)
		{
			return false;
		}
	}
	IOLog("virtio-net start(): Populated receive buffers: %u free descriptors left, avail idx %u\n",
		rx_queue.num_free_desc, rx_queue.avail->idx);
	
	if (!getOutputQueue())
	{
		IOLog("virtio-net start(): failed to get output queue\n");
		return false;
	}
	// create the interface nub
	interface = NULL;
	if (!attachInterface((IONetworkInterface **)&interface, false))
	{
		IOLog("virtio-net start(): attachInterface() failed, interface = %p [%s].\n",
			interface, interface ? interface->getMetaClass()->getClassName() : "null");
		return false;
	}
	interface->registerService();
	IOLog("virtio-net start(): interface registered.\n");

	// start handling interrupts now that the internal data structures are set up
	intr_event_source = new IOFilterInterruptEventSource();
	if (!intr_event_source || !intr_event_source->init(this, &interruptAction, &interruptFilter, pci))
	{
		release_obj(intr_event_source);
		return false;
	}
	if (kIOReturnSuccess != work_loop->addEventSource(intr_event_source))
		return false;
	intr_event_source->enable();
	IOLog("virtio-net start(): now handling interrupts, good to go.\n");
	
	return true;
}

bool eu_philjordan_virtio_net::configureInterface(IONetworkInterface *netif)
{
	IOLog("virtio-net configureInterface([%s] @ %p)\n", netif ? netif->getMetaClass()->getClassName() : "null", netif);
	if (!super::configureInterface(netif))
	{
		IOLog("virtio-net configureInterface(): super failed\n");
		return false;
	}
	return true;
}

IOReturn eu_philjordan_virtio_net::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
	IOLog("virtio-net getPacketFilters()\n");
	return super::getPacketFilters(group, filters);
}



int32_t eu_philjordan_virtio_net::readStatus()
{
	if (!status_field_offset) return -1;
	return configRead16(status_field_offset);
}

bool eu_philjordan_virtio_net::setupVirtqueue(
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
	configWriteLE16(VIRTIO_PCI_CONF_OFFSET_QUEUE_SELECT, queue_id);
	uint16_t queue_size = configReadLE16(VIRTIO_PCI_CONF_OFFSET_QUEUE_SIZE);
	if (queue_size == 0)
	{
		IOLog("Queue size for queue %u is 0.\n", queue_id);
		return false;
	}
	else if (!is_pow2(queue_size))
	{
		IOLog("Queue size for queue %u is %u, which is not a power of 2. Aborting.\n", queue_id, queue_size);
		return false;
	}
	IOLog("Reported queue size for queue %u: %u\n", queue_id, queue_size);
	// allocate an appropriately sized DMA buffer. As per the spec, this must be physically contiguous.
	size_t queue_size_bytes = vring_size(queue_size);
	IOBufferMemoryDescriptor* queue_buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOInhibitCache, queue_size_bytes, VIRTIO_RING_ALLOC_MASK);
	if (!queue_buffer)
	{
		IOLog("Failed to allocate queue buffer with " PRIuPTR " contiguous bytes and mask " PRIX64 ".\n",
			queue_size_bytes, VIRTIO_RING_ALLOC_MASK);
		return false;
	}
	memset(queue_buffer->getBytesNoCopy(), 0, queue_size_bytes);
	configWriteLE32(VIRTIO_PCI_CONF_OFFSET_QUEUE_ADDRESS, queue_buffer->getPhysicalAddress() >> 12u);
				
	virtqueue_init(queue, queue_buffer, queue_size, queue_id);
	
	return true;
}
void virtqueue_init(virtio_net_virtqueue& queue, IOBufferMemoryDescriptor* buf, uint16_t queue_size, uint16_t queue_id)
{
	queue.index = queue_id;
	queue.buf = buf;
	vring_init(&queue, queue_size, buf->getBytesNoCopy(), VIRTIO_PAGE_SIZE);
	
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
	release_obj(queue.buf);
	if (queue.packets_for_descs)
	{
		PJFreeArray(queue.packets_for_descs, queue.num);
		queue.packets_for_descs = NULL;
	}
	memset(&queue, 0, sizeof(queue));
}


IOOutputQueue* eu_philjordan_virtio_net::createOutputQueue()
{
	/* For now, go with a gated output queue, as this is the simplest option. Later
	 * on, we can provide more granular access to the virtqueue mechanism. */
	uint32_t capacity = max(16, tx_queue.num / 4); // each packet takes 2 buffers, try to always fill up half the virtqueue, hence a quarter
	IOGatedOutputQueue* queue = IOGatedOutputQueue::withTarget(this, this->getWorkLoop(), capacity);
	IOLog("virtio-net createOutputQueue(): %p\n", queue);
	return queue;
}

IOReturn eu_philjordan_virtio_net::enable(IONetworkInterface* interface)
{
	IOLog("virtio-net enable()\n");
	if (interface != this->interface)
	{
		IOLog("virtio-net enable(): unknown interface %p (expected %p)\n", interface, this->interface);
		return kIOReturnBadArgument;
	}
	
	IOOutputQueue* output_queue = getOutputQueue();
	if (!output_queue)
		return kIOReturnError;
	uint32_t capacity = max(16, tx_queue.num / 4); // each packet takes 2 buffers, try to always fill up half the virtqueue, hence a quarter
	output_queue->setCapacity(capacity);
	output_queue->start();
	
	return kIOReturnSuccess;
}


IOReturn eu_philjordan_virtio_net::disable(IONetworkInterface* interface)
{
	IOLog("virtio-net disable()\n");
	IOOutputQueue* output_queue = getOutputQueue();
	if (output_queue)
	{
		output_queue->stop();
		output_queue->setCapacity(0);
		output_queue->flush();
	}
	
	return kIOReturnSuccess;
}
	
UInt32 eu_philjordan_virtio_net::outputPacket(mbuf_t buffer, void *param)
{
	if (tx_queue.num_free_desc < 3)
	{
		IOLog("virtio-net outputPacket(): Transmit queue full\n");
		return kIOReturnOutputStall;
	}
	
	if (mbuf_len(buffer) > kIOEthernetMaxPacketSize)
	{
		IOLog("virtio-net outputPacket(): dropped oversized packet of length %lu\n", mbuf_len(buffer));
		freePacket(buffer);
		return kIOReturnOutputDropped;
	}
	
	uint16_t avail_idx = tx_queue.avail->idx;
	if (!addPacketToQueue(buffer, tx_queue, false /* packet to be read by device */, avail_idx))
	{
		IOLog("virtio-net outputPacket(): failed to add packet (length: %lu) to queue, dropping it.\n", mbuf_len(buffer));
		freePacket(buffer);
		return kIOReturnOutputDropped;
	}
	notifyQueueAvailIdx(tx_queue, avail_idx);
	return kIOReturnOutputSuccess;
}
	
void eu_philjordan_virtio_net::receivePacket(void *pkt, UInt32 *pktSize, UInt32 timeout)
{
	#warning TODO
}

void eu_philjordan_virtio_net::sendPacket(void *pkt, UInt32 pktSize)
{
	#warning TODO
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

bool eu_philjordan_virtio_net::notifyQueueAvailIdx(virtio_net_virtqueue& queue, uint16_t new_avail_idx)
{
	OSSynchronizeIO();
	queue.avail->idx = new_avail_idx;
	OSSynchronizeIO();
	if (0 == (queue.used->flags & VRING_USED_F_NO_NOTIFY))
	{
		configWrite16(VIRTIO_PCI_CONF_OFFSET_QUEUE_NOTIFY, queue.index);
		return true;
	}
	return false;
}

bool eu_philjordan_virtio_net::addPacketToQueue(mbuf_t packet_mbuf, virtio_net_virtqueue& queue, bool for_writing, uint16_t& at_avail_idx)
{
	// recycle or allocate memory for the packet virtio header buffer
	IOBufferMemoryDescriptor* packet_mem = OSDynamicCast(IOBufferMemoryDescriptor, packet_bufdesc_pool->getAnyObject());
	if (packet_mem)
	{
		packet_mem->retain();
		packet_bufdesc_pool->removeObject(packet_mem);
	}
	else
	{
		packet_mem = IOBufferMemoryDescriptor::inTaskWithOptions(
			kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut | kIOInhibitCache, sizeof(virtio_net_packet),
			sizeof(void*) /* align to pointer */);
	}
	
	if (!packet_mem)
		return false;
	virtio_net_packet* packet = static_cast<virtio_net_packet*>(packet_mem->getBytesNoCopy());

	packet->mbuf = packet_mbuf;
	packet->mem = packet_mem;

	// get the necessary descriptors
	const int32_t head_desc = vring_pop_free_desc(queue);
	if (head_desc < 0)
		return release_obj(packet_mem), false;
	const int32_t main_desc = vring_pop_free_desc(queue);
	if (main_desc < 0)
		return vring_push_free_desc(queue, head_desc), release_obj(packet_mem), false;

	// set up the buffer descriptors
	const uint16_t direction_flag = for_writing ? VRING_DESC_F_WRITE : 0;
	
	vring_desc& head_buf = queue.desc[head_desc];
	head_buf.addr = packet_mem->getPhysicalAddress() + offsetof(virtio_net_packet, header);
	head_buf.len = sizeof(virtio_net_hdr);
	head_buf.flags = VRING_DESC_F_NEXT | direction_flag;
	head_buf.next = main_desc;

	const size_t len = mbuf_len(packet_mbuf);
	
	vring_desc& main_buf = queue.desc[main_desc];
	void* data = mbuf_data(packet_mbuf);
	main_buf.addr = mbuf_data_to_physical(data);
	addr64_t to_next_page = trunc_page_64(main_buf.addr + PAGE_SIZE_64);
	
	if (to_next_page >= len)
	{
		// the whole packet is contained in a physical page
		main_buf.len = len;
		main_buf.flags = direction_flag;
		main_buf.next = UINT16_MAX;
	}
	else
	{
		// split packet into 2 buffers
		main_buf.len = static_cast<uint32_t>(to_next_page);
		main_buf.flags = head_buf.flags;
		int32_t extra_desc = vring_pop_free_desc(queue);
		if (extra_desc < 0)
			return vring_push_free_desc(queue, main_desc), vring_push_free_desc(queue, head_desc),
				release_obj(packet_mem), false;
		main_buf.next = extra_desc;
		vring_desc& extra_buf = queue.desc[extra_desc];
		void* extra_data = static_cast<char*>(data) + to_next_page;
		extra_buf.addr = mbuf_data_to_physical(extra_data);
		extra_buf.len = static_cast<uint32_t>(len - to_next_page);
		assert(extra_buf.len <= PAGE_SIZE); // this is only true because the max size for ethernet packets is smaller than the page size. Otherwise, we'd need to support an arbitrary number of extra buffers
		extra_buf.flags = direction_flag;
		extra_buf.next = UINT16_MAX;
	}
	
	// initialise the network header
	packet->header.flags = 0;
	packet->header.gso_type = VIRTIO_NET_HDR_GSO_NONE;
	packet->header.hdr_len = sizeof(packet->header);
	packet->header.gso_size = 0;
	packet->header.csum_start = 0;
	packet->header.csum_offset = 0;

	// ensure we can find it again
	queue.packets_for_descs[head_desc] = packet;
	
	// add the chain to the available ring
	queue.avail->ring[at_avail_idx % queue.num] = head_desc;
	++at_avail_idx;
	
	return true;
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
bool eu_philjordan_virtio_net::populateReceiveBuffers()
{
	uint16_t avail_idx = rx_queue.avail->idx;
	bool added = false;
	// with less than 2 descriptors remaining, we definitely won't be able to fit another packet
	while (rx_queue.num_free_desc >= 2)
	{
		// allocate data buffer and header memory
		mbuf_t packet_mbuf = allocatePacket(kIOEthernetMaxPacketSize);
		if (!packet_mbuf)
			return (added && notifyQueueAvailIdx(rx_queue, avail_idx)), false;
		
		{
			size_t len = mbuf_len(packet_mbuf);
			if (len != kIOEthernetMaxPacketSize)
				kprintf("virtio-net populateReceiveBuffers(): unexpected new packet length %lu (wanted: %u)\n",
					len, kIOEthernetMaxPacketSize);
			assert(len == kIOEthernetMaxPacketSize);
		}
		
		// this will increment avail_idx if successful
		if (!addPacketToQueue(packet_mbuf, rx_queue, true /* packet is writeable */, avail_idx))
			return freePacket(packet_mbuf), (added && notifyQueueAvailIdx(rx_queue, avail_idx)), false;
		
		added = true;
	}
	if (added)
		notifyQueueAvailIdx(rx_queue, avail_idx); 
	
	return true;
}

void eu_philjordan_virtio_net::handleReceivedPackets()
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
#warning TODO: defend against out-of-range indices
		virtio_net_packet* packet = rx_queue.packets_for_descs[used_desc];
		
		// work out actual packet length, without the header
		uint32_t len = used.len;
		if (len >= sizeof(virtio_net_hdr))
		{
			len -= sizeof(virtio_net_hdr);
		}
		else
		{
			len = 0;
		}
		
		// this buffer should correspond to a packet we queued; pass it to the network system
		if (packet)
		{
			rx_queue.packets_for_descs[used_desc] = NULL;

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
					mbuf_free(packet->mbuf);
			}
			packet->mbuf = NULL;
			IOBufferMemoryDescriptor* mem = packet->mem;
			if (mem)
				packet_bufdesc_pool->setObject(mem);
			release_obj(mem);
		}
		else
		{
			IOLog("virtio-net handleReceivedPackets(): warning, used receive buffer chain without matching packet reported. Probably leaking memory.\n");
			IOLog("virtio-net: handleReceivedPackets(): rx queue used element: desc %u, length %u\n", used.id, used.len);
			uint16_t desc = used_desc;
			while (true)
			{
#warning TODO: Defend against infinite loop (out of range descriptors, circular lists)
				IOLog("virtio-net: handleReceivedPackets(): used buffer %u: length %u, associated packet: %p\n",
					desc, rx_queue.desc[desc].len, packet);
				if (0 == (rx_queue.desc[desc].flags & VRING_DESC_F_NEXT))
					break;
				desc = rx_queue.desc[desc].next;
			}
		}
		
		// recycle the descriptors
		uint16_t desc = used_desc; 
		while (true)
		{
#warning TODO: Defend against infinite loop (out of range descriptors, circular lists)
			uint16_t next = rx_queue.desc[desc].next;
			bool has_next = (0 != (rx_queue.desc[desc].flags & VRING_DESC_F_NEXT));
			rx_queue.desc[desc].addr = 0;
			rx_queue.desc[desc].len = 0;
			rx_queue.desc[desc].flags = 0;
			rx_queue.desc[desc].next = 0;
			vring_push_free_desc(rx_queue, desc);
			if (!has_next)
				break;
			desc = next;
		}
		++rx_queue.last_used_idx;
	}
	if (packets_submitted > 0)
	{
		interface->flushInputQueue();
		//IOLog("virtio-net handleReceivedPackets(): %u received\n", packets_submitted);
	}
}


const OSString* eu_philjordan_virtio_net::newVendorString() const
{
	return OSString::withCStringNoCopy("Virtio");
}


const OSString* eu_philjordan_virtio_net::newModelString() const
{
	return OSString::withCStringNoCopy("Paravirtual Ethernet Adapter");
}



void eu_philjordan_virtio_net::stop(IOService* provider)
{
	IOLog("virtio-net stop()\n");
	if (provider != this->pci_dev)
		IOLog("Warning: stopping virtio-net with a different provider!?\n");
	
	packet_bufdesc_pool->flushCollection();
	release_obj(interface);

	// reset device to disable virtqueues
	setVirtioDeviceStatus(VIRTIO_PCI_DEVICE_STATUS_RESET);
	
#warning TODO: clear/free buffers in queue descriptors
	
	release_obj(rx_queue.buf);
	memset(&rx_queue, 0, sizeof(rx_queue));
	release_obj(tx_queue.buf);
	memset(&tx_queue, 0, sizeof(tx_queue));
	
	release_obj(this->pci_config_mmap);

	if (this->pci_dev)
		this->pci_dev->close(this);
	this->pci_dev = NULL;
	
	super::stop(provider);
}

void eu_philjordan_virtio_net::free()
{
	IOLog("virtio-net free()\n");
	
	release_obj(packet_bufdesc_pool);
	release_obj(interface);

	virtqueue_free(rx_queue);
	release_obj(tx_queue.buf);
	memset(&tx_queue, 0, sizeof(tx_queue));

	release_obj(this->pci_config_mmap);

	if (intr_event_source)
	{
		work_loop->removeEventSource(intr_event_source);
		release_obj(intr_event_source);
	}
	release_obj(work_loop);

	if (this->pci_dev)
		this->pci_dev->close(this);
	this->pci_dev = NULL;

	super::free();
}


IOReturn eu_philjordan_virtio_net::getHardwareAddress(IOEthernetAddress* addrP)
{
	*addrP = mac_address;
	return kIOReturnSuccess;
}
