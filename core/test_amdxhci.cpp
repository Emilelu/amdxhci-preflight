//
//  test_amdxhci.cpp
//  Host unit test for AMDXHCIPreflight.h against a simulated AMD xHCI
//  controller. Build & run:  ./build_and_test.sh
//
//  The simulator is deliberately adversarial: it models a controller whose
//  firmware still owns the legacy semaphore, whose CNR drains slowly, whose
//  reset takes time or never completes, whose ports are stuck in Polling, and
//  PCI functions that are not there at all.
//

#include "AMDXHCIPreflight.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace amdxhci;

static int g_fail = 0;
static int g_checks = 0;

static void check(const char* test, const char* what, bool cond)
{
	++g_checks;
	if (!cond) {
		++g_fail;
		std::printf("  FAIL [%s] %s\n", test, what);
	}
}

static void check_eq(const char* test, const char* what, long got, long want)
{
	++g_checks;
	if (got != want) {
		++g_fail;
		std::printf("  FAIL [%s] %s: got %ld want %ld\n", test, what, got, want);
	}
}

// ---------------------------------------------------------------------------
// Simulated controller
// ---------------------------------------------------------------------------
struct Mock
{
	enum Kind {
		kHealthy,
		kPhantomOnes,		// MMIO reads all 1
		kPhantomZeros,		// MMIO reads all 0
		kNoLegacyCap,		// no legacy support capability
		kBiosSticky,		// BIOS_OWNED never clears on its own
		kResetStuck,		// HCRST never self-clears
		kResetSlow,			// HCRST self-clears after reset_ms
		kCnrSlow,			// CNR set for cnr_ms at entry
		kCnrNever,			// CNR never clears at entry
		kHaltStuck			// ignores halt and never finishes the reset
	};

	Kind		kind;
	uint32_t	bar_size;
	uint8_t		ports;
	uint8_t		slots;
	uint16_t	intrs;

	std::vector<uint32_t>	reg;
	std::vector<uint32_t>	port_sc;
	uint8_t			cfg[256];

	uint32_t	now_ms;
	uint32_t	mmio_writes;
	uint32_t	cfg_writes;
	std::string	log;

	bool		halted;
	bool		halt_stuck;		// controller ignores RS being cleared
	bool		reset_pending;
	bool		reset_forever;		// HCRST never self-clears
	uint32_t	reset_at;
	uint32_t	reset_len_ms;
	uint32_t	cnr_until;
	bool		cnr_forever;
	bool		bios_never_releases;
	uint32_t	legs_sup;
	uint32_t	legs_ctl;

	Mock(Kind k = kHealthy, uint8_t nports = 8)
	{
		kind = k;
		bar_size = 0x4000;
		ports = nports;
		slots = 64;
		intrs = 1;
		reg.assign(bar_size / 4, 0);
		port_sc.assign(nports, 0);
		std::memset(cfg, 0, sizeof cfg);
		now_ms = 0;
		mmio_writes = 0;
		cfg_writes = 0;
		halted = false;
		halt_stuck = false;
		reset_pending = false;
		reset_forever = false;
		reset_at = 0;
		reset_len_ms = 20;
		cnr_until = 0;
		cnr_forever = false;
		bios_never_releases = false;
		legs_sup = kLegSupBiosOwned;
		legs_ctl = 0xE0003FFFu;		// every SMI enable set, all events pending
		setup();
	}

	uint32_t& r32(uint32_t off) { return reg[off / 4]; }

