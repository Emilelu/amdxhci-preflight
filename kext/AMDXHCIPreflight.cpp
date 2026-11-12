/*
 * AMDXHCIPreflight.cpp
 *
 * A driver that owns nothing.
 *
 * On AMD platforms the xHC is handed over by firmware in an undefined state:
 * bus mastering may be off, the controller may be in D3, the legacy BIOS SMI
 * semaphore is very often still set, the command/event rings still point at
 * memory the firmware freed, and the ports may be sitting in Polling or
 * Compliance. AppleUSBXHCIPCI assumes it starts from a quiescent controller
 * and, when the leftovers are bad enough, spins forever waiting for a port
 * reset to complete - the boot hang that has been reported on AMD boards
 * since Sonoma.
 *
 * This kext probes the controller with a very high score, runs a bounded,
 * deterministic clean-up, then deliberately fails its own start() so IOKit
 * detaches it and starts the next candidate, which is AppleUSBXHCIPCI.
 * Returning false from start() withdraws a candidate, and that is exactly the
 * mechanism the GUX fork relies on today.
 *
 * It inherits from IOService and nothing else on purpose: subclassing
 * IOUSBController puts a vtable compiled against 10.x in front of a newer
 * kernel, and every release that grows that vtable turns an unrelated call
 * into a jump through garbage.
 *
 * Build: see build.sh and README.md.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert/boot.h>

#include "AMDXHCIPreflight.h"

#define super IOService

/* Same knobs as the patched fork: a kext has no debugger at boot. */
static void pf_options(amdxhci::Options* opt)
{
	uint32_t v;

	amdxhci::Defaults(opt);
	if (PE_parse_boot_argn("-amdxhci_nohandoff", &v, sizeof v))
		opt->handoff = false;
	if (PE_parse_boot_argn("-amdxhci_noreset", &v, sizeof v))
		opt->reset = false;
	if (PE_parse_boot_argn("-amdxhci_noports", &v, sizeof v))
		opt->ports = false;
	if (PE_parse_boot_argn("-amdxhci_nomsi", &v, sizeof v))
		opt->msi = false;
	if (PE_parse_boot_argn("-amdxhci_rxdetect_all", &v, sizeof v))
		opt->rxdetect_all = true;
	if (PE_parse_boot_argn("amdxhci_cnr_ms", &v, sizeof v) && v)
		opt->cnr_ms = v;
	if (PE_parse_boot_argn("amdxhci_reset_ms", &v, sizeof v) && v)
		opt->reset_ms = v;
	if (PE_parse_boot_argn("amdxhci_handoff_ms", &v, sizeof v) && v)
		opt->handoff_ms = v;
	if (PE_parse_boot_argn("amdxhci_retries", &v, sizeof v) && v && v <= 8U)
		opt->reset_retries = v;
	if (PE_parse_boot_argn("amdxhci_log", &v, sizeof v))
		opt->log_level = static_cast<int>(v);
}

struct PfCtx
{
	IOPCIDevice*		device;
	volatile uint8_t*	base;
	uint32_t		size;
	uint32_t		spent;
};

static uint32_t pf_read32(void* ctx, uint32_t off, bool* fault)
{
	PfCtx* c = static_cast<PfCtx*>(ctx);
	uint32_t v;

	if (off + 4U > c->size) {
		*fault = true;
		return 0xFFFFFFFFU;
	}
	v = *reinterpret_cast<volatile uint32_t const*>(c->base + off);
	if (v == 0xFFFFFFFFU)
		*fault = true;
	return v;
}

static void pf_write32(void* ctx, uint32_t off, uint32_t val)
{
	PfCtx* c = static_cast<PfCtx*>(ctx);
	if (off + 4U <= c->size)
		*reinterpret_cast<volatile uint32_t*>(c->base + off) = val;
}

static uint8_t pf_read8(void* ctx, uint32_t off, bool* fault)
{
	PfCtx* c = static_cast<PfCtx*>(ctx);
	uint8_t v;

	if (off + 1U > c->size) {
		*fault = true;
		return 0xFFU;
	}
	v = *reinterpret_cast<volatile uint8_t const*>(c->base + off);
	if (v == 0xFFU)
		*fault = true;
	return v;
}

static uint8_t pf_cfg8(void* ctx, uint8_t off)
{
	return static_cast<PfCtx*>(ctx)->device->configRead8(off);
}

static uint16_t pf_cfg16(void* ctx, uint8_t off)
{
	return static_cast<PfCtx*>(ctx)->device->configRead16(off);
}

static void pf_cfgw16(void* ctx, uint8_t off, uint16_t val)
{
	static_cast<PfCtx*>(ctx)->device->configWrite16(off, val);
}

static void pf_sleep(void* ctx, uint32_t ms)
{
	PfCtx* c = static_cast<PfCtx*>(ctx);
	c->spent += ms;
	IOSleep(ms);
}

static uint32_t pf_spent(void* ctx)
{
	return static_cast<PfCtx*>(ctx)->spent;
}

static void pf_log(void* ctx, const char* msg)
{
	static_cast<void>(ctx);
	IOLog("AMDXHCIPreflight: %s\n", msg);
}

