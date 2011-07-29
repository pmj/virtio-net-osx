/* Copyright 2011 Phil Jordan <phil@philjordan.eu>
 * Dual-licensed under the 3-clause BSD License and the zLib license */

#include "virtio_net.h"
#include <IOKit/pci/IOPCIDevice.h>
#include "virtio_ring.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/task.h>

// darwin doesn't have inttypes.h TODO: build an inttypes.h for the kernel
#ifndef PRIuPTR
#define PRIuPTR "%lu"
#endif
#ifndef PRIX64
#define PRIX64 "%llX"
#endif

OSDefineMetaClassAndStructors(eu_philjordan_virtio_net, IOEthernetController);
#define super IOEthernetController

bool eu_philjordan_virtio_net::init(OSDictionary* properties)
{
	IOLog("virtio-net init()\n");
	bool ok = super::init(properties);
	if (!ok)
		return false;
	
	rx_queue = NULL;
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

static void release_obj(OSObject*& obj)
{
	if (obj)
	{
		obj->release();
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

bool eu_philjordan_virtio_net::start(IOService* provider)
{
	IOLog("virtio-net start(%p)\n", provider);
	IOPCIDevice* pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci)
	{
		if (!provider)
			return false;
		const OSMetaClass* meta = provider->getMetaClass();
		IOLog("virtio-net start(): Provider (%p) has wrong type: %s\n", provider, meta->getClassName());
		return false;
	}
	
	IOLog("virtio-net start(): attempting to map device memory with register 0\n");
	IODeviceMemory* devmem = pci->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!devmem)
	{
		IOLog("virtio-net start(): Getting memory descriptor failed.\n");
		return false;
	}
	IOMemoryMap* iomap = devmem->map();
	if (!iomap)
	{
		IOLog("virtio-net start(): Mapping failed.\n");
		return false;
	}
	IOLog("virtio-net start(): Mapped %llu bytes (descriptor length: %llu)\n",
		static_cast<uint64_t>(iomap->getLength()),
		static_cast<uint64_t>(devmem->getLength()));
	
	IOLog("virtio-net start(): Device Initialisation Sequence\n");
	
	// Reset the device just in case it was previously opened and left in a weird state.
	pci->ioWrite8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, VIRTIO_PCI_DEVICE_STATUS_RESET, iomap);

	IOLog("virtio-net start(): Device reset\n");

	// Acknowledge the device, then tell it we're the driver
	uint8_t status = pci->ioRead8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, iomap);
	pci->ioWrite8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, status | VIRTIO_PCI_DEVICE_STATUS_ACKNOWLEDGE, iomap);
	IOLog("virtio-net start(): Device acknowledged\n");

	status = pci->ioRead8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, iomap);
	pci->ioWrite8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, status | VIRTIO_PCI_DEVICE_STATUS_DRIVER, iomap);
	IOLog("virtio-net start(): Device informed of driver\n");
	
	uint32_t dev_features = OSSwapLittleToHostInt32(pci->ioRead32(VIRTIO_PCI_CONF_OFFSET_DEVICE_FEATURE_BITS_0_31, iomap));
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
	
	/* Read out the flexible config space */
	size_t config_offset = VIRTIO_PCI_CONF_OFFSET_END_HEADER;

#warning TODO: find out how to detect if MSI-X is enabled (I don't think it ever is on Mac OS X)
	// if (msix_enabled) config_offset += 4;
	
	ssize_t hi_features_offset = -1;
	if (dev_features & VIRTIO_F_FEATURES_HIGH)
	{
		// Has extended feature table
		hi_features_offset = config_offset;
		config_offset += 2 * 4;
	}
	
	// offset for the device-specific configuration space
	size_t device_specific_offset = config_offset;
	
	{
		/* virtqueues must be aligned to 4096 bytes (last 12 bits 0) and the shifted
		 * address will be written to a 32-bit register 
		 */
		const mach_vm_address_t VIRTIO_RING_ALLOC_MASK = 0xffffffffull << 12u;
		// detect and allocate virtqueues - expect 2 plus 1 if VIRTIO_NET_F_CTRL_VQ is set
		// queue 0 is receive queue (Appendix C, Configuration)
		// queue 1 is transmit queue
		// queue 2 is control queue (if present)
		pci->ioWrite16(VIRTIO_PCI_CONF_OFFSET_QUEUE_SELECT, 0, iomap);
		uint16_t queue_size = pci->ioRead16(VIRTIO_PCI_CONF_OFFSET_QUEUE_SIZE, iomap);
#warning TODO: check queue size for sanity (>0, pow2, minimum sensible size)
		if (queue_size == 0)
		{
			IOLog("Queue size for queue 0 is 0.\n");
			goto fail;
		}
		IOLog("Reported queue size for rx queue: %u\n", queue_size);
		// allocate an appropriately sized DMA buffer. As per the spec, this must be physically contiguous.
		size_t queue_size_bytes = vring_size(queue_size);
		IOBufferMemoryDescriptor* rx_queue_buffer = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
			kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut, queue_size_bytes, VIRTIO_RING_ALLOC_MASK);
		if (!rx_queue_buffer)
		{
			IOLog("Failed to allocate queue buffer with " PRIuPTR " contiguous bytes and mask " PRIX64 ".\n",
				queue_size_bytes, VIRTIO_RING_ALLOC_MASK);
			goto fail;
		}
		memset(rx_queue_buffer->getBytesNoCopy(), 0, queue_size_bytes);
		pci->ioWrite32(VIRTIO_PCI_CONF_OFFSET_QUEUE_ADDRESS, rx_queue_buffer->getPhysicalAddress() >> 12u);
		
		iomap->release();
		
		IOLog("Initialised virtqueue 0 (receive queue) with " PRIuPTR " bytes at " PRIX64 "\n", queue_size_bytes, rx_queue_buffer->getPhysicalAddress());
		
		this->rx_queue = rx_queue_buffer;
		return true;
	}
fail:
	if (pci && iomap)
	{
		pci->ioWrite8(VIRTIO_PCI_CONF_OFFSET_DEVICE_STATUS, status | VIRTIO_PCI_DEVICE_STATUS_FAILED, iomap);
		iomap->release();
	}
	return false;
}

void eu_philjordan_virtio_net::stop(IOService* provider)
{
	IOLog("virtio-net stop()\n");
	if (this->rx_queue)
	{
		this->rx_queue->release();
		this->rx_queue = NULL;
		IOLog("Released rx queue\n");
	}
}

void eu_philjordan_virtio_net::free()
{
	IOLog("virtio-net free()\n");

	if (this->rx_queue)
	{
		this->rx_queue->release();
		this->rx_queue = NULL;
		IOLog("Released rx queue\n");
	}

	super::free();
}


IOReturn eu_philjordan_virtio_net::getHardwareAddress(IOEthernetAddress* addrP)
{
#warning TODO
	return kIOReturnError;
}
