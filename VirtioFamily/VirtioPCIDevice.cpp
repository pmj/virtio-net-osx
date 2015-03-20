//
//  VirtioPCIDevice.cpp
//  virtio-osx
//
//  Created by Laura Dennis-Jordan on 17/03/2015.
//
//

#include "VirtioPCIDevice.h"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

OSDefineMetaClassAndStructors(VirtioPCIDevice, IOService);

namespace VirtioCapOffset
{
	enum virtio_pci_cap : uint8_t
	{
		CAP_VNDR = 0,
		CAP_NEXT,
		CAP_LEN,
		CFG_TYPE,
		BAR,
		OFFSET = 8,
		LENGTH = 12
	};
}

IOService* VirtioPCIDevice::probe(IOService* provider, SInt32* score)
{
	IOLog("VirtioPCIDevice::probe()\n");
	
	IOPCIDevice* pci_dev = OSDynamicCast(IOPCIDevice, provider);
	if (!pci_dev)
	{
		IOLog("VirtioPCIDevice::No PCI device found\n");
		return NULL;
	}
	
	/*
	for (unsigned r = 0; r < 16; ++r)
	{
		uint8_t b[16];
		for (unsigned i = 0; i < 16; ++i)
		{
			b[i] = pci_dev->configRead8(r * 16 + i);
		}
		IOLog("%02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x\n",
			b[0], b[1], b[2], b[3],
			b[4], b[5], b[6], b[7],
			b[8], b[9], b[10], b[11],
			b[12], b[13], b[14], b[15]);
	}
	*/
	
	uint32_t pciFoundValue = 0;
	uint8_t offset = 0;
	int numCapabilitiesFound = 0;
	do
	{
		pciFoundValue = pci_dev->findPCICapability(kIOPCIVendorSpecificCapability, &offset);
		if(pciFoundValue != 0)
		{
			IOLog("VirtioLegacyPCIDevice::Capability found at %d", pciFoundValue);
			uint8_t cap_vndr = pci_dev->configRead8(pciFoundValue + VirtioCapOffset::CAP_VNDR);
			uint8_t cap_next = pci_dev->configRead8(pciFoundValue + VirtioCapOffset::CAP_NEXT);
			uint8_t cap_len = pci_dev->configRead8(pciFoundValue + VirtioCapOffset::CAP_LEN);
			uint8_t cfg_type = pci_dev->configRead8(pciFoundValue + VirtioCapOffset::CFG_TYPE);
			uint8_t bar = pci_dev->configRead8(pciFoundValue + VirtioCapOffset::BAR);
			uint32_t offset = pci_dev->configRead32(pciFoundValue + VirtioCapOffset::OFFSET);
			uint32_t length = pci_dev->configRead32(pciFoundValue + VirtioCapOffset::LENGTH);

			IOLog("VirtioLegacyPCIDevice::cap_vndr = %d \ncap_next = %d \n cap_len = %d \n cfg_type = %d \n bar = %d \n offset = %d \n length = %d \n", cap_vndr, cap_next, cap_len, cfg_type,	bar, offset, length);
			numCapabilitiesFound++;
		}
	} while (pciFoundValue != 0);
	IOLog("VirtioPCIDevice::pci Found Value = %d \n num capabilities found = %d\n", pciFoundValue, numCapabilitiesFound);
	if(numCapabilitiesFound == 0)
	{
		//Legacy Device
		return NULL;
	}
	return this;
}

