#pragma once
#include "../IOService.h"

enum
{
	kIOPCIConfigVendorID		= 0x00,
	kIOPCIConfigDeviceID		= 0x02,
	kIOPCIConfigCommand		= 0x04,
	kIOPCICommandMemorySpace	= 0x0002,
	kIOPCICommandBusMaster		= 0x0004
};

class IOPCIDevice : public IOService
{
public:
	virtual UInt8 configRead8(UInt8 where) { return 0; }
	virtual UInt16 configRead16(UInt8 where) { return 0; }
	virtual void configWrite16(UInt8 where, UInt16 data) {}
	virtual IOMemoryMap* mapDeviceMemoryWithIndex(unsigned int index) { return 0; }
	virtual bool findPCICapability(UInt8 capabilityID, UInt8* offset) { return false; }
};