	void setup()
	{
		// Capability registers, op base at 0x40.
		r32(0x00) = 0x01100040u;			// HCIVersion 0x110, CapLength 0x40
		r32(0x04) = ((uint32_t)ports << 24) | ((uint32_t)intrs << 8) | slots;
		r32(0x08) = 0x00000400u;			// HCSPARAMS2
		r32(0x0C) = 0x00000000u;
		r32(0x10) = (0x40u << 16) | 0x00000001u;	// XECP -> dword 0x40 (byte 0x100)

		// SuperSpeed protocol cap at 0x100, legacy cap at 0x110.
		if (kind != kNoLegacyCap) {
			r32(0x100) = 0x00000402u;	// id 2, next 0x04
			r32(0x110) = 0x00000001u;	// id 1, next 0
			r32(0x114) = legs_sup;
			r32(0x118) = legs_ctl;
		} else {
			r32(0x100) = 0x00000009u;	// id 9, next 0
		}

		// Operational registers: firmware left the controller running with
		// live ring pointers.
		r32(0x40) = kCmdRunStop | kCmdIntEn | kCmdWrapEvent;	// USBCMD
		r32(0x44) = kStsHsErr | kStsEventInt | kStsPortChange;	// USBSTS
		r32(0x58) = 0xDEADB000u;	// CRCR lo
		r32(0x5C) = 0x00000002u;	// CRCR hi
		r32(0x70) = 0xC0FFEE00u;	// DCBAAP lo
		r32(0x74) = 0x00000001u;	// DCBAAP hi
		r32(0x78) = 0x0000003Fu;	// CONFIG: 63 slots enabled

		for (uint32_t i = 0; i < ports; ++i) {
			uint32_t pls = kLinkU0;
			if (i == 3) pls = kLinkU3;
			if (i == 2 || i == 5) pls = kLinkPolling;
			port_sc[i] = kPortConnStat | kPortEnabled | kPortPower |
				     (pls << 5) | kPortConnChange | kPortLinkStChg;
		}

		if (kind == kCnrSlow) {
			cnr_until = 300;
			r32(0x44) |= kStsNotReady;
		} else if (kind == kCnrNever) {
			cnr_forever = true;
			r32(0x44) |= kStsNotReady;
		} else if (kind == kResetSlow) {
			reset_len_ms = 250;
		} else if (kind == kResetStuck) {
			reset_forever = true;
		} else if (kind == kBiosSticky) {
			bios_never_releases = true;
		} else if (kind == kHaltStuck) {
			halt_stuck = true;
			reset_forever = true;
		}

		// PCI config space: PM at 0x40, MSI at 0x50, MSI-X at 0x60.
		cfg[0x34] = 0x40;
		cfg[0x40] = 0x01; cfg[0x41] = 0x50;		// PM   -> 0x50
		cfg[0x42] = 0x03;					// PMCSR: D3
		cfg[0x50] = 0x05; cfg[0x51] = 0x60;		// MSI  -> 0x60
		cfg[0x52] = 0x01;					// MSI enable set
		cfg[0x60] = 0x11; cfg[0x61] = 0x00;		// MSI-X -> end
		cfg[0x62] = 0x00; cfg[0x63] = 0x80;		// MSI-X enable set
	}

	// -- MMIO -------------------------------------------------------------
	uint32_t read32(uint32_t off, bool* fault)
	{
		if (kind == kPhantomOnes) {
			*fault = true;
			return 0xFFFFFFFFu;
		}
		if (kind == kPhantomZeros)
			return 0u;
		if (off + 4u > bar_size) {
			*fault = true;
			return 0xFFFFFFFFu;
		}
		if (off == 0x40) {			// USBCMD readback
			if (reset_pending && !reset_forever && now_ms >= reset_at) {
				reset_pending = false;
				r32(0x40) &= ~kCmdReset;
				halted = true;
			}
			return r32(0x40);
		}
		if (off == 0x44) {			// USBSTS
			uint32_t v = r32(0x44) & ~(kStsHalted | kStsNotReady);
						if (halted)
				v |= kStsHalted;
			if (cnr_forever || now_ms < cnr_until)
				v |= kStsNotReady;
			return v;
		}
		if (off >= 0x440 && off < 0x440 + (uint32_t)ports * 0x10) {
			uint32_t p = (off - 0x440) / 0x10;
			if ((off - 0x440) % 0x10 != 0) {
				*fault = true;
				return 0xFFFFFFFFu;
			}
			return port_sc[p];
		}
		return r32(off);
	}

