//
//  PJCommandGate.h
//  NVMHCI
//
//  Created by Phillip Jordan on 07/04/2013.
//  Copyright (c) 2013 Phil Jordan. All rights reserved.
//

#ifndef __NVMHCI__PJCommandGate__
#define __NVMHCI__PJCommandGate__

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#include <IOKit/IOCommandGate.h>

class PJCommandGate : public IOCommandGate
{
	OSDeclareDefaultStructors(PJCommandGate)
public:
	using IOCommandGate::closeGate;
	using IOCommandGate::openGate;
	using IOCommandGate::tryCloseGate;
    using IOCommandGate::sleepGate;
    using IOCommandGate::wakeupGate;

	static PJCommandGate* commandGate(OSObject *owner, Action action = NULL);
	
	class Lock
	{
	public:
		void release();
		~Lock();

#if __has_feature(cxx_rvalue_references)
		Lock(Lock&& that);
	private:
#else
#endif
		Lock(const Lock&);
	private:
		PJCommandGate* held_gate;
		
		explicit Lock(PJCommandGate* gate);
		
		// disallow:
		Lock();
		Lock& operator=(const Lock&);
		
		friend class PJCommandGate;
	};

#if !__has_feature(cxx_rvalue_references)
	int lock_count;
#endif
	Lock acquireLock();
};


#endif /* defined(__NVMHCI__PJCommandGate__) */
