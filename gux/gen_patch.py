#!/usr/bin/env python3
"""
Inject the AMD xHCI preflight into a GUX-RyzenXHCIFix checkout.

    python3 gen_patch.py /path/to/GUX-RyzenXHCIFix

Idempotent and strict: every anchor must be found exactly once, so a silent
partial patch is impossible. It

  1. copies core/AMDXHCIPreflight.h next to the other sources
  2. adds the IOKit adapter to GenericUSBXHCI/V1Pure.cpp
  3. turns the plain configWrite16() into a read-modify-write (it used to
     clear bus mastering while the BIOS could still own the controller)
  4. runs the preflight and hands the controller over to AppleUSBXHCIPCI
  5. reverts the b7c0850 "comment out the if" hack, which is dead code now
  6. raises IOProbeScore from 1 to 10000 so the preflight always runs first

Run `git diff` afterwards to produce the patch file.
"""

import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(HERE, "..", "core")

ADAPTER = r'''#pragma mark -
#pragma mark AMD xHCI preflight
#pragma mark -

/*
 * Runs before AppleUSBXHCIPCI can probe the same controller and leaves the
 * hardware halted, reset, free of firmware DMA state and with the legacy
 * support semaphore taken over. See AMDXHCIPreflight.h for the details and
 * README.md for why each step is needed on AMD platforms.
 */
static_assert(__builtin_offsetof(struct XHCICapRegisters, HCCParams) == 0x10U, "HCCParams offset");
static_assert(__builtin_offsetof(struct XHCIOpRegisters, USBSts) == 0x04U, "USBSts offset");
static_assert(__builtin_offsetof(struct XHCIOpRegisters, CRCr) == 0x18U, "CRCr offset");
static_assert(__builtin_offsetof(struct XHCIOpRegisters, DCBAap) == 0x30U, "DCBAap offset");
static_assert(__builtin_offsetof(struct XHCIOpRegisters, Config) == 0x38U, "Config offset");
static_assert(__builtin_offsetof(struct XHCIOpRegisters, prs) == 0x400U, "PortSC base offset");

struct AmdXhciPreflightCtx
{
	IOPCIDevice*		device;
	volatile uint8_t*	base;
	uint32_t		size;
	uint32_t		spent;
};

static uint32_t amdxhci_pf_read32(void* ctx, uint32_t off, bool* fault)
{
	AmdXhciPreflightCtx* c = static_cast<AmdXhciPreflightCtx*>(ctx);
	if (off + 4U > c->size) {
		*fault = true;
		return 0xFFFFFFFFU;
	}
	uint32_t v = *reinterpret_cast<volatile uint32_t const*>(c->base + off);
	if (v == 0xFFFFFFFFU)
		*fault = true;
	return v;
}

static void amdxhci_pf_write32(void* ctx, uint32_t off, uint32_t val)
{
	AmdXhciPreflightCtx* c = static_cast<AmdXhciPreflightCtx*>(ctx);
	if (off + 4U <= c->size)
		*reinterpret_cast<volatile uint32_t*>(c->base + off) = val;
}

static uint8_t amdxhci_pf_read8(void* ctx, uint32_t off, bool* fault)
{
	AmdXhciPreflightCtx* c = static_cast<AmdXhciPreflightCtx*>(ctx);
	if (off + 1U > c->size) {
		*fault = true;
		return 0xFFU;
	}
	uint8_t v = *reinterpret_cast<volatile uint8_t const*>(c->base + off);
	if (v == 0xFFU)
		*fault = true;
	return v;
}

static uint8_t amdxhci_pf_cfg8(void* ctx, uint8_t off)
{
	return static_cast<AmdXhciPreflightCtx*>(ctx)->device->configRead8(off);
}

static uint16_t amdxhci_pf_cfg16(void* ctx, uint8_t off)
{
	return static_cast<AmdXhciPreflightCtx*>(ctx)->device->configRead16(off);
}

static void amdxhci_pf_cfgw16(void* ctx, uint8_t off, uint16_t val)
{
	static_cast<AmdXhciPreflightCtx*>(ctx)->device->configWrite16(off, val);
}

static void amdxhci_pf_sleep(void* ctx, uint32_t ms)
{
	AmdXhciPreflightCtx* c = static_cast<AmdXhciPreflightCtx*>(ctx);
	c->spent += ms;
	IOSleep(ms);
}

static uint32_t amdxhci_pf_spent(void* ctx)
{
	return static_cast<AmdXhciPreflightCtx*>(ctx)->spent;
}

static void amdxhci_pf_log(void* ctx, const char* msg)
{
	static_cast<void>(ctx);
	IOLog("AMDXHCIPreflight: %s\n", msg);
}

static void amdxhci_pf_options(amdxhci::Options* opt)
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
	if (PE_parse_boot_argn("-amdxhci_dryrun", &v, sizeof v)) {
		opt->dryrun = true;
		opt->handoff = opt->reset = opt->ports = opt->msi = false;
	}
	if (PE_parse_boot_argn("-amdxhci_flr", &v, sizeof v))
		opt->flr = true;
	if (PE_parse_boot_argn("-amdxhci_noflr", &v, sizeof v))
		opt->flr = false;
	if (PE_parse_boot_argn("amdxhci_flr_ms", &v, sizeof v) && v)
		opt->flr_ms = v;
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

/*
 * Put the function into D0 and make sure memory decoding and bus mastering are
 * both on. Firmware may hand the controller over in D3 or with bus mastering
 * off, and touching the registers in that state is what wedges a boot.
 */
static void amdxhci_pf_pci_prepare(IOPCIDevice* device)
{
	uint16_t cmd = device->configRead16(kIOPCIConfigCommand);
	uint16_t want = static_cast<uint16_t>(cmd | kIOPCICommandMemorySpace | kIOPCICommandBusMaster);

	if (want != cmd)
		device->configWrite16(kIOPCIConfigCommand, want);

	UInt8 pmOff = 0;
	if (device->findPCICapability(1U, &pmOff) && pmOff >= 0x40U) {
		uint16_t pmcsr = device->configRead16(static_cast<uint8_t>(pmOff + 4U));
		if (pmcsr & 3U) {
			device->configWrite16(static_cast<uint8_t>(pmOff + 4U),
					      static_cast<uint16_t>(pmcsr & ~3U));
			IOSleep(10);
		}
	}
}

static void amdxhci_pf_run(IOPCIDevice* device, IOMemoryMap* map)
{
	AmdXhciPreflightCtx ctx;
	amdxhci::Io io;
	amdxhci::Options opt;
	amdxhci::Report report;
	uint32_t v;

	if (!map || !map->getVirtualAddress()) {
		device->setProperty("amdxhci-preflight", "no BAR0 mapping");
		return;
	}
	ctx.device	= device;
	ctx.base	= reinterpret_cast<volatile uint8_t*>(map->getVirtualAddress());
	ctx.size	= static_cast<uint32_t>(map->getLength());
	ctx.spent	= 0;

	amdxhci_pf_options(&opt);
	if (PE_parse_boot_argn("-amdxhci_off", &v, sizeof v)) {
		device->setProperty("amdxhci-preflight", "disabled by boot-arg");
		return;
	}

	io.ctx		= &ctx;
	io.read32	= amdxhci_pf_read32;
	io.write32	= amdxhci_pf_write32;
	io.read8	= amdxhci_pf_read8;
	io.cfg_read8	= amdxhci_pf_cfg8;
	io.cfg_read16	= amdxhci_pf_cfg16;
	io.cfg_write16	= amdxhci_pf_cfgw16;
	io.sleep_ms	= amdxhci_pf_sleep;
	io.spent_ms	= amdxhci_pf_spent;
	io.log		= amdxhci_pf_log;
	io.bar_size	= ctx.size;
	io.vendor	= device->configRead16(kIOPCIConfigVendorID);
	io.device	= device->configRead16(kIOPCIConfigDeviceID);

	device->setProperty("amdxhci-preflight-enabled", true);
	amdxhci_pf_pci_prepare(device);
	report = amdxhci::Run(io, opt);

	device->setProperty("amdxhci-preflight", report.line);
	device->setProperty("amdxhci-status", static_cast<unsigned long long>(report.status), 32U);
	device->setProperty("amdxhci-flags", static_cast<unsigned long long>(report.flags), 32U);
	device->setProperty("amdxhci-ports", static_cast<unsigned long long>(report.num_ports), 32U);
	device->setProperty("amdxhci-legacy-cap", static_cast<unsigned long long>(report.legs_off), 32U);
	device->setProperty("amdxhci-reset-attempts", static_cast<unsigned long long>(report.reset_attempts), 32U);
	device->setProperty("amdxhci-total-ms", static_cast<unsigned long long>(report.total_ms), 32U);
	if (report.flags & amdxhci::kFlagPhantom)
		device->setProperty("amdxhci-phantom", true);
}
'''