	void write32(uint32_t off, uint32_t v)
	{
		++mmio_writes;
		if (off + 4u > bar_size)
			return;
		if (off == 0x40) {			// USBCMD
			uint32_t old = r32(0x40);
			r32(0x40) = v & ~kCmdReset;
			if (!(v & kCmdRunStop) && (old & kCmdRunStop) && !halt_stuck)
				halted = true;
			if (v & kCmdReset) {
				reset_pending = true;
				reset_at = now_ms + reset_len_ms;
				r32(0x40) |= kCmdReset;
			}
			return;
		}
		if (off == 0x44) {			// USBSTS: W1C
			r32(0x44) &= ~(v & (kStsHsErr | kStsEventInt | kStsPortChange | kStsSaveRestErr));
			return;
		}
		if (off == 0x114) {			// USBLEGSUP
			if ((v & kLegSupOsOwned) && !bios_never_releases)
				v &= ~kLegSupBiosOwned;
			legs_sup = v;
			r32(0x114) = v;
			return;
		}
		if (off == 0x118) {
			legs_ctl = v;
			r32(0x118) = v;
			return;
		}
		if (off >= 0x440 && off < 0x440 + (uint32_t)ports * 0x10) {
			uint32_t p = (off - 0x440) / 0x10;
			uint32_t sc = port_sc[p];
			sc &= ~(v & kPortChangeBits);		// RW1C
			if (v & kPortLinkWrStrobe) {
				uint32_t pls = (v >> 5) & 0xFu;
				sc = (sc & ~(0xFu << 5)) | (pls << 5);
				if (pls == kLinkRxDetect) {
					sc &= ~(kPortConnStat | kPortEnabled);
				}
			}
			sc = (sc & ~kPortWriteBack) | (v & kPortWriteBack);
			port_sc[p] = sc;
			return;
		}
		r32(off) = v;
	}

	uint32_t spent_ms() const { return now_ms; }

	void sleep_ms(uint32_t ms) { now_ms += ms; }

	uint8_t cfg_read8(uint8_t off) const { return cfg[off]; }
	uint16_t cfg_read16(uint8_t off) const
	{
		return (uint16_t)(cfg[off] | ((uint16_t)cfg[(uint8_t)(off + 1)] << 8));
	}
	void cfg_write16(uint8_t off, uint16_t v)
	{
		++cfg_writes;
		cfg[off] = (uint8_t)(v & 0xFFu);
		cfg[(uint8_t)(off + 1)] = (uint8_t)(v >> 8);
	}

	// -- thunks -----------------------------------------------------------
	static uint32_t t_read32(void* c, uint32_t off, bool* f)
	{
		return static_cast<Mock*>(c)->read32(off, f);
	}
	static void t_write32(void* c, uint32_t off, uint32_t v)
	{
		static_cast<Mock*>(c)->write32(off, v);
	}
	static uint8_t t_read8(void* c, uint32_t off, bool* f)
	{
		Mock* m = static_cast<Mock*>(c);
		if (m->kind == Mock::kPhantomOnes) {
			*f = true;
			return 0xFFu;
		}
		if (m->kind == Mock::kPhantomZeros)
			return 0u;
		if (off + 1u > m->bar_size) {
			*f = true;
			return 0xFFu;
		}
		return (uint8_t)(m->reg[off / 4] >> ((off % 4) * 8));
	}
	static uint8_t t_cfg8(void* c, uint8_t off) { return static_cast<Mock*>(c)->cfg_read8(off); }
	static uint16_t t_cfg16(void* c, uint8_t off) { return static_cast<Mock*>(c)->cfg_read16(off); }
	static void t_cfgw16(void* c, uint8_t off, uint16_t v)
	{
		static_cast<Mock*>(c)->cfg_write16(off, v);
	}
	static void t_sleep(void* c, uint32_t ms) { static_cast<Mock*>(c)->sleep_ms(ms); }
	static uint32_t t_spent(void* c) { return static_cast<Mock*>(c)->spent_ms(); }
	static void t_log(void* c, const char* msg)
	{
		Mock* m = static_cast<Mock*>(c);
		m->log += msg;
		m->log += "\n";
	}

