//
//  SSDCMultiSubrangeMemoryDescriptor.h
//  ssdcache
//
//  Created by Phillip Jordan on 4/26/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//

#ifndef ssdcache_SSDCMultiSubrangeMemoryDescriptor_h
#define ssdcache_SSDCMultiSubrangeMemoryDescriptor_h

#include <IOKit/IOMemoryDescriptor.h>

struct SSDCMemoryDescriptorSubrange
{
	IOMemoryDescriptor* md;
	IOByteCount offset;
	IOByteCount length;
};

#ifndef SSDCMultiSubrangeMemoryDescriptor
#error The SSDCMultiSubrangeMemoryDescriptor class name needs to be #defined to something with a reverse-DNS prefix, e.g. using PJ_PREFIXED_NAME()
#endif

class SSDCMultiSubrangeMemoryDescriptor : public IOMemoryDescriptor
{
	OSDeclareDefaultStructors(SSDCMultiSubrangeMemoryDescriptor);
protected:
	SSDCMemoryDescriptorSubrange* subranges;
	size_t num_subranges;
	bool subranges_allocated;
	bool initialised;

	virtual void free();
public:

	static SSDCMultiSubrangeMemoryDescriptor* withDescriptorRanges(
		SSDCMemoryDescriptorSubrange* descriptor_ranges,
		size_t count,
		IODirection direction,
		bool copy_ranges);

	virtual bool initWithDescriptorRanges(
		SSDCMemoryDescriptorSubrange* descriptor_ranges,
		size_t count,
		IODirection direction,
		bool copy_ranges);

	virtual addr64_t getPhysicalSegment(
		IOByteCount offset, IOByteCount* length, IOOptionBits options = 0);

	virtual IOReturn prepare(IODirection forDirection = kIODirectionNone);

	virtual IOReturn complete(IODirection forDirection = kIODirectionNone);
};

//SSDC_TYPE_SIZE_IS_LESS_OR_EQUAL(SSDCMultiSubrangeMemoryDescriptor, 128);

#endif