# configWrite16 block  ->  read-modify-write + preflight + hand over
NEW_CMDPTR = r'''	/*
	 * Read-modify-write: the original plain write also cleared bus mastering
	 * while the BIOS could still own the xHC and be DMA'ing into it. On AMD
	 * boards that is a reliable way to get a PCIe UR/AER storm during boot.
	 */
	{
		uint16_t pciCmd = _device->configRead16(kIOPCIConfigCommand);
		uint16_t pciWant = static_cast<uint16_t>(pciCmd | kIOPCICommandMemorySpace |
							 kIOPCICommandBusMaster);
		if (pciWant != pciCmd)
			_device->configWrite16(kIOPCIConfigCommand, pciWant);
	}
	/*
	 * This fork used to bail out much further down with an IOLog and a
	 * UIMFinalize(). All that did to the hardware was map BAR0 and enable
	 * memory decoding, so whether AppleUSBXHCIPCI found the controller in a
	 * usable state depended on firmware leftovers, probe order and whether a
	 * device happened to be plugged in. That is the intermittent boot hang.
	 *
	 * Do the hand-over properly instead: put the function in D0, take the
	 * legacy semaphore, halt, reset, drop every piece of firmware DMA state
	 * and disarm MSI/MSI-X, so the next driver always starts from the same
	 * clean slate. The remainder of this function is never reached.
	 */
	amdxhci_pf_run(_device, _deviceBase);
	UIMFinalize();
	return kIOReturnUnsupported;
'''

