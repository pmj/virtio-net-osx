//
//  PJMbufMemoryDescriptor.h
//  virtio-osx
//
//  Created by Phil Jordan on 26/12/2013.
//
//

#ifndef __virtio_osx__PJMbufMemoryDescriptor__
#define __virtio_osx__PJMbufMemoryDescriptor__

#include <IOKit/IOMemoryDescriptor.h>
#include <sys/kernel_types.h>

#ifndef PJMbufMemoryDescriptor
#error The PJMbufMemoryDescriptor class name needs to be #defined to something with a reverse-DNS prefix, e.g. using PJ_PREFIXED_NAME()
#endif

class PJMbufMemoryDescriptor : public IOMemoryDescriptor
{
	OSDeclareDefaultStructors(PJMbufMemoryDescriptor);
protected:
	bool first_init_done;
	
	/// The buffer or buffer chain this memory descriptor represents
	mbuf_t mbuf;

	// Fields used for making iterative getPhysicalSegment() calls O(1) rather than O(N)
	mbuf_t cur_mbuf;
	IOByteCount cur_mbuf_begin;
	
	int prepare_count;
	
	using IOMemoryDescriptor::initWithOptions;
public:
	/// Initialiser. mbuf may be NULL, and this method can safely be called repeatedly
	virtual bool initWithMbuf(mbuf_t mbuf, IODirection direction);
	
	static PJMbufMemoryDescriptor* withMbuf(mbuf_t mbuf, IODirection direction);

	virtual addr64_t getPhysicalSegment(
		IOByteCount offset, IOByteCount* length, IOOptionBits options = 0);

	virtual IOReturn prepare(IODirection forDirection = kIODirectionNone);
	virtual IOReturn complete(IODirection forDirection = kIODirectionNone);

	static size_t mbufChainLen(mbuf_t mbuf);
};

#endif /* defined(__virtio_osx__PJMbufMemoryDescriptor__) */