/*
 * Put the function in D0 and enable memory decoding plus bus mastering.
 * Firmware frequently hands the controller over in D3 or with bus mastering
 * off, and touching the registers in that state is what wedges a boot.
 */
static void pf_pci_prepare(IOPCIDevice* device)
{
	uint16_t cmd = device->configRead16(kIOPCIConfigCommand);
	uint16_t want = static_cast<uint16_t>(cmd | kIOPCICommandMemorySpace | kIOPCICommandBusMaster);

	if (want != cmd)
		device->configWrite16(kIOPCIConfigCommand, want);

	UInt8 pmOff = 0;
	if (device->findPCICapability(1U, &pmOff)) {
		uint16_t pmcsr = device->configRead16(static_cast<uint8_t>(pmOff + 4U));
		if (pmcsr & 3U) {
			device->configWrite16(static_cast<uint8_t>(pmOff + 4U),
					      static_cast<uint16_t>(pmcsr & ~3U));
			IOSleep(10);
		}
	}
}

/* Run the preflight and publish the result. Returns false when skipped. */
static bool pf_run(IOPCIDevice* device, const amdxhci::Options& opt)
{
	IOMemoryMap* map = device->mapDeviceMemoryWithIndex(0U);
	amdxhci::Io io;
	amdxhci::Report report;
	PfCtx ctx;

	if (!map || !map->getVirtualAddress()) {
		if (map)
			map->release();
		device->setProperty("amdxhci-preflight", "no BAR0 mapping");
		return false;
	}

	ctx.device	= device;
	ctx.base	= reinterpret_cast<volatile uint8_t*>(map->getVirtualAddress());
	ctx.size	= static_cast<uint32_t>(map->getLength());
	ctx.spent	= 0;

	io.ctx		= &ctx;
	io.read32	= pf_read32;
	io.write32	= pf_write32;
	io.read8	= pf_read8;
	io.cfg_read8	= pf_cfg8;
	io.cfg_read16	= pf_cfg16;
	io.cfg_write16	= pf_cfgw16;
	io.sleep_ms	= pf_sleep;
	io.spent_ms	= pf_spent;
	io.log		= pf_log;
	io.bar_size	= ctx.size;
	io.vendor	= device->configRead16(kIOPCIConfigVendorID);
	io.device	= device->configRead16(kIOPCIConfigDeviceID);

	device->setProperty("amdxhci-preflight-enabled", true);
	pf_pci_prepare(device);
	report = amdxhci::Run(io, opt);

	device->setProperty("amdxhci-preflight", report.line);
	device->setProperty("amdxhci-status", static_cast<unsigned long long>(report.status), 32U);
	device->setProperty("amdxhci-flags", static_cast<unsigned long long>(report.flags), 32U);
	device->setProperty("amdxhci-ports", static_cast<unsigned long long>(report.num_ports), 32U);
	device->setProperty("amdxhci-legacy-cap", static_cast<unsigned long long>(report.legs_off), 32U);
	device->setProperty("amdxhci-reset-attempts",
			    static_cast<unsigned long long>(report.reset_attempts), 32U);
	device->setProperty("amdxhci-total-ms", static_cast<unsigned long long>(report.total_ms), 32U);
	if (report.flags & amdxhci::kFlagPhantom)
		device->setProperty("amdxhci-phantom", true);

	IOLog("AMDXHCIPreflight: %s\n", report.line);
	map->release();
	return true;
}

class com_yg_AMDXHCIPreflight : public IOService
{
	OSDeclareDefaultStructors(com_yg_AMDXHCIPreflight)

public:
	virtual IOService*	probe(IOService* provider, SInt32* score);
	virtual bool		start(IOService* provider);
};

OSDefineMetaClassAndStructors(com_yg_AMDXHCIPreflight, IOService)

IOService* com_yg_AMDXHCIPreflight::probe(IOService* provider, SInt32* score)
{
	IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
	uint16_t vendor, devid;

	if (!device)
		return 0;
	vendor = device->configRead16(kIOPCIConfigVendorID);
	devid = device->configRead16(kIOPCIConfigDeviceID);

	/*
	 * Never probe a slot that does not answer, and touch no MMIO here: probe
	 * runs before access to the device is serialised, so every hardware
	 * access belongs in start().
	 */
	if (vendor != 0x1022U || devid == 0xFFFFU)
		return 0;

	if (score)
		*score = 10000;
	return this;
}

bool com_yg_AMDXHCIPreflight::start(IOService* provider)
{
	IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
	amdxhci::Options opt;
	uint32_t v;

	if (!device || !super::start(provider))
		return false;

	pf_options(&opt);

	if (PE_parse_boot_argn("-amdxhci_off", &v, sizeof v))
		device->setProperty("amdxhci-preflight", "disabled by boot-arg");
	else
		pf_run(device, opt);

	/*
	 * Withdraw on purpose. IOKit detaches this candidate and starts the next
	 * match, AppleUSBXHCIPCI, which now finds a controller that is in D0,
	 * halted, reset, free of firmware DMA state, with MSI disarmed and with
	 * the legacy semaphore already taken over.
	 */
	return false;
}