OLD_CMDPTR_RE = re.compile(
    r"\t/\*\n"
    r"\t \* TBD: This also disables bus-mastering\.  Bios may still\n"
    r"\t \*   own the xHC\.  Is that a problem\?\n"
    r"\t \*/\n"
    r"\t// enable the card registers\n"
    r"\t_device->configWrite16\(kIOPCIConfigCommand, kIOPCICommandMemorySpace\);\n",
    re.S,
)

# revert b7c0850: give the original upstream logic its braces back
OLD_HACK_RE = re.compile(
    r"\t//if \(!_filterInterruptSource\) \{\n"
    r"(\t\tIOLog\(\"%s: Unable to create filterInterruptEventSource\\n\", __FUNCTION__\);\n"
    r"\t\tUIMFinalize\(\);\n"
    r"\t\treturn kIOReturnNoResources;\n)"
    r"\t//\}\n"
)

INCLUDE_ANCHOR = '#include <IOKit/IOFilterInterruptEventSource.h>\n'
INCLUDE_NEW = ('#include <IOKit/IOFilterInterruptEventSource.h>\n'
               '\n'
               '/* boot-arg parsing moved between SDK releases; both spellings\n'
               ' * and no pexpert prototypes at all (CLT) all happen. */\n'
               '#if defined(__has_include)\n'
               '#  if __has_include(<pexpert/pexpert/boot.h>)\n'
               '#    include <pexpert/pexpert/boot.h>\n'
               '#  elif __has_include(<pexpert/boot.h>)\n'
               '#    include <pexpert/boot.h>\n'
               '#  endif\n'
               '#endif\n'
               'extern "C" int PE_parse_boot_argn(const char * arg_string,\n'
               '\t\t\t\t  void * arg_ptr, int max_arg_size);\n'
               '\n'
               '#include "AMDXHCIPreflight.h"\n')

