//
//  PJCommandGate.cpp
//  NVMHCI
//
//  Created by Phillip Jordan on 07/04/2013.
//  Copyright (c) 2013 Phil Jordan. All rights reserved.
//

#include "PJCommandGate.h"

OSDefineMetaClassAndStructors(PJCommandGate, IOCommandGate);

PJCommandGate* PJCommandGate::commandGate(OSObject *owner, Action action)
{
	PJCommandGate* gate = new PJCommandGate();
	if (gate)
	{
		if (!gate->init(owner, action))
		{
			gate->release();
			return NULL;
		}
	}
	return gate;
}

PJCommandGate::Lock PJCommandGate::acquireLock()
{
	this->closeGate();
	return Lock(this);
}


PJCommandGate::Lock::Lock(PJCommandGate* gate) :
	held_gate(gate)
{
#if !__has_feature(cxx_rvalue_references)
	if (gate)
		++gate->lock_count;
#endif
}

void PJCommandGate::Lock::release()
{
	if (PJCommandGate* g = this->held_gate)
	{
#if !__has_feature(cxx_rvalue_references)
		int count = --g->lock_count;
#endif
		this->held_gate = NULL;
#if !__has_feature(cxx_rvalue_references)
		if (count == 0)
#endif
			g->openGate();
	}
}

PJCommandGate::Lock::~Lock()
{
	release();
}

#if __has_feature(cxx_rvalue_references)
PJCommandGate::Lock::Lock(Lock&& that) :
	held_gate(that.held_gate)
{
	that.held_gate = NULL;
}
#else
PJCommandGate::Lock::Lock(const Lock& that) :
	held_gate(that.held_gate)
{
	if (held_gate)
		++held_gate->lock_count;
}
#endif
