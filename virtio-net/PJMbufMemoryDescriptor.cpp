//
//  PJMbufMemoryDescriptor.cpp
//  virtio-osx
//
//  Created by Phil Jordan on 26/12/2013.
//
/* Copyright 2013 Phil Jordan <phil@philjordan.eu>
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

#include "PJMbufMemoryDescriptor.h"
#include <sys/kpi_mbuf.h>

#define super IOMemoryDescriptor
OSDefineMetaClassAndStructors(PJMbufMemoryDescriptor, IOMemoryDescriptor);

size_t PJMbufMemoryDescriptor::mbufChainLen(mbuf_t mbuf)
{
	size_t size = 0;
	for (mbuf_t cur = mbuf; cur != NULL; cur = mbuf_next(cur))
		size += mbuf_len(cur);
	return size;
}

bool PJMbufMemoryDescriptor::initWithMbuf(mbuf_t mbuf, IODirection direction)
{
	if (!this->first_init_done)
	{
		if (!super::init())
			return false;
		this->first_init_done = true;
		this->prepare_count = 0;
	}
	else
	{
		assert(this->prepare_count == 0);
		if (this->prepare_count != 0)
			return false;
	}
	
	this->_flags = direction;
#ifndef __LP64__
	this->_direction = direction;
#endif
	this->_mappings = NULL;
	this->_tag = 0;
	
	this->mbuf = mbuf;
	if (mbuf)
	{
		this->_length = mbufChainLen(mbuf);
	}
	else
	{
		this->_length = 0;
	}
	this->cur_mbuf = mbuf;
	this->cur_mbuf_begin = 0;

	return true;
}

PJMbufMemoryDescriptor* PJMbufMemoryDescriptor::withMbuf(mbuf_t mbuf, IODirection direction)
{
	PJMbufMemoryDescriptor* me = new PJMbufMemoryDescriptor();
	if (!me)
		return NULL;
	if (!me->initWithMbuf(mbuf, direction))
	{
		me->release();
		return NULL;
	}
	return me;
}

addr64_t PJMbufMemoryDescriptor::getPhysicalSegment(
	IOByteCount offset, IOByteCount* length, IOOptionBits options)
{
	if (!this->mbuf || offset >= this->_length)
	{
		if (length) *length = 0;
		return 0;
	}
	
	if (offset < this->cur_mbuf_begin)
	{
		this->cur_mbuf = this->mbuf;
		this->cur_mbuf_begin = 0;
	}
	
	mbuf_t cur = this->cur_mbuf;
	// Walk through the mbuf chain until we reach the mbuf containing offset
	size_t cur_len = mbuf_len(cur);
	while (offset >= this->cur_mbuf_begin + cur_len)
	{
		this->cur_mbuf_begin += cur_len;
		this->cur_mbuf = cur = mbuf_next(cur);

		/* If this assert fails, there is either a bug in PJMbufMemoryDescriptor, or
		 * the internal structure of this->mbuf has been modified out-of-band. Don't
		 * do that! */
		assert(cur);
		if (!cur)
		{
			if (length) *length = 0;
			this->cur_mbuf = this->mbuf;
			this->cur_mbuf_begin = 0;
			return 0;
		}
		
		cur_len = mbuf_len(cur);
	}
	
	assert(this->cur_mbuf_begin <= offset && offset < this->cur_mbuf_begin + cur_len);
	void* cur_data = mbuf_data(cur);
	
	size_t mbuf_offset = offset - this->cur_mbuf_begin;
		
	char* offset_data = static_cast<char*>(cur_data);
	offset_data += mbuf_offset;
	uintptr_t offset_addr = reinterpret_cast<uintptr_t>(cur_data);
	offset_addr += mbuf_offset;
	
	addr64_t phys = mbuf_data_to_physical(offset_data);
	if (!length)
		return phys;
	
	// the mbuf data may not be physically contiguous, so we'll need to check at every page boundary
	IOByteCount len = 0;
	size_t remain = cur_len - mbuf_offset;
	addr64_t page_phys;
	do
	{
		uintptr_t next_page = trunc_page(offset_addr + PAGE_SIZE);
		size_t next_page_delta = next_page - offset_addr;
		if (remain <= next_page_delta)
		{
			len += remain;
			*length = len;
			return phys;
		}
		offset_data += next_page_delta;
		len += next_page_delta;
		page_phys = mbuf_data_to_physical(offset_data);
		offset_addr += next_page_delta;
		remain -= next_page_delta;
	} while (page_phys == phys + len);

	*length = len;
	return phys;
}

IOReturn PJMbufMemoryDescriptor::prepare(IODirection forDirection)
{
	if (!this->mbuf)
		return kIOReturnInternalError;
	++this->prepare_count;
	return kIOReturnSuccess;
}

IOReturn PJMbufMemoryDescriptor::complete(IODirection forDirection)
{
	if (!this->mbuf)
		return kIOReturnInternalError;
	assert(this->prepare_count > 0);
	--this->prepare_count;
	return kIOReturnSuccess;
}