PRAGMA_ANCHOR = '#pragma mark -\n#pragma mark IOUSBController Pure\n#pragma mark -\n'

PLIST_RE = re.compile(r"(<key>IOProbeScore</key>\s*<integer>)1(</integer>)")


def sub_once(text, pattern, replacement, what, literal=False):
    if literal:
        n = text.count(pattern)
        if n != 1:
            sys.exit("anchor %r found %d times (expected 1)" % (what, n))
        return text.replace(pattern, replacement)
    matches = list(pattern.finditer(text))
    if len(matches) != 1:
        sys.exit("anchor %r matched %d times (expected 1)" % (what, len(matches)))
    return text[:matches[0].start()] + replacement + text[matches[0].end():]


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_patch.py /path/to/GUX-RyzenXHCIFix")
    repo = os.path.abspath(sys.argv[1])
    src = os.path.join(repo, "GenericUSBXHCI", "V1Pure.cpp")
    hdr_src = os.path.join(CORE, "AMDXHCIPreflight.h")
    hdr_dst = os.path.join(repo, "GenericUSBXHCI", "AMDXHCIPreflight.h")
    plist = os.path.join(repo, "GenericUSBXHCI", "GenericUSBXHCI-Info.plist")

    for p in (src, hdr_src, plist):
        if not os.path.exists(p):
            sys.exit("missing: " + p)

    shutil.copyfile(hdr_src, hdr_dst)
    print("copied  GenericUSBXHCI/AMDXHCIPreflight.h")

    with open(src, "r", encoding="utf-8", errors="surrogateescape") as f:
        text = f.read()
    if "AMDXHCIPreflight.h" in text:
        sys.exit("V1Pure.cpp already patched, aborting")

    text = sub_once(text, INCLUDE_ANCHOR, INCLUDE_NEW, "include anchor", literal=True)
    text = sub_once(text, PRAGMA_ANCHOR, ADAPTER + "\n" + PRAGMA_ANCHOR, "pragma mark anchor",
                    literal=True)
    text = sub_once(text, OLD_CMDPTR_RE, NEW_CMDPTR, "configWrite16 block")

    # Revert b7c0850: give the original upstream logic its braces back, so the
    # file does not carry a commented-out "if" that only exists to force an exit.
    m = list(OLD_HACK_RE.finditer(text))
    if len(m) != 1:
        sys.exit("anchor 'b7c0850 hack' matched %d times (expected 1)" % len(m))
    body = m[0].group(1)
    text = (text[:m[0].start()] + "\tif (!_filterInterruptSource) {\n" + body + "\t}\n" +
            text[m[0].end():])

    with open(src, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(text)
    print("patched GenericUSBXHCI/V1Pure.cpp")

    with open(plist, "r", encoding="utf-8", errors="surrogateescape") as f:
        p = f.read()
    p, n = PLIST_RE.subn(r"\g<1>10000\g<2>", p, count=1)
    if n != 1:
        sys.exit("IOProbeScore anchor not found in Info.plist")
    with open(plist, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(p)
    print("patched GenericUSBXHCI/GenericUSBXHCI-Info.plist (IOProbeScore 1 -> 10000)")


if __name__ == "__main__":
    main()
