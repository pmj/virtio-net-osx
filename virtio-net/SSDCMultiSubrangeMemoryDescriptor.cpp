//
//  SSDCMultiSubrangeMemoryDescriptor.cpp
//  ssdcache
//
//  Created by Phillip Jordan on 4/26/12.
//  Copyright (c) 2012 __MyCompanyName__. All rights reserved.
//
/* Copyright 2012-2013 Phil Jordan <phil@philjordan.eu>
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

#include "SSDCMultiSubrangeMemoryDescriptor.h"
//#include "../shared/mem.h"

#ifndef ssdc_assert
#define ssdc_assert assert
#endif

#ifndef ssdc_zmalloc_array_block
#include <IOKit/IOLib.h>
#define ssdc_zmalloc_array_block(type, count) static_cast<type*>(IOMalloc(sizeof(type) * (count)))
#define ssdc_free_array(ptr, count) IOFree((ptr), sizeof((ptr)[0]) * (count))
#endif

#define super IOMemoryDescriptor

OSDefineMetaClassAndStructors(SSDCMultiSubrangeMemoryDescriptor, IOMemoryDescriptor);

SSDCMultiSubrangeMemoryDescriptor* SSDCMultiSubrangeMemoryDescriptor::withDescriptorRanges(
	SSDCMemoryDescriptorSubrange* descriptor_ranges,
	size_t count,
	IODirection direction,
	bool copy_ranges)
{
	SSDCMultiSubrangeMemoryDescriptor* desc = new SSDCMultiSubrangeMemoryDescriptor();
	if (desc && !desc->initWithDescriptorRanges(descriptor_ranges, count, direction, copy_ranges))
	{
		desc->release();
		desc = NULL;
	}
	return desc;
}

bool SSDCMultiSubrangeMemoryDescriptor::initWithDescriptorRanges(
	SSDCMemoryDescriptorSubrange* descriptor_ranges,
	size_t count,
	IODirection direction,
	bool copy_ranges)
{
	if (subranges)
	{
		ssdc_assert(initialised);
		for (size_t i = 0; i < num_subranges; ++i)
			subranges[i].md->release();
			
		if (subranges_allocated)
			ssdc_free_array(subranges, num_subranges);
	}
	else if (!initialised)
	{
		if (!super::init())
			return false;
		initialised = true;
	}
	
	_flags = direction;
#ifndef __LP64__
	_direction = direction;
#endif
	_length = 0;
	_mappings = 0;
	_tag = 0;
	
	subranges_allocated = false;
	if (count == 0)
	{
		subranges = NULL;
		num_subranges = 0;
		return true;
	}
	
	num_subranges = count;
	subranges_allocated = copy_ranges;
	if (copy_ranges)
	{
		subranges = ssdc_zmalloc_array_block(SSDCMemoryDescriptorSubrange, count);
		if (!subranges)
			return false;
		for (size_t i = 0; i < count; ++i)
			subranges[i] = descriptor_ranges[i];
	}
	else
	{
		subranges = descriptor_ranges;
	}
	
	for (size_t i = 0; i < num_subranges; ++i)
	{
		subranges[i].md->retain();
		_length += subranges[i].length;
		if (!_tag) _tag = subranges[i].md->getTag();
		ssdc_assert(subranges[i].md->getDirection() == direction);
	}
	
	return true;
}

void SSDCMultiSubrangeMemoryDescriptor::free()
{
	if (subranges)
	{
		ssdc_assert(initialised);
		for (size_t i = 0; i < num_subranges; ++i)
			subranges[i].md->release();
			
		if (subranges_allocated)
			ssdc_free_array(subranges, num_subranges);
	}
	super::free();
}

addr64_t SSDCMultiSubrangeMemoryDescriptor::getPhysicalSegment(
	IOByteCount offset, IOByteCount* length, IOOptionBits options)
{
	ssdc_assert(offset <= _length);
	for (size_t i = 0; i < num_subranges; ++i)
	{
		IOByteCount len = subranges[i].length;
		if (offset < len)
		{
			IOByteCount phys_len = 0;
			addr64_t addr = subranges[i].md->getPhysicalSegment(offset + subranges[i].offset, &phys_len, options);
			if (phys_len > len)
				phys_len = len;
			if (length)
				*length = phys_len;
			return addr;
		}
		offset -= len;
	}
	if (length)
		*length = 0;
	return 0;
}

IOReturn SSDCMultiSubrangeMemoryDescriptor::prepare(IODirection forDirection)
{
	if (forDirection == kIODirectionNone)
	{
		forDirection = getDirection();
	}

	IOReturn status = kIOReturnSuccess;
	size_t i;
	for (i = 0; i < num_subranges; ++i) 
	{
		status = subranges[i].md->prepare(forDirection);
		if (status != kIOReturnSuccess)
			break;
	}

	if (status != kIOReturnSuccess)
	{
		/* Undo the prepare on the members that have already succeeded */
		for (size_t undo = 0; undo < i; ++undo)
		{
			IOReturn undo_status = subranges[undo].md->complete(forDirection);
			ssdc_assert(undo_status == kIOReturnSuccess);
		}
	}

	return status;
}

IOReturn SSDCMultiSubrangeMemoryDescriptor::complete(IODirection forDirection)
{
	IOReturn final_status = kIOReturnSuccess;

	if (forDirection == kIODirectionNone)
	{
		forDirection = getDirection();
	}

	for (size_t i = 0; i < num_subranges; ++i) 
	{
		IOReturn status = subranges[i].md->complete(forDirection);
		if (status != kIOReturnSuccess)
			final_status = status;
	}

	return final_status;
}