	Io io()
	{
		Io i;
		i.ctx = this;
		i.read32 = t_read32;
		i.write32 = t_write32;
		i.read8 = t_read8;
		i.cfg_read8 = t_cfg8;
		i.cfg_read16 = t_cfg16;
		i.cfg_write16 = t_cfgw16;
		i.sleep_ms = t_sleep;
		i.spent_ms = t_spent;
		i.log = t_log;
		i.bar_size = bar_size;
		i.vendor = 0x1022;
		i.device = 0x43EE;
		return i;
	}
};

// ---------------------------------------------------------------------------
static Options fast_opts()
{
	Options o;
	Defaults(&o);
	o.log_level = 2;
	return o;
}

static void banner(const char* name) { std::printf("== %s\n", name); }

// ---------------------------------------------------------------------------
int main()
{
	// 1. healthy controller -------------------------------------------------
	{
		banner("healthy");
		Mock m(Mock::kHealthy, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check_eq("healthy", "status", r.status, kStatusOk);
		check("healthy", "halted", (r.flags & kFlagHalted) != 0);
		check("healthy", "reset ok", (r.flags & kFlagResetOk) != 0);
		check("healthy", "rings zeroed", (r.flags & kFlagRingsZeroed) != 0);
		check("healthy", "ports cleared", (r.flags & kFlagPortsCleared) != 0);
		check("healthy", "msi disabled", (r.flags & kFlagMsiOff) != 0);
		check("healthy", "no phantom", (r.flags & kFlagPhantom) == 0);
		check_eq("healthy", "crcr lo", (long)m.r32(0x58), 0);
		check_eq("healthy", "crcr hi", (long)m.r32(0x5C), 0);
		check_eq("healthy", "dcbaap lo", (long)m.r32(0x70), 0);
		check_eq("healthy", "dcbaap hi", (long)m.r32(0x74), 0);
		check_eq("healthy", "config", (long)m.r32(0x78), 0);
		check_eq("healthy", "usbcmd rs clear", (long)(m.r32(0x40) & kCmdRunStop), 0);
		check_eq("healthy", "usbsts change bits", (long)(m.r32(0x44) & (kStsHsErr | kStsEventInt | kStsPortChange)), 0);
		check_eq("healthy", "legs bios owned", (long)(m.legs_sup & kLegSupBiosOwned), 0);
		check_eq("healthy", "legs os owned", (long)(m.legs_sup & kLegSupOsOwned), kLegSupOsOwned);
		check_eq("healthy", "legs smi enables", (long)(m.legs_ctl & kLegCtlSmiEnableBits), 0);
		check_eq("healthy", "legs smi events acked", (long)(m.legs_ctl & kLegCtlSmiEvents), kLegCtlSmiEvents);
		check_eq("healthy", "msi enable", (long)(m.cfg_read16(0x52) & 1), 0);
		check_eq("healthy", "msix enable", (long)(m.cfg_read16(0x62) & 0x8000), 0);
		check_eq("healthy", "rxdetect mask lo", (long)r.rxdetect_mask_lo, (1 << 2) | (1 << 5));
		check("healthy", "rxdetect flag", (r.flags & kFlagRxDetectForced) != 0);
		check_eq("healthy", "port0 kept power", (long)(m.port_sc[0] & kPortPower), kPortPower);
		check_eq("healthy", "port0 kept link state", (long)((m.port_sc[0] >> 5) & 0xF), 0);
		check_eq("healthy", "port2 rxdetect", (long)((m.port_sc[2] >> 5) & 0xF), 5);
		check_eq("healthy", "port2 change bits", (long)(m.port_sc[2] & kPortChangeBits), 0);
		check("healthy", "no timeouts", (r.flags & (kFlagCnrTimeout | kFlagResetTimeout | kFlagHandoffTimeout)) == 0);
		check("healthy", "bounded time", r.total_ms < 120);
		std::printf("  line: %s\n", r.line);
	}

	// 2. phantom PCI function: all ones ------------------------------------
	{
		banner("phantom / all ones");
		Mock m(Mock::kPhantomOnes, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check_eq("phantom-ff", "status", r.status, kStatusBailed);
		check("phantom-ff", "phantom flag", (r.flags & kFlagPhantom) != 0);
		check("phantom-ff", "bailsafe flag", (r.flags & kFlagBailSafe) != 0);
		check_eq("phantom-ff", "no mmio writes", (long)m.mmio_writes, 0);
		check_eq("phantom-ff", "no cfg writes", (long)m.cfg_writes, 0);
		check_eq("phantom-ff", "instant", (long)r.total_ms, 0);
	}

	// 3. phantom PCI function: all zeros -----------------------------------
	{
		banner("phantom / all zeros");
		Mock m(Mock::kPhantomZeros, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check_eq("phantom-00", "status", r.status, kStatusBailed);
		check("phantom-00", "phantom flag", (r.flags & kFlagPhantom) != 0);
		check_eq("phantom-00", "no writes", (long)m.mmio_writes, 0);
	}

	// 4. BAR too small ------------------------------------------------------
	{
		banner("bar too small");
		Mock m(Mock::kHealthy, 8);
		m.bar_size = 0x800;
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check_eq("barsmall", "status", r.status, kStatusBailed);
		check_eq("barsmall", "no writes", (long)m.mmio_writes, 0);
	}

	// 5. no legacy capability ----------------------------------------------
	{
		banner("no legacy cap");
		Mock m(Mock::kNoLegacyCap, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check("nolegacy", "legacy missing flag", (r.flags & kFlagLegacyMissing) != 0);
		check("nolegacy", "still reset", (r.flags & kFlagResetOk) != 0);
		check_eq("nolegacy", "status", r.status, kStatusOk);
	}

	// 6. BIOS refuses to let go --------------------------------------------
	{
		banner("bios sticky");
		Mock m(Mock::kBiosSticky, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check("sticky", "forced flag", (r.flags & kFlagHandoffForced) != 0);
		check("sticky", "no handoff timeout", (r.flags & kFlagHandoffTimeout) == 0);
		check_eq("sticky", "bios owned cleared", (long)(m.legs_sup & kLegSupBiosOwned), 0);
		check_eq("sticky", "os owned set", (long)(m.legs_sup & kLegSupOsOwned), kLegSupOsOwned);
		check_eq("sticky", "smi masked", (long)(m.legs_ctl & kLegCtlSmiEnableBits), 0);
		check_eq("sticky", "status", r.status, kStatusOk);
		check("sticky", "waited about handoff_ms", r.wait_handoff_ms >= 900 && r.wait_handoff_ms <= 1100);
	}

	// 7. reset never completes ---------------------------------------------
	{
		banner("reset stuck");
		Mock m(Mock::kResetStuck, 8);
		Options o = fast_opts();
		o.reset_retries = 2;
		Report r = Run(m.io(), o);
		check("stuck", "reset timeout", (r.flags & kFlagResetTimeout) != 0);
		check_eq("stuck", "attempts", r.reset_attempts, 2);
		check_eq("stuck", "status", r.status, kStatusWarn);
		check("stuck", "halted so rings are safe to zero", (r.flags & kFlagRingsZeroed) != 0);
		check_eq("stuck", "dcbaap zeroed", (long)m.r32(0x70), 0);
		check("stuck", "bounded", r.total_ms <= 2 * 1000 + 1200 + 200);
	}

	// 8. slow reset ---------------------------------------------------------
	{
		banner("reset slow (250ms)");
		Mock m(Mock::kResetSlow, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check("slow", "reset ok", (r.flags & kFlagResetOk) != 0);
		check_eq("slow", "attempts", r.reset_attempts, 1);
		check("slow", "measured wait", r.wait_reset_ms >= 250 && r.wait_reset_ms <= 400);
		check_eq("slow", "status", r.status, kStatusOk);
	}

	// 9. CNR drains slowly --------------------------------------------------
	{
		banner("cnr slow (300ms)");
		Mock m(Mock::kCnrSlow, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check("cnr", "no timeout", (r.flags & kFlagCnrTimeout) == 0);
		check("cnr", "waited", r.wait_cnr_ms >= 280 && r.wait_cnr_ms <= 420);
		check("cnr", "reset still ok", (r.flags & kFlagResetOk) != 0);
	}

	// 10. CNR never clears --------------------------------------------------
	{
		banner("cnr never");
		Mock m(Mock::kCnrNever, 8);
		Options o = fast_opts();
		o.cnr_ms = 100;
		Report r = Run(m.io(), o);
		check("cnrnever", "timeout flag", (r.flags & kFlagCnrTimeout) != 0);
		check_eq("cnrnever", "status", r.status, kStatusWarn);
		check("cnrnever", "still tried reset", (r.flags & kFlagResetOk) != 0);
	}

	// 11. RX detect forced on every port ------------------------------------
	{
		banner("rxdetect all");
		Mock m(Mock::kHealthy, 8);
		Options o = fast_opts();
		o.rxdetect_all = true;
		Report r = Run(m.io(), o);
		check_eq("rxall", "mask", (long)r.rxdetect_mask_lo, 0xFF);
	}

	// 12. knobs off --------------------------------------------------------
	{
		banner("everything off");
		Mock m(Mock::kHealthy, 8);
		Options o = fast_opts();
		o.handoff = false;
		o.reset = false;
		o.ports = false;
		o.msi = false;
		Report r = Run(m.io(), o);
		check("off", "no msi write", (r.flags & kFlagMsiOff) == 0);
		check("off", "no reset", (r.flags & kFlagResetOk) == 0);
		check("off", "still classified", r.num_ports == 8);
		check_eq("off", "biossup untouched", (long)(m.legs_sup & kLegSupBiosOwned), kLegSupBiosOwned);
	}

	// 13. report line is always terminated ---------------------------------
	{
		banner("report line");
		Mock m(Mock::kHealthy, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check("line", "terminated", r.line[sizeof(r.line) - 1] == 0 || std::strlen(r.line) < sizeof(r.line));
		check("line", "non empty", std::strlen(r.line) > 20);
		check("line", "has vendor", std::strstr(r.line, "0x1022") != 0);
	}

	// 14. 32 port controller, port 33 stuck --------------------------------
	{
		banner("32 ports");
		Mock m(Mock::kHealthy, 32);
		m.port_sc[31] = kPortConnStat | kPortPower | (kLinkPolling << 5) | kPortLinkStChg;
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check_eq("p32", "mask lo", (long)r.rxdetect_mask_lo, (1 << 2) | (1 << 5) | (1u << 31));
		check_eq("p32", "mask hi", (long)r.rxdetect_mask_hi, 0);
		check_eq("p32", "port31 rxdetect", (long)((m.port_sc[31] >> 5) & 0xF), 5);
	}

	// 15. neither halt nor reset completes --------------------------------
	{
		banner("halt stuck + reset stuck");
		Mock m(Mock::kHaltStuck, 8);
		Options o = fast_opts();
		Report r = Run(m.io(), o);
		check("nostop", "no halt", (r.flags & kFlagHalted) == 0);
		check("nostop", "no reset", (r.flags & kFlagResetOk) == 0);
		check("nostop", "rings NOT zeroed", (r.flags & kFlagRingsZeroed) == 0);
		check_eq("nostop", "dcbaap untouched", (long)m.r32(0x70), 0xC0FFEE00);
		check_eq("nostop", "config untouched", (long)m.r32(0x78), 0x3F);
		check_eq("nostop", "status", r.status, kStatusWarn);
		std::printf("  line: %s\n", r.line);
	}

	std::printf("\n%d checks, %d failures\n", g_checks, g_fail);
	if (g_fail) {
		std::printf("RESULT: FAIL\n");
		return 1;
	}
	std::printf("RESULT: PASS\n");
	return 0;
}
