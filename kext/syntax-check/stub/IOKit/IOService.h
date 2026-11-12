//
// Minimal stand-ins for the kernel headers, only good enough to type-check
// AMDXHCIPreflight.cpp off a Mac. Nothing here runs, and nothing here is
// meant to model IOKit's semantics - it exists so that a typo, a missing
// include or a wrong argument type is caught before the file ever reaches a
// real Xcode build.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

typedef int SInt32;
typedef unsigned char UInt8;
typedef unsigned short UInt16;
typedef unsigned int UInt32;
typedef unsigned long long UInt64;

extern "C" void IOLog(const char* fmt, ...);
extern "C" void IOSleep(unsigned int ms);
extern "C" bool PE_parse_boot_argn(const char* arg_string, void* arg_ptr, int max_arg_size);

class IOMemoryMap
{
public:
	virtual ~IOMemoryMap() {}
	virtual void* getVirtualAddress() { return 0; }
	virtual unsigned long long getLength() { return 0; }
	virtual void release() {}
};

class OSObject
{
public:
	virtual ~OSObject() {}
	virtual void retain() {}
	virtual void release() {}
};

class IORegistryEntry : public OSObject
{
public:
	virtual bool setProperty(const char* aKey, const char* aValue) { return true; }
	virtual bool setProperty(const char* aKey, bool aValue) { return true; }
	virtual bool setProperty(const char* aKey, unsigned long long aValue,
				 unsigned int aNumberOfBits) { return true; }
};

class IOService : public IORegistryEntry
{
public:
	virtual bool start(IOService*) { return true; }
	virtual bool stop(IOService*) { return false; }
	virtual IOService* probe(IOService*, SInt32*) { return 0; }
};

#define OSDeclareDefaultStructors(c)	public: c() {} virtual ~c() {}
#define OSDefineMetaClassAndStructors(c, s)
#define OSDynamicCast(type, inst)	(dynamic_cast<type*>(inst))
