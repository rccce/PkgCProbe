/*
 * PkgCProbe.kext
 *
 * One-shot diagnostic for "the CPU package never goes deeper than PC3" on an
 * Intel 100/200-series chipset (Sunrise Point) with a Skylake/Kaby Lake CPU.
 * It attaches to the PMC (00:1f.2, 8086:a121) next to Apple's AppleIntelPCHPMC.
 *
 * At start it only reads:
 *   - PWRMBASE (PMC config space 0x48; 0xFE000000 is tried if that does not look right),
 *     the PMC LTR ignore register (PWRMBASE+0x30C),
 *     the latency tolerance (LTR) each chipset block reports (PWRMBASE+0x360..0x3A4)
 *     and the current platform and aggregated LTR (PWRMBASE+0x350 and 0x354).
 *     Offsets as in Linux drivers/platform/x86/intel/pmc (SPT_PMC_*); the ignore bit of
 *     each block as in Apple's pmtelemetry ("... Ign" channels).
 *   - MSR 0xE2 (package C-state limit) and 0x60A-0x60C (package C3/C6/C7 latency limits).
 *
 * Optional ApplyMask (default 0): after the test, or at once if RunTest is false, keep
 * these ignore bits set and set them again after every wake.
 *
 * Results: ioreg property "Results" and kernel log lines starting with "PkgCProbe:".
 *
 * r2: PMC registers mapped with IOMemoryDescriptor::map(); AHCI found among the PMC's siblings.
 * r3/r4: DMI (both ends) and the parked PEG0 port were read and tested; both ruled out.
 *
 * r5: (1) The PEG0 and DMI parts are removed.
 * (2) Read-only survey before the test: PCH IP power gating (PPFEAR, PWRMBASE+0x590..0x594,
 *     names as in Linux spt_pfear_map; bit set = gated), every PCH root port and the device
 *     behind it (ASPM, clock PM, LTR enable, L1 substates), the port status of each xHCI
 *     controller (connected ports, speed, link state), the HD Audio streams that are running,
 *     the RTL8111's own ASPM and clock request enables (Config5 bit 0, Config2 bit 7), and
 *     more AHCI port registers (PxIE, PxIS, PxSERR, PxCI, PxSACT).
 * (3) New test phases with SATA aggressive link power management (ALPM). The AHCI driver in
 *     this EFI (CtlnaAHCIPort 341.0.2) contains Apple's own ALPM code: Enable sets PxCMD.ALPE
 *     (the controller puts the idle link into Partial by itself), and while it is on, the
 *     driver's 80 ms inactivity timer moves the idle link on to Slumber. Nothing on this
 *     system calls it. r5 calls the driver's own EnableAggressiveLinkPowerManagement and
 *     DisableAggressiveLinkPowerManagement (vtable 0x878 and 0x880) on the driver's own work
 *     loop, only if the port object is CtlnaIntelPchSeriesAHCIPort, the four ALPM functions
 *     are byte for byte the ones that were checked, the controller supports ALPM, Partial and
 *     Slumber, the port is running with a device, and PhyRdy-change interrupts are off (no hot
 *     plug; then the driver ignores PhyRdy changes). For the Slumber phases it asks once for
 *     Slumber in the same way the driver's timer does (PxCMD.ICC). After the test ALPM is set
 *     back to how it was, the link is woken up, and the PhyRdy-change and Comm-Wake bits this
 *     caused are cleared from PxSERR; also if the Mac sleeps during the test.
 * (4) Optional SataAlpm (default false): keep ALPM on through the driver after the test (or
 *     StartDelay seconds after start if RunTest is false) and 5 s after every wake.
 */

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <string.h>
#include <stdarg.h>

namespace {

constexpr UInt64 kNone = ~0ULL;            // "no latency requirement"

// PMC registers (Sunrise Point)
constexpr UInt8  kPwrmBaseConfig = 0x48;
constexpr UInt32 kPwrmDefault    = 0xFE000000;   // Intel reference PWRMBASE, tried if config 0x48 does not work
constexpr UInt32 kPwrmMapLength  = 0x1000;
constexpr UInt32 kLtrIgnore      = 0x30C;
constexpr UInt32 kLtrPlatform    = 0x350;
constexpr UInt32 kLtrAggregated  = 0x354;
constexpr UInt32 kPpfear         = 0x590;        // r5: 5 bytes, one bit per IP, 1 = power gated
constexpr unsigned kPpfearBytes  = 5;

// PCI
constexpr UInt8  kPciClassConfig = 0x08;          // bits 31:8 class code, 7:0 revision
constexpr UInt32 kAhciClass      = 0x010601;      // mass storage, SATA, AHCI
constexpr UInt32 kXhciClass      = 0x0C0330;      // USB xHCI
constexpr UInt32 kBridgeClass    = 0x060400;      // PCI-to-PCI bridge (root ports)
constexpr UInt32 kHdaClassPrefix = 0x0403;        // multimedia, HD Audio
constexpr UInt8  kAhciBarConfig  = 0x24;          // BAR5 (ABAR)
constexpr UInt8  kBar0Config     = 0x10;
constexpr UInt8  kBar2Config     = 0x18;          // RTL8111 memory registers
constexpr UInt32 kRealtekId      = 0x816810EC;    // 10ec:8168
constexpr UInt8  kHostPciexbar   = 0x60;          // host bridge: PCIe configuration window (ECAM) base
constexpr UInt32 kMapLength      = 0x1000;

// AHCI port registers (offsets from the port base) and bits
constexpr UInt32 kPxIS    = 0x10;
constexpr UInt32 kPxIE    = 0x14;
constexpr UInt32 kPxCMD   = 0x18;
constexpr UInt32 kPxSSTS  = 0x28;
constexpr UInt32 kPxSCTL  = 0x2C;
constexpr UInt32 kPxSERR  = 0x30;
constexpr UInt32 kPxSACT  = 0x34;
constexpr UInt32 kPxCI    = 0x38;
constexpr UInt32 kPxDEVSLP = 0x44;
constexpr UInt32 kCmdStart  = 1u << 0;
constexpr UInt32 kCmdAlpe   = 1u << 26;
constexpr UInt32 kCmdIccMask = 0xF0000000u;
constexpr UInt32 kCmdIccActive  = 0x10000000u;
constexpr UInt32 kCmdIccSlumber = 0x60000000u;
constexpr UInt32 kIePhyRdy  = 1u << 22;          // PRCE
constexpr UInt32 kSerrPhyRdyWake = (1u << 16) | (1u << 18);   // DIAG.N (PhyRdy change), DIAG.W (Comm Wake)

struct LtrBlock {
    const char *name;
    UInt32 offset;
    UInt32 ignoreBit;
};

const LtrBlock kLtr[] = {
    {"SPA",  0x360,  0}, {"SPB",  0x364,  1}, {"SATA", 0x368,  2}, {"GBE",  0x36C,  3},
    {"XHCI", 0x370,  4}, {"ME",   0x378,  6}, {"EVA",  0x37C,  7}, {"SPC",  0x380,  8},
    {"HDA",  0x384,  9}, {"LPSS", 0x38C, 11}, {"CAM",  0x390, 14}, {"SPD",  0x394, 12},
    {"SPE",  0x398, 13}, {"ESPI", 0x39C, 15}, {"SCC",  0x3A0, 16}, {"ISH",  0x3A4, 17},
};
constexpr unsigned kLtrCount = sizeof(kLtr) / sizeof(kLtr[0]);
const unsigned kShown[] = {2, 4, 5, 8};      // SATA, XHCI, ME, HDA

constexpr UInt32 kIgnoreSATA = 1u << 2;
constexpr UInt32 kIgnoreAll  = 0x0003FBDF;   // bits 0-4, 6-9 and 11-17; 5 and 10 are reserved

// PPFEAR bit names (Linux spt_pfear_map); index = byte * 8 + bit
const char *const kPfearNames[kPpfearBytes * 8] = {
    "PMC", "OPI-DMI", "SPI", "XHCI", "SPA", "SPB", "SPC", "GBE",
    "SATA", "HDA-PGD0", "HDA-PGD1", "HDA-PGD2", "HDA-PGD3", nullptr, "LPSS", "LPC",
    "SMB", "ISH", "P2SB", "DFX", "SCC", nullptr, "FUSE", "CAMERA",
    nullptr, "USB3-OTG", "EXI", "CSE", "CSME_KVM", "CSME_PMT", "CSME_CLINK", "CSME_PTIO",
    "CSME_USBR", "CSME_SUSRAM", "CSME_SMT", nullptr, "CSME_SMS2", "CSME_SMS1", "CSME_RTC", "CSME_PSF",
};
// The IPs whose gating is counted in every phase
const unsigned kGatedShown[] = {1, 3, 4, 5, 6, 8, 9};      // OPI-DMI, XHCI, SPA, SPB, SPC, SATA, HDA-PGD0
constexpr unsigned kGatedCount = sizeof(kGatedShown) / sizeof(kGatedShown[0]);

// r5: what a phase does to the SATA link besides the ignore register
enum SataAction : UInt32 {
    kSataAsFound = 0,           // ALPM as the AHCI driver had it before the test
    kSataAlpm    = 1u << 0,     // the driver's EnableAggressiveLinkPowerManagement
    kSataSlumber = 1u << 1,     // and ask once for Slumber, as the driver's inactivity timer does
};

struct Phase {
    UInt32 mask;
    UInt32 sata;
    const char *name;
};

const Phase kPhases[] = {
    {kIgnoreSATA, kSataAsFound,             "ignore SATA, ALPM as found (daily setting)"},
    {kIgnoreSATA, kSataAlpm,                "ignore SATA + SATA ALPM"},
    {kIgnoreSATA, kSataAlpm | kSataSlumber, "ignore SATA + SATA ALPM + Slumber"},
    {0,           kSataAlpm | kSataSlumber, "nothing ignored + SATA ALPM + Slumber"},
    {kIgnoreAll,  kSataAlpm | kSataSlumber, "ignore all + SATA ALPM + Slumber"},
    {kIgnoreSATA, kSataAsFound,             "ignore SATA, ALPM as found again"},
};
constexpr unsigned kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

constexpr UInt32 kTickMS       = 500;
constexpr UInt32 kSettleTicks  = 4;          // 2 s between changing the settings and measuring
constexpr UInt32 kWakeApplyMS  = 1000;
constexpr UInt32 kWakeAlpmMS   = 5000;
constexpr UInt32 kAlpmRetryMS  = 10000;
constexpr UInt32 kMaxApplyLogs = 3;

// r5: Apple's ALPM functions in CtlnaAHCIPort 341.0.2 (com.apple.driver.CtlnaAHCIPort), as
// vtable slots of CtlnaAHCIPort / CtlnaIntelPchSeriesAHCIPort (byte offset / 8), and their code.
// The code uses only the object (flags at +0x169, port registers at +0x138, controller
// registers at +0x140) and no addresses that the linker changes.
constexpr const char *kAhciPortClass = "CtlnaIntelPchSeriesAHCIPort";
constexpr unsigned kSlotAlpmSupport = 0x868 / 8;
constexpr unsigned kSlotAlpmState   = 0x870 / 8;
constexpr unsigned kSlotAlpmEnable  = 0x878 / 8;
constexpr unsigned kSlotAlpmDisable = 0x880 / 8;

const UInt8 kCodeAlpmSupport[] = {   // ReportAggressiveLinkPowerManagementSupport
    0x55, 0x48, 0x89, 0xe5, 0x8b, 0x8f, 0x69, 0x01, 0x00, 0x00, 0x31, 0xc0, 0xf6, 0xc1, 0x01, 0x74,
    0x1a, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x0f, 0xba, 0xe1, 0x10, 0x72, 0x0f, 0x48, 0x8b, 0x87, 0x40,
    0x01, 0x00, 0x00, 0x8b, 0x00, 0xc1, 0xe8, 0x0d, 0x83, 0xe0, 0x03, 0x5d, 0xc3,
};
const UInt8 kCodeAlpmState[] = {     // ReportAggressiveLinkPowerManagementState
    0x55, 0x48, 0x89, 0xe5, 0xf6, 0x87, 0x6b, 0x01, 0x00, 0x00, 0x01, 0x75, 0x2a, 0x48, 0x8b, 0x87,
    0x38, 0x01, 0x00, 0x00, 0xb9, 0x00, 0x0f, 0x00, 0x00, 0x23, 0x48, 0x28, 0x31, 0xd2, 0x81, 0xf9,
    0x00, 0x02, 0x00, 0x00, 0x0f, 0x94, 0xc2, 0x81, 0xf9, 0x00, 0x06, 0x00, 0x00, 0xb8, 0x02, 0x00,
    0x00, 0x00, 0x0f, 0x45, 0xc2, 0x5d, 0xc3, 0x31, 0xc0, 0x5d, 0xc3,
};
const UInt8 kCodeAlpmEnable[] = {    // EnableAggressiveLinkPowerManagement
    0x55, 0x48, 0x89, 0xe5, 0x8b, 0x8f, 0x69, 0x01, 0x00, 0x00, 0xb8, 0xc7, 0x02, 0x00, 0xe0, 0xf6,
    0xc1, 0x01, 0x74, 0x4c, 0xb8, 0xff, 0xf0, 0xff, 0xff, 0x0f, 0xba, 0xe1, 0x10, 0x72, 0x20, 0x48,
    0x8b, 0x8f, 0x38, 0x01, 0x00, 0x00, 0xba, 0x00, 0x00, 0x00, 0x04, 0x0b, 0x51, 0x18, 0xb8, 0xff,
    0xf0, 0xff, 0xff, 0x23, 0x41, 0x2c, 0x89, 0x51, 0x18, 0x8b, 0x8f, 0x69, 0x01, 0x00, 0x00, 0x0f,
    0xba, 0xe1, 0x10, 0x72, 0x10, 0x48, 0x8b, 0x8f, 0x38, 0x01, 0x00, 0x00, 0x89, 0x41, 0x2c, 0x8b,
    0x8f, 0x69, 0x01, 0x00, 0x00, 0x83, 0xc9, 0x02, 0x89, 0x8f, 0x69, 0x01, 0x00, 0x00, 0x31, 0xc0,
    0x5d, 0xc3,
};
const UInt8 kCodeAlpmDisable[] = {   // DisableAggressiveLinkPowerManagement
    0x55, 0x48, 0x89, 0xe5, 0x8b, 0x8f, 0x69, 0x01, 0x00, 0x00, 0xb8, 0xc7, 0x02, 0x00, 0xe0, 0xf6,
    0xc1, 0x01, 0x74, 0x25, 0x0f, 0xba, 0xe1, 0x10, 0x72, 0x14, 0x48, 0x8b, 0x87, 0x38, 0x01, 0x00,
    0x00, 0x81, 0x60, 0x18, 0xff, 0xff, 0xff, 0xfb, 0x8b, 0x8f, 0x69, 0x01, 0x00, 0x00, 0x83, 0xe1,
    0xfd, 0x89, 0x8f, 0x69, 0x01, 0x00, 0x00, 0x31, 0xc0, 0x5d, 0xc3,
};

// Results of the Slumber request
enum SlumberResult : UInt32 {
    kSlumberReached = 0, kSlumberBusy = 1, kSlumberNotActive = 2, kSlumberTimeout = 3, kSlumberAlready = 4,
};

// MSRs used; all exist on Skylake/Kaby Lake client CPUs (family 6, models 4E/5E/8E/9E).
constexpr UInt32 kMsrPkgCstConfig = 0xE2;
constexpr UInt32 kMsrPkgC3Res     = 0x3F8;
constexpr UInt32 kMsrPkgC6Res     = 0x3F9;
constexpr UInt32 kMsrPkgC7Res     = 0x3FA;
constexpr UInt32 kMsrRaplUnit     = 0x606;
constexpr UInt32 kMsrPkgC3Irtl    = 0x60A;
constexpr UInt32 kMsrPkgC6Irtl    = 0x60B;
constexpr UInt32 kMsrPkgC7Irtl    = 0x60C;
constexpr UInt32 kMsrPkgC2Res     = 0x60D;
constexpr UInt32 kMsrPkgEnergy    = 0x611;
constexpr UInt32 kMsrDramEnergy   = 0x619;
constexpr UInt32 kMsrPkgC8Res     = 0x630;
constexpr UInt32 kMsrPkgC9Res     = 0x631;
constexpr UInt32 kMsrPkgC10Res    = 0x632;
constexpr UInt32 kMsrPkgAnyCoreC0 = 0x659;

inline UInt64 pcpReadMsr(UInt32 msr)
{
    UInt32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<UInt64>(hi) << 32) | lo;
}

inline UInt64 pcpReadTsc()
{
    UInt32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<UInt64>(hi) << 32) | lo;
}

inline void pcpCpuid(UInt32 leaf, UInt32 regs[4])
{
    __asm__ volatile("cpuid" : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3]) : "a"(leaf), "c"(0));
}

UInt64 uptimeNS()
{
    uint64_t abstime = 0;
    uint64_t ns = 0;
    clock_get_uptime(&abstime);
    absolutetime_to_nanoseconds(abstime, &ns);
    return ns;
}

const UInt64 kTimeScale[8] = {1, 32, 1024, 32768, 1048576, 33554432, 0, 0};

// One 16-bit LTR field: bit 15 = requirement present, bits 12:10 = scale, bits 9:0 = value.
UInt64 ltrFieldNS(UInt32 field)
{
    if ((field & 0x8000) == 0)
        return kNone;
    return static_cast<UInt64>(field & 0x3FF) * kTimeScale[(field >> 10) & 7];
}

// The shorter of the snoop (bits 15:0) and no-snoop (bits 31:16) requirements.
UInt64 ltrNS(UInt32 raw)
{
    UInt64 snoop = ltrFieldNS(raw & 0xFFFF);
    UInt64 noSnoop = ltrFieldNS(raw >> 16);
    return snoop < noSnoop ? snoop : noSnoop;
}

// MSR_PKGCx_IRTL: bit 15 = valid, bits 12:10 = time unit, bits 9:0 = value.
UInt64 irtlNS(UInt64 msr)
{
    if ((msr & 0x8000) == 0)
        return kNone;
    return (msr & 0x3FF) * kTimeScale[(msr >> 10) & 7];
}

const char *limitName(UInt32 limit)
{
    static const char *const names[] = {"PC0/PC1", "PC2", "PC3", "PC6", "PC7", "PC7s", "PC8", "PC9", "PC10"};
    return limit < sizeof(names) / sizeof(names[0]) ? names[limit] : "other";
}

const char *linkStateName(UInt32 ipm)
{
    switch (ipm) {
    case 0: return "no device";
    case 1: return "active";
    case 2: return "partial";
    case 6: return "slumber";
    case 8: return "devsleep";
    default: return "other";
    }
}

const char *slumberName(UInt32 result)
{
    switch (result) {
    case kSlumberReached:   return "reached";
    case kSlumberBusy:      return "not asked: commands outstanding";
    case kSlumberNotActive: return "not asked: link not active";
    case kSlumberTimeout:   return "not reached within 5 ms";
    case kSlumberAlready:   return "already in Slumber";
    default:                return "not asked: driver work loop not available";
    }
}

const char *usbSpeedName(UInt32 speed)
{
    static const char *const names[] = {"?", "FS", "LS", "HS", "SS", "SS+"};
    return speed < sizeof(names) / sizeof(names[0]) ? names[speed] : "?";
}

const char *usbLinkName(UInt32 pls)
{
    static const char *const names[16] = {"U0", "U1", "U2", "U3", "disabled", "rxdetect", "inactive", "polling",
                                          "recovery", "hotreset", "compliance", "test", "12", "13", "14", "resume"};
    return names[pls & 0xF];
}

UInt32 permille(UInt64 part, UInt64 whole)
{
    return whole == 0 ? 0 : static_cast<UInt32>((part * 1000) / whole);
}

const char *aspmName(UInt32 bits)
{
    static const char *const names[] = {"off", "L0s", "L1", "L0s+L1"};
    return names[bits & 3];
}

bool sameText(const char *a, const char *b)
{
    if (a == nullptr || b == nullptr)
        return false;
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

// "2" -> 2; anything that is not a plain decimal number -> -1
int decimalValue(const char *text)
{
    if (text == nullptr || *text == '\0')
        return -1;
    int value = 0;
    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '9' || value > 1000)
            return -1;
        value = value * 10 + (*text - '0');
    }
    return value;
}

// True if the code at function is exactly expected (only kernel addresses are read).
bool sameCode(const void *function, const UInt8 *expected, size_t length)
{
    uintptr_t address = reinterpret_cast<uintptr_t>(function);
    if (address < 0xFFFFFF8000000000ULL)
        return false;
    const volatile UInt8 *code = static_cast<const volatile UInt8 *>(function);
    for (size_t i = 0; i < length; i++) {
        if (code[i] != expected[i])
            return false;
    }
    return true;
}

// Device and function of a PCI device on bus 0 from its "reg" property ((device << 3) | function), or -1.
int pciDevFn(IORegistryEntry *entry)
{
    OSData *reg = OSDynamicCast(OSData, entry->getProperty("reg"));
    if (reg == nullptr || reg->getLength() < 4)
        return -1;
    const UInt8 *bytes = static_cast<const UInt8 *>(reg->getBytesNoCopy());
    UInt32 address = static_cast<UInt32>(bytes[0]) | (static_cast<UInt32>(bytes[1]) << 8) |
                     (static_cast<UInt32>(bytes[2]) << 16) | (static_cast<UInt32>(bytes[3]) << 24);
    if (((address >> 16) & 0xFF) != 0)
        return -1;
    return static_cast<int>((address >> 8) & 0xFF);
}

// Offset of a standard PCI capability, or 0. Uses 32-bit configuration reads only.
UInt8 findCapability(IOPCIDevice *device, UInt8 id)
{
    if (((device->configRead32(0x04) >> 16) & 0x10) == 0)
        return 0;
    UInt8 next = static_cast<UInt8>(device->configRead32(0x34) & 0xFC);
    for (unsigned guard = 0; next >= 0x40 && guard < 48; guard++) {
        UInt32 header = device->configRead32(next);
        if ((header & 0xFF) == id)
            return next;
        next = static_cast<UInt8>((header >> 8) & 0xFC);
    }
    return 0;
}

// The first PCI device behind a bridge (two levels down: bridge driver, then devices), retained.
IOPCIDevice *firstDeviceBehind(IOPCIDevice *bridge)
{
    IOPCIDevice *found = nullptr;
    OSIterator *level1 = bridge->getChildIterator(gIOServicePlane);
    if (level1 == nullptr)
        return nullptr;
    OSObject *a = nullptr;
    while (found == nullptr && (a = level1->getNextObject()) != nullptr) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, a);
        OSIterator *level2 = entry != nullptr ? entry->getChildIterator(gIOServicePlane) : nullptr;
        if (level2 == nullptr)
            continue;
        OSObject *b = nullptr;
        while ((b = level2->getNextObject()) != nullptr) {
            IOPCIDevice *device = OSDynamicCast(IOPCIDevice, b);
            if (device != nullptr) {
                found = device;
                found->retain();
                break;
            }
        }
        level2->release();
    }
    level1->release();
    return found;
}

// RAPL energy counter delta (units of 2^-unit J) over ns, in milliwatts.
UInt64 milliwatts(UInt32 delta, UInt32 unit, UInt64 ns)
{
    if (ns == 0)
        return 0;
    UInt64 microjoules = (static_cast<UInt64>(delta) * 1000000ULL) >> unit;
    return microjoules * 1000000ULL / ns;
}

} // namespace

// A bounded line of text for the log and the Results property.
class PcpLine {
public:
    PcpLine() { text[0] = '\0'; }

    __attribute__((format(printf, 2, 3))) void add(const char *format, ...)
    {
        if (length + 1 >= sizeof(text))
            return;
        va_list args;
        va_start(args, format);
        int written = vsnprintf(text + length, sizeof(text) - length, format, args);
        va_end(args);
        if (written > 0) {
            length += static_cast<size_t>(written);
            if (length > sizeof(text) - 1)
                length = sizeof(text) - 1;
        }
    }

    void addTime(UInt64 ns)
    {
        if (ns == kNone) {
            add("none");
        } else {
            UInt64 tenths = (ns + 50) / 100;
            add("%llu.%llu us", tenths / 10, tenths % 10);
        }
    }

    void addPercent(const char *label, UInt32 value)
    {
        add(" %s %u.%u%%", label, value / 10, value % 10);
    }

    const char *c_str() const { return text; }

private:
    char text[1024];
    size_t length {0};
};

struct PcpSnapshot {
    UInt64 ns;
    UInt64 tsc;
    UInt64 pc2;
    UInt64 pc3;
    UInt64 pc6;
    UInt64 pc7;
    UInt64 pc8;
    UInt64 pc9;
    UInt64 pc10;
    UInt64 busy;
    UInt32 pkgEnergy;
    UInt32 dramEnergy;
};

class PkgCProbe : public IOService {
    OSDeclareDefaultStructors(PkgCProbe);

public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;

private:
    enum State { kIdle, kWaiting, kRunning, kDone };
    enum AlpmCall : UInt32 { kCallEnable = 1, kCallDisable = 2, kCallSlumber = 3 };
    typedef UInt32 (*PortFunction)(IOService *port);

    IOPCIDevice *pmc {nullptr};
    IOMemoryMap *pwrmMap {nullptr};
    volatile UInt32 *pwrm {nullptr};
    IOPCIDevice *ahciDevice {nullptr};
    IOMemoryMap *ahciMap {nullptr};
    volatile UInt32 *ahci {nullptr};
    int ahciPort {-1};
    IOWorkLoop *workLoop {nullptr};
    IOTimerEventSource *timer {nullptr};
    IONotifier *powerNotifier {nullptr};
    OSArray *results {nullptr};

    UInt32 pwrmConfig {0};
    UInt32 pwrmBase {0};
    UInt32 ignoreOriginal {0};
    bool ignoreChanged {false};
    bool writable {false};
    bool msrUsable {false};
    UInt32 energyUnit {14};

    UInt32 startDelay {180};
    UInt32 phaseSeconds {15};
    UInt32 applyMask {0};
    bool runTest {true};
    bool keepAlpm {false};

    State state {kIdle};
    bool awake {true};
    bool applyPending {false};
    UInt32 applyCount {0};
    UInt32 phase {0};
    UInt32 tick {0};

    PcpSnapshot first {};
    UInt64 ltrMin[kLtrCount] {};
    UInt64 ltrMax[kLtrCount] {};
    UInt64 platformMin {kNone};
    UInt64 platformMax {0};
    UInt64 aggregatedMin {kNone};
    UInt64 aggregatedMax {0};
    UInt64 lowestCounted {kNone};
    int lowestBy {-1};
    UInt32 linkCount[5] {};                  // active, partial, slumber, devsleep, other
    UInt32 gatedCount[kGatedCount] {};       // samples with the IP power gated
    UInt32 limitMin {0xFF};
    UInt32 limitMax {0};
    UInt32 samples {0};

    // r5: SATA ALPM through the AHCI driver
    IOService *alpmPort {nullptr};
    PortFunction alpmSupport {nullptr};
    PortFunction alpmState {nullptr};
    PortFunction alpmEnable {nullptr};
    PortFunction alpmDisable {nullptr};
    bool alpmChecked {false};
    bool alpmReady {false};
    bool alpmWasOn {false};
    bool alpmOn {false};
    bool alpmPending {false};
    UInt32 alpmWaitMS {0};
    UInt32 alpmKeepCount {0};
    UInt32 alpmRetries {0};
    const char *alpmWhen {"at start"};
    char alpmWhy[120] {};
    char alpmNote[240] {};

    UInt32 readPwrm(UInt32 offset) const { return pwrm[offset / 4]; }
    volatile UInt32 *portRegs() const { return ahci + (0x100 + ahciPort * 0x80) / 4; }
    bool tryPwrm(UInt32 base);
    void addResult(const char *text);
    void readOptions();
    void describeStart();
    void describeLtr(const char *title);
    void findAhci();
    UInt64 ecamBase(IOPCIDevice *host);
    void describeIo();
    void describePpfear(const char *when);
    void describeRootPort(IOPCIDevice *port, int devfn, UInt64 ecam);
    void addL1ss(PcpLine &line, UInt64 ecam, UInt32 bus, UInt32 devfn, UInt32 id);
    void describeXhci(IOPCIDevice *device, const char *label);
    void describeHda(IOPCIDevice *device);
    void describeRealtek(IOPCIDevice *device, UInt32 bus);
    IOService *findAhciPortService();
    bool alpmPrepare();
    UInt32 alpmRun(UInt32 call);
    UInt32 slumberGated();
    void wakeLinkGated();
    void sataApply(UInt32 action);
    void alpmRestore();
    void alpmKeep();
    void writeIgnore(UInt32 value);
    void takeSnapshot(PcpSnapshot *snapshot);
    void resetSamples();
    void sample(UInt32 ignored);
    void addRange(PcpLine &line, UInt64 low, UInt64 high);
    void addBusDelay(PcpLine &line);
    void report(const PcpSnapshot &last);
    void beginTest();
    void stepTest();
    void finishTest(const char *why);
    void applyFix(const char *when);
    void releaseResources();
    void onTimer();

    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn setAwakeAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn alpmAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
    static IOReturn powerMessage(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument, vm_size_t argSize);
};

OSDefineMetaClassAndStructors(PkgCProbe, IOService);

bool PkgCProbe::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    pmc = OSDynamicCast(IOPCIDevice, provider);
    results = OSArray::withCapacity(32);
    if (pmc == nullptr || results == nullptr) {
        releaseResources();
        return false;
    }
    readOptions();
    setProperty("State", "starting");

    UInt32 regs[4];
    pcpCpuid(0, regs);
    bool intel = regs[1] == 0x756e6547 && regs[3] == 0x49656e69 && regs[2] == 0x6c65746e;   // GenuineIntel
    pcpCpuid(1, regs);
    UInt32 family = (regs[0] >> 8) & 0xF;
    UInt32 model = ((regs[0] >> 4) & 0xF) | (((regs[0] >> 16) & 0xF) << 4);
    msrUsable = intel && family == 6 && (model == 0x4E || model == 0x5E || model == 0x8E || model == 0x9E);

    pwrmConfig = pmc->configRead32(kPwrmBaseConfig);
    UInt32 base = pwrmConfig & 0xFFFFF000;
    if (!tryPwrm(base) && base != kPwrmDefault)
        tryPwrm(kPwrmDefault);

    workLoop = IOWorkLoop::workLoop();
    if (workLoop != nullptr)
        timer = IOTimerEventSource::timerEventSource(this, &PkgCProbe::timerAction);
    if (timer == nullptr || workLoop->addEventSource(timer) != kIOReturnSuccess) {
        IOLog("PkgCProbe: could not create the work loop\n");
        releaseResources();
        return false;
    }

    IOService *rootDomain = getPMRootDomain();
    if (rootDomain != nullptr)
        powerNotifier = rootDomain->registerInterest(gIOGeneralInterest, &PkgCProbe::powerMessage, this, nullptr);
    if (powerNotifier == nullptr) {
        IOLog("PkgCProbe: could not register for sleep and wake messages\n");
        releaseResources();
        return false;
    }

    describeStart();
    describeLtr("LTR at start:");

    // Without usable PMC registers the test still runs, but only measures (the ignore register is never written).
    if (runTest) {
        state = kWaiting;
        setProperty("State", writable ? "waiting" : "waiting (read-only)");
        timer->setTimeoutMS(startDelay * 1000);
    } else {
        state = kDone;
        setProperty("State", "no test");
        if (applyMask != 0)
            applyFix("at start");
        if (keepAlpm) {
            alpmPending = true;
            alpmWaitMS = 0;
            alpmWhen = "at start";
            timer->setTimeoutMS(startDelay * 1000);
        }
    }
    return true;
}

void PkgCProbe::stop(IOService *provider)
{
    releaseResources();
    IOService::stop(provider);
}

void PkgCProbe::free()
{
    releaseResources();
    IOService::free();
}

void PkgCProbe::releaseResources()
{
    if (powerNotifier != nullptr) {
        powerNotifier->remove();
        powerNotifier = nullptr;
    }
    if (timer != nullptr) {
        timer->cancelTimeout();
        if (workLoop != nullptr)
            workLoop->removeEventSource(timer);   // waits for a running timer action
        timer->release();
        timer = nullptr;
    }
    if (workLoop != nullptr) {
        workLoop->release();
        workLoop = nullptr;
    }
    // Nothing runs on our work loop any more: put back what a test that was cut short changed.
    if (alpmReady && alpmOn && !alpmWasOn)
        alpmRestore();
    if (state == kRunning && ignoreChanged && writable && pwrm != nullptr) {
        pwrm[kLtrIgnore / 4] = ignoreOriginal;
        ignoreChanged = false;
    }
    state = kDone;
    alpmReady = false;
    alpmSupport = alpmState = alpmEnable = alpmDisable = nullptr;
    if (alpmPort != nullptr) {
        alpmPort->release();
        alpmPort = nullptr;
    }
    if (ahciMap != nullptr) {
        ahciMap->release();
        ahciMap = nullptr;
        ahci = nullptr;
    }
    if (ahciDevice != nullptr) {
        ahciDevice->release();
        ahciDevice = nullptr;
    }
    if (pwrmMap != nullptr) {
        pwrmMap->release();
        pwrmMap = nullptr;
        pwrm = nullptr;
    }
    if (results != nullptr) {
        results->release();
        results = nullptr;
    }
    pmc = nullptr;
}

// Maps 4 KB at base and keeps the mapping only if it looks like the Sunrise Point PMC:
// no value of all ones, at least one LTR requirement bit set, no ignore bits above bit 17.
// Only then will the ignore register ever be written.
bool PkgCProbe::tryPwrm(UInt32 base)
{
    if (base == 0 || base == 0xFFFFF000)
        return false;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, kPwrmMapLength);
    if (memory == nullptr)
        return false;
    IOMemoryMap *mapping = memory->map(kIOMapInhibitCache);   // kernel map, uncached
    memory->release();
    if (mapping == nullptr)
        return false;

    volatile UInt32 *regs = reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress());
    UInt32 ignore = regs[kLtrIgnore / 4];
    bool requirement = false;
    bool floating = false;
    for (unsigned i = 0; i < kLtrCount; i++) {
        UInt32 value = regs[kLtr[i].offset / 4];
        if (value == 0xFFFFFFFF)
            floating = true;
        if ((value & 0x80008000) != 0)
            requirement = true;
    }
    if ((ignore & 0xFFFC0000) != 0 || !requirement || floating) {
        mapping->release();
        return false;
    }

    pwrmMap = mapping;
    pwrm = regs;
    pwrmBase = base;
    ignoreOriginal = ignore;
    writable = true;
    return true;
}

void PkgCProbe::addResult(const char *text)
{
    IOLog("PkgCProbe: %s\n", text);
    if (results == nullptr)
        return;
    OSString *string = OSString::withCString(text);
    if (string != nullptr) {
        results->setObject(string);
        string->release();
    }
    // Publish a copy, so that ioreg never sees an array that is being changed.
    OSArray *copy = OSArray::withArray(results);
    if (copy != nullptr) {
        setProperty("Results", copy);
        copy->release();
    }
}

void PkgCProbe::readOptions()
{
    OSNumber *number = OSDynamicCast(OSNumber, getProperty("StartDelay"));
    if (number != nullptr) {
        UInt32 value = number->unsigned32BitValue();
        startDelay = value < 30 ? 30 : (value > 3600 ? 3600 : value);
    }
    number = OSDynamicCast(OSNumber, getProperty("PhaseSeconds"));
    if (number != nullptr) {
        UInt32 value = number->unsigned32BitValue();
        phaseSeconds = value < 5 ? 5 : (value > 120 ? 120 : value);
    }
    number = OSDynamicCast(OSNumber, getProperty("ApplyMask"));
    if (number != nullptr)
        applyMask = number->unsigned32BitValue() & kIgnoreAll;
    OSBoolean *flag = OSDynamicCast(OSBoolean, getProperty("RunTest"));
    if (flag != nullptr)
        runTest = flag->isTrue();
    flag = OSDynamicCast(OSBoolean, getProperty("SataAlpm"));
    if (flag != nullptr)
        keepAlpm = flag->isTrue();
}

void PkgCProbe::describeStart()
{
    PcpLine line;
    UInt32 regs[4];
    pcpCpuid(1, regs);
    line.add("r5; CPUID 0x%08x; PMC config 0x48 = 0x%08x", regs[0], pwrmConfig);
    if (pwrm == nullptr)
        line.add("; no PMC LTR registers found there or at 0x%08x: read-only, nothing will be changed", kPwrmDefault);
    else
        line.add("; PWRMBASE 0x%08x, LTR_IGN 0x%08x", pwrmBase, ignoreOriginal);
    if (msrUsable) {
        UInt64 config = pcpReadMsr(kMsrPkgCstConfig);
        energyUnit = static_cast<UInt32>((pcpReadMsr(kMsrRaplUnit) >> 8) & 0x1F);
        line.add("; MSR 0xE2 0x%08llx (package limit %s, CFG lock %llu)", config & 0xFFFFFFFFULL,
                 limitName(static_cast<UInt32>(config & 0xF)), (config >> 15) & 1);
        line.add("; IRTL PC3 ");
        line.addTime(irtlNS(pcpReadMsr(kMsrPkgC3Irtl)));
        line.add(", PC6 ");
        line.addTime(irtlNS(pcpReadMsr(kMsrPkgC6Irtl)));
        line.add(", PC7 ");
        line.addTime(irtlNS(pcpReadMsr(kMsrPkgC7Irtl)));
        line.add("; energy unit 2^-%u J", energyUnit);
    } else {
        line.add("; not a Skylake/Kaby Lake CPU: MSRs not read");
    }
    line.add("; StartDelay %u s, PhaseSeconds %u s, RunTest %s, ApplyMask 0x%x, SataAlpm %s",
             startDelay, phaseSeconds, runTest ? "yes" : "no", applyMask, keepAlpm ? "yes" : "no");
    addResult(line.c_str());
}

void PkgCProbe::describeLtr(const char *title)
{
    if (pwrm == nullptr)
        return;
    PcpLine line;
    UInt32 platform = readPwrm(kLtrPlatform);
    UInt32 aggregated = readPwrm(kLtrAggregated);
    line.add("%s platform 0x%08x = ", title, platform);
    line.addTime(ltrNS(platform));
    line.add(", aggregated 0x%08x = ", aggregated);
    line.addTime(ltrNS(aggregated));
    line.add(";");
    bool any = false;
    for (unsigned i = 0; i < kLtrCount; i++) {
        UInt32 raw = readPwrm(kLtr[i].offset);
        if (raw == 0)
            continue;
        any = true;
        line.add(" %s 0x%08x = ", kLtr[i].name, raw);
        line.addTime(ltrNS(raw));
    }
    line.add(any ? "; all other blocks 0 (no requirement)" : " all blocks 0 (no requirement)");
    addResult(line.c_str());
}

// The AHCI controller is on the same PCI bus as the PMC: look at the PMC's siblings
// for class code 01.06.01 and map its BAR5. Only reads its registers. Kept until the driver stops.
void PkgCProbe::findAhci()
{
    if (ahciMap != nullptr)
        return;
    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    OSIterator *children = bus != nullptr ? bus->getChildIterator(gIOServicePlane) : nullptr;
    if (children != nullptr) {
        OSObject *child = nullptr;
        while (ahciMap == nullptr && (child = children->getNextObject()) != nullptr) {
            IOPCIDevice *device = OSDynamicCast(IOPCIDevice, child);
            if (device != nullptr && (device->configRead32(kPciClassConfig) >> 8) == kAhciClass) {
                ahciMap = device->mapDeviceMemoryWithRegister(kAhciBarConfig, kIOMapInhibitCache);
                if (ahciMap != nullptr) {
                    ahciDevice = device;
                    ahciDevice->retain();
                }
            }
        }
        children->release();
    }
    if (ahciMap == nullptr) {
        addResult("AHCI: no AHCI controller, or its registers could not be mapped");
        return;
    }

    ahci = reinterpret_cast<volatile UInt32 *>(ahciMap->getVirtualAddress());
    UInt64 size = ahciMap->getSize();
    UInt32 cap = ahci[0x00 / 4];
    UInt32 implemented = ahci[0x0C / 4];
    UInt32 cap2 = ahci[0x24 / 4];

    for (unsigned port = 0; port < 32; port++) {
        if ((implemented & (1u << port)) == 0)
            continue;
        if (0x100 + (port + 1) * 0x80 > size)
            break;
        volatile UInt32 *regs = ahci + (0x100 + port * 0x80) / 4;
        UInt32 status = regs[kPxSSTS / 4];
        if ((status & 0xF) != 3)
            continue;
        UInt32 command = regs[kPxCMD / 4];
        UInt32 control = regs[kPxSCTL / 4];
        UInt32 enable = regs[kPxIE / 4];
        if (ahciPort < 0)
            ahciPort = static_cast<int>(port);
        PcpLine line;
        line.add("AHCI port %u: PxCMD 0x%08x (ST %u, ALPE %u, ASP %u), PxSCTL 0x%08x (IPM %u), PxSSTS 0x%08x (link %s), "
                 "PxIE 0x%08x (PhyRdy irq %u), PxIS 0x%08x, PxSERR 0x%08x, PxCI 0x%08x, PxSACT 0x%08x, PxDEVSLP 0x%08x; "
                 "CAP 0x%08x (SALP %u, partial %u, slumber %u), CAP2 0x%08x (APST %u, DevSleep %u)",
                 port, command, command & 1, (command >> 26) & 1, (command >> 27) & 1,
                 control, (control >> 8) & 0xF, status, linkStateName((status >> 8) & 0xF),
                 enable, (enable >> 22) & 1, regs[kPxIS / 4], regs[kPxSERR / 4], regs[kPxCI / 4], regs[kPxSACT / 4],
                 regs[kPxDEVSLP / 4], cap, (cap >> 26) & 1, (cap >> 13) & 1, (cap >> 14) & 1,
                 cap2, (cap2 >> 2) & 1, (cap2 >> 3) & 1);
        addResult(line.c_str());
    }
    if (ahciPort < 0)
        addResult("AHCI: no port with a device link");
}

// PCIe configuration window: AppleACPIPlatform's "acpi-mmcfg-seg0", else the host bridge's PCIEXBAR.
UInt64 PkgCProbe::ecamBase(IOPCIDevice *host)
{
    UInt64 base = 0;
    IOPlatformExpert *platform = IOService::getPlatform();
    OSObject *object = platform != nullptr ? platform->copyProperty("acpi-mmcfg-seg0") : nullptr;
    OSNumber *number = OSDynamicCast(OSNumber, object);
    if (number != nullptr)
        base = number->unsigned64BitValue();
    if (object != nullptr)
        object->release();
    if (base == 0 && host != nullptr) {
        UInt64 bar = (static_cast<UInt64>(host->configRead32(kHostPciexbar + 4)) << 32) |
                     host->configRead32(kHostPciexbar);
        if ((bar & 1) != 0)
            base = bar & 0x7FFC000000ULL;
    }
    return base;
}

// PCH IP power gating: which IPs are on (not gated) and which are gated. Read only.
void PkgCProbe::describePpfear(const char *when)
{
    if (pwrm == nullptr)
        return;
    const volatile UInt8 *bytes = reinterpret_cast<const volatile UInt8 *>(pwrm) + kPpfear;
    UInt8 value[kPpfearBytes];
    for (unsigned i = 0; i < kPpfearBytes; i++)
        value[i] = bytes[i];
    PcpLine line;
    line.add("PCH IP power gating %s (PPFEAR %02x %02x %02x %02x %02x): on:", when,
             value[0], value[1], value[2], value[3], value[4]);
    for (unsigned pass = 0; pass < 2; pass++) {
        if (pass == 1)
            line.add("; gated:");
        for (unsigned i = 0; i < kPpfearBytes * 8; i++) {
            if (kPfearNames[i] == nullptr)
                continue;
            unsigned gated = (value[i / 8] >> (i % 8)) & 1;
            if (gated == pass)
                line.add(" %s", kPfearNames[i]);
        }
    }
    addResult(line.c_str());
}

// L1 PM substates control of one function, read through ECAM (extended configuration space).
void PkgCProbe::addL1ss(PcpLine &line, UInt64 ecam, UInt32 bus, UInt32 devfn, UInt32 id)
{
    if (ecam == 0) {
        line.add(", L1SS not read (no ECAM)");
        return;
    }
    IOMemoryMap *mapping = nullptr;
    IODeviceMemory *memory = IODeviceMemory::withRange(ecam + (static_cast<UInt64>(bus) << 20) +
                                                       (static_cast<UInt64>(devfn) << 12), kMapLength);
    if (memory != nullptr) {
        mapping = memory->map(kIOMapInhibitCache);
        memory->release();
    }
    if (mapping == nullptr) {
        line.add(", L1SS not read");
        return;
    }
    volatile UInt32 *cfg = reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress());
    if (cfg[0] != id) {
        line.add(", L1SS not read (ECAM 0x%08x)", cfg[0]);
        mapping->release();
        return;
    }
    UInt32 offset = 0x100;
    bool found = false;
    for (unsigned guard = 0; offset >= 0x100 && offset < 0x1000 && guard < 64; guard++) {
        UInt32 header = cfg[offset / 4];
        if (header == 0 || header == 0xFFFFFFFF)
            break;
        if ((header & 0xFFFF) == 0x1E) {
            UInt32 control = cfg[(offset + 8) / 4];
            line.add(", L1SS ctl 0x%02x (ASPM L1.1 %u, L1.2 %u)", control & 0xFF, (control >> 3) & 1, (control >> 2) & 1);
            found = true;
            break;
        }
        offset = (header >> 20) & 0xFFC;
    }
    if (!found)
        line.add(", no L1SS");
    mapping->release();
}

// One PCH root port and the device behind it. Read only.
void PkgCProbe::describeRootPort(IOPCIDevice *port, int devfn, UInt64 ecam)
{
    PcpLine line;
    UInt32 id = port->configRead32(0x00);
    UInt32 secondary = (port->configRead32(0x18) >> 8) & 0xFF;
    line.add("PCIe %02x.%u %04x:%04x:", devfn >> 3, devfn & 7, id & 0xFFFF, id >> 16);
    UInt8 express = findCapability(port, 0x10);
    if (express != 0) {
        UInt32 lcap = port->configRead32(static_cast<UInt8>(express + 0x0C));
        UInt32 link = port->configRead32(static_cast<UInt8>(express + 0x10));
        UInt32 slot = port->configRead32(static_cast<UInt8>(express + 0x18));
        UInt32 dctl2 = port->configRead32(static_cast<UInt8>(express + 0x28));
        line.add(" LNKCAP ASPM %s, LNKCTL 0x%04x (ASPM %s, CLKPM %u, link disable %u), LNKSTS 0x%04x (Gen%u x%u, "
                 "DLL active %u), presence %u, LTR %s",
                 aspmName((lcap >> 10) & 3), link & 0xFFFF, aspmName(link & 3), (link >> 8) & 1, (link >> 4) & 1,
                 link >> 16, (link >> 16) & 0xF, (link >> 20) & 0x3F, (link >> 29) & 1, (slot >> 22) & 1,
                 ((dctl2 >> 10) & 1) != 0 ? "on" : "off");
    }
    addL1ss(line, ecam, 0, static_cast<UInt32>(devfn), id);

    IOPCIDevice *device = firstDeviceBehind(port);
    UInt32 deviceId = 0;
    UInt32 deviceClass = 0;
    if (device == nullptr) {
        line.add(" | no device");
    } else {
        deviceId = device->configRead32(0x00);
        deviceClass = device->configRead32(kPciClassConfig) >> 8;
        line.add(" | %02x:00.0 %04x:%04x class %06x", secondary, deviceId & 0xFFFF, deviceId >> 16, deviceClass);
        UInt8 pm = findCapability(device, 0x01);
        if (pm != 0)
            line.add(", D%u", device->configRead32(static_cast<UInt8>(pm + 4)) & 3);
        UInt8 deviceExpress = findCapability(device, 0x10);
        if (deviceExpress != 0) {
            UInt32 lcap = device->configRead32(static_cast<UInt8>(deviceExpress + 0x0C));
            UInt32 link = device->configRead32(static_cast<UInt8>(deviceExpress + 0x10));
            UInt32 dctl2 = device->configRead32(static_cast<UInt8>(deviceExpress + 0x28));
            line.add(", LNKCAP ASPM %s, LNKCTL 0x%04x (ASPM %s, CLKPM %u), LTR %s",
                     aspmName((lcap >> 10) & 3), link & 0xFFFF, aspmName(link & 3), (link >> 8) & 1,
                     ((dctl2 >> 10) & 1) != 0 ? "on" : "off");
        }
        addL1ss(line, ecam, secondary, 0, deviceId);
    }
    addResult(line.c_str());

    if (device != nullptr) {
        if (deviceClass == kXhciClass) {
            char label[48];
            snprintf(label, sizeof(label), "xHCI %02x:00.0 %04x:%04x", secondary, deviceId & 0xFFFF, deviceId >> 16);
            describeXhci(device, label);
        } else if (deviceId == kRealtekId) {
            describeRealtek(device, secondary);
        }
        device->release();
    }
}

// Port status of an xHCI controller: connected ports with speed and link state. Read only.
void PkgCProbe::describeXhci(IOPCIDevice *device, const char *label)
{
    PcpLine line;
    line.add("%s:", label);
    UInt8 pm = findCapability(device, 0x01);
    if (pm != 0)
        line.add(" D%u,", device->configRead32(static_cast<UInt8>(pm + 4)) & 3);
    IOMemoryMap *mapping = device->mapDeviceMemoryWithRegister(kBar0Config, kIOMapInhibitCache);
    if (mapping == nullptr) {
        line.add(" registers not mapped");
        addResult(line.c_str());
        return;
    }
    volatile UInt32 *regs = reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress());
    UInt64 size = mapping->getSize();
    UInt32 cap0 = regs[0];
    if (cap0 == 0xFFFFFFFF) {
        line.add(" registers read all ones (powered off)");
    } else {
        UInt32 capLength = cap0 & 0xFF;
        UInt32 ports = (regs[1] >> 24) & 0xFF;
        UInt32 command = capLength + 4 <= size ? regs[capLength / 4] : 0;
        UInt32 status = capLength + 8 <= size ? regs[(capLength + 4) / 4] : 0;
        line.add(" USBCMD run %u, USBSTS halted %u; connected:", command & 1, status & 1);
        UInt32 connected = 0;
        for (UInt32 n = 1; n <= ports; n++) {
            UInt64 offset = capLength + 0x400 + 0x10 * (n - 1);
            if (offset + 4 > size)
                break;
            UInt32 portsc = regs[offset / 4];
            if ((portsc & 1) == 0)
                continue;
            connected++;
            line.add(" p%u %s %s", n, usbSpeedName((portsc >> 10) & 0xF), usbLinkName((portsc >> 5) & 0xF));
        }
        line.add(" (%u of %u ports)", connected, ports);
    }
    mapping->release();
    addResult(line.c_str());
}

// HD Audio: which DMA streams are running, and whether their position moves. Read only.
void PkgCProbe::describeHda(IOPCIDevice *device)
{
    PcpLine line;
    int devfn = pciDevFn(device);
    line.add("HDA %02x.%u:", devfn >> 3, devfn & 7);
    UInt8 pm = findCapability(device, 0x01);
    if (pm != 0)
        line.add(" D%u,", device->configRead32(static_cast<UInt8>(pm + 4)) & 3);
    IOMemoryMap *mapping = device->mapDeviceMemoryWithRegister(kBar0Config, kIOMapInhibitCache);
    if (mapping == nullptr) {
        line.add(" registers not mapped");
        addResult(line.c_str());
        return;
    }
    volatile UInt32 *regs = reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress());
    UInt64 size = mapping->getSize();
    UInt32 gcap = regs[0] & 0xFFFF;
    UInt32 inputs = (gcap >> 8) & 0xF;
    UInt32 outputs = (gcap >> 12) & 0xF;
    UInt32 both = (gcap >> 3) & 0x1F;
    UInt32 streams = inputs + outputs + both;
    if (regs[0] == 0xFFFFFFFF) {
        line.add(" registers read all ones (powered off)");
    } else {
        line.add(" GCTL 0x%08x, streams in %u out %u bi %u; running:", regs[0x08 / 4], inputs, outputs, both);
        UInt32 running = 0;
        UInt32 before[30] = {};
        for (UInt32 i = 0; i < streams && i < 30; i++) {
            UInt64 base = 0x80 + 0x20 * i;
            if (base + 8 > size)
                break;
            before[i] = regs[(base + 4) / 4];
        }
        IODelay(2000);
        for (UInt32 i = 0; i < streams && i < 30; i++) {
            UInt64 base = 0x80 + 0x20 * i;
            if (base + 8 > size)
                break;
            UInt32 control = regs[base / 4];
            if (((control >> 1) & 1) == 0)
                continue;
            running++;
            const char *kind = i < inputs ? "in" : (i < inputs + outputs ? "out" : "bi");
            UInt32 index = i < inputs ? i : (i < inputs + outputs ? i - inputs : i - inputs - outputs);
            UInt32 after = regs[(base + 4) / 4];
            line.add(" %s%u (position %s)", kind, index, after != before[i] ? "moving" : "not moving");
        }
        if (running == 0)
            line.add(" none");
    }
    mapping->release();
    addResult(line.c_str());
}

// RTL8111: the chip's own ASPM and clock request enables, and the Ethernet link. Read only.
void PkgCProbe::describeRealtek(IOPCIDevice *device, UInt32 bus)
{
    PcpLine line;
    line.add("RTL8111 %02x:00.0:", bus);
    IOMemoryMap *mapping = device->mapDeviceMemoryWithRegister(kBar2Config, kIOMapInhibitCache);
    if (mapping == nullptr || mapping->getSize() < 0x100) {
        line.add(" registers not mapped");
        if (mapping != nullptr)
            mapping->release();
        addResult(line.c_str());
        return;
    }
    volatile UInt8 *regs = reinterpret_cast<volatile UInt8 *>(mapping->getVirtualAddress());
    UInt32 config2 = regs[0x53];
    UInt32 config5 = regs[0x56];
    UInt32 phy = regs[0x6C];
    if (config2 == 0xFF && config5 == 0xFF && phy == 0xFF)
        line.add(" registers read all ones (powered off)");
    else
        line.add(" Config2 0x%02x (ClkReqEn %u), Config5 0x%02x (ASPM_en %u), PHYstatus 0x%02x (Ethernet link %s)",
                 config2, (config2 >> 7) & 1, config5, config5 & 1, phy, ((phy >> 1) & 1) != 0 ? "up" : "down");
    mapping->release();
    addResult(line.c_str());
}

// Read-only survey of what may keep the chipset busy.
void PkgCProbe::describeIo()
{
    describePpfear("before the test");
    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    if (bus == nullptr)
        return;

    IOPCIDevice *host = nullptr;
    OSIterator *children = bus->getChildIterator(gIOServicePlane);
    if (children == nullptr)
        return;
    OSObject *child = nullptr;
    while (host == nullptr && (child = children->getNextObject()) != nullptr) {
        IOPCIDevice *device = OSDynamicCast(IOPCIDevice, child);
        if (device != nullptr && pciDevFn(device) == 0)
            host = device;
    }
    UInt64 ecam = ecamBase(host);
    children->release();

    children = bus->getChildIterator(gIOServicePlane);
    if (children == nullptr)
        return;
    while ((child = children->getNextObject()) != nullptr) {
        IOPCIDevice *device = OSDynamicCast(IOPCIDevice, child);
        if (device == nullptr)
            continue;
        int devfn = pciDevFn(device);
        UInt32 deviceClass = device->configRead32(kPciClassConfig) >> 8;
        if (deviceClass == kBridgeClass && devfn >= (0x1B << 3) && devfn < (0x1E << 3)) {
            describeRootPort(device, devfn, ecam);
        } else if (deviceClass == kXhciClass) {
            char label[24];
            snprintf(label, sizeof(label), "xHCI 00:%02x.%u", (devfn >> 3) & 0x1F, devfn & 7);
            describeXhci(device, label);
        } else if ((deviceClass >> 8) == kHdaClassPrefix)
            describeHda(device);
    }
    children->release();
}

// The AHCI driver's port object for ahciPort: SAT0 (IOPCIDevice) -> controller -> PRTn@n, retained.
IOService *PkgCProbe::findAhciPortService()
{
    IOService *found = nullptr;
    OSIterator *controllers = ahciDevice != nullptr ? ahciDevice->getChildIterator(gIOServicePlane) : nullptr;
    if (controllers == nullptr)
        return nullptr;
    OSObject *a = nullptr;
    while (found == nullptr && (a = controllers->getNextObject()) != nullptr) {
        IORegistryEntry *controller = OSDynamicCast(IORegistryEntry, a);
        OSIterator *ports = controller != nullptr ? controller->getChildIterator(gIOServicePlane) : nullptr;
        if (ports == nullptr)
            continue;
        OSObject *b = nullptr;
        while ((b = ports->getNextObject()) != nullptr) {
            IOService *service = OSDynamicCast(IOService, b);
            if (service != nullptr && decimalValue(service->getLocation()) == ahciPort) {
                found = service;
                found->retain();
                break;
            }
        }
        ports->release();
    }
    controllers->release();
    return found;
}

// Decides once whether ALPM may be switched through the AHCI driver.
bool PkgCProbe::alpmPrepare()
{
    if (alpmChecked)
        return alpmReady;
    alpmChecked = true;
    findAhci();

    const char *why = nullptr;
    IOService *port = nullptr;
    if (ahci == nullptr || ahciPort < 0 || ahciDevice == nullptr)
        why = "no AHCI port with a device link";
    if (why == nullptr) {
        port = findAhciPortService();
        if (port == nullptr)
            why = "the AHCI driver's port object was not found";
    }
    if (why == nullptr) {
        const OSMetaClass *meta = port->getMetaClass();
        if (meta == nullptr || !sameText(meta->getClassName(), kAhciPortClass))
            why = "the AHCI port driver is not CtlnaIntelPchSeriesAHCIPort";
    }
    if (why == nullptr) {
        const void *const *vtable = *reinterpret_cast<const void *const *const *>(port);
        const void *support = vtable[kSlotAlpmSupport];
        const void *linkState = vtable[kSlotAlpmState];
        const void *enable = vtable[kSlotAlpmEnable];
        const void *disable = vtable[kSlotAlpmDisable];
        if (!sameCode(support, kCodeAlpmSupport, sizeof(kCodeAlpmSupport)) ||
            !sameCode(linkState, kCodeAlpmState, sizeof(kCodeAlpmState)) ||
            !sameCode(enable, kCodeAlpmEnable, sizeof(kCodeAlpmEnable)) ||
            !sameCode(disable, kCodeAlpmDisable, sizeof(kCodeAlpmDisable))) {
            why = "the driver's ALPM code is not the CtlnaAHCIPort 341.0.2 code that was checked";
        } else {
            alpmSupport = reinterpret_cast<PortFunction>(const_cast<void *>(support));
            alpmState = reinterpret_cast<PortFunction>(const_cast<void *>(linkState));
            alpmEnable = reinterpret_cast<PortFunction>(const_cast<void *>(enable));
            alpmDisable = reinterpret_cast<PortFunction>(const_cast<void *>(disable));
        }
    }
    if (why == nullptr) {
        volatile UInt32 *regs = portRegs();
        UInt32 cap = ahci[0x00 / 4];
        if (((cap >> 26) & 1) == 0 || ((cap >> 13) & 1) == 0 || ((cap >> 14) & 1) == 0)
            why = "the controller does not support ALPM, Partial and Slumber";
        else if ((regs[kPxSSTS / 4] & 0xF) != 3)
            why = "no device link";
        else if ((regs[kPxCMD / 4] & kCmdStart) == 0)
            why = "the port is not running";
        else if ((regs[kPxIE / 4] & kIePhyRdy) != 0)
            why = "PhyRdy-change interrupts are on for this port (hot plug)";
        else if (port->getWorkLoop() == nullptr)
            why = "the AHCI driver has no work loop";
        else if (alpmSupport(port) == 0)
            why = "the AHCI driver reports no ALPM support for this port";
    }
    if (why != nullptr) {
        snprintf(alpmWhy, sizeof(alpmWhy), "%s", why);
        alpmSupport = alpmState = alpmEnable = alpmDisable = nullptr;
        if (port != nullptr)
            port->release();
        PcpLine line;
        line.add("SATA ALPM: nothing will be changed (%s)", why);
        addResult(line.c_str());
        return false;
    }

    alpmPort = port;                         // keeps the retain from findAhciPortService
    alpmReady = true;
    alpmWasOn = (portRegs()[kPxCMD / 4] & kCmdAlpe) != 0;
    alpmOn = alpmWasOn;
    PcpLine line;
    line.add("SATA ALPM: ready; driver %s, port %d, its ALPM support %u, link state %u; ALPM was %s before",
             kAhciPortClass, ahciPort, alpmSupport(port), alpmState(port), alpmWasOn ? "on" : "off");
    addResult(line.c_str());
    return true;
}

// Runs one ALPM call on the AHCI driver's work loop, so it never overlaps the driver's own work.
UInt32 PkgCProbe::alpmRun(UInt32 call)
{
    UInt32 result = 0xFFFFFFFF;
    if (!alpmReady || alpmPort == nullptr || alpmPort->isInactive())
        return result;
    IOWorkLoop *loop = alpmPort->getWorkLoop();
    if (loop == nullptr)
        return result;
    loop->runAction(&PkgCProbe::alpmAction, this, reinterpret_cast<void *>(static_cast<uintptr_t>(call)), &result);
    return result;
}

IOReturn PkgCProbe::alpmAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg2; (void)arg3;
    PkgCProbe *self = OSDynamicCast(PkgCProbe, owner);
    UInt32 *result = static_cast<UInt32 *>(arg1);
    if (self == nullptr || result == nullptr || self->alpmPort == nullptr || self->alpmEnable == nullptr)
        return kIOReturnSuccess;
    switch (static_cast<UInt32>(reinterpret_cast<uintptr_t>(arg0))) {
    case kCallEnable:
        *result = self->alpmEnable(self->alpmPort);
        break;
    case kCallDisable:
        *result = self->alpmDisable(self->alpmPort);
        self->wakeLinkGated();
        break;
    case kCallSlumber:
        *result = self->slumberGated();
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}

// Asks for Slumber the way the driver's inactivity timer does: from Partial first back to
// Active (1 ms), then PxCMD.ICC = Slumber, and up to 5 ms for the link to get there.
// Only when no command is outstanding. Runs on the AHCI driver's work loop.
UInt32 PkgCProbe::slumberGated()
{
    volatile UInt32 *regs = portRegs();
    if (regs[kPxCI / 4] != 0 || regs[kPxSACT / 4] != 0 || (regs[kPxCMD / 4] & kCmdStart) == 0)
        return kSlumberBusy;
    UInt32 ipm = (regs[kPxSSTS / 4] >> 8) & 0xF;
    if (ipm == 6)
        return kSlumberAlready;
    if (ipm == 2) {
        regs[kPxCMD / 4] = (regs[kPxCMD / 4] & ~kCmdIccMask) | kCmdIccActive;
        IOSleep(1);
        ipm = (regs[kPxSSTS / 4] >> 8) & 0xF;
    }
    if (ipm != 1)
        return kSlumberNotActive;
    regs[kPxCMD / 4] = (regs[kPxCMD / 4] & ~kCmdIccMask) | kCmdIccSlumber;
    for (unsigned k = 0; k < 5; k++) {
        if (((regs[kPxSSTS / 4] >> 8) & 0xF) == 6)
            return kSlumberReached;
        IOSleep(1);
    }
    return ((regs[kPxSSTS / 4] >> 8) & 0xF) == 6 ? kSlumberReached : kSlumberTimeout;
}

// After ALPM is off: wake the link if it is idle in Partial or Slumber (up to 10 ms), and clear the
// PhyRdy-change and Comm-Wake bits that the link changes left in PxSERR (write 1 to clear; nothing else).
// Runs on the AHCI driver's work loop.
void PkgCProbe::wakeLinkGated()
{
    volatile UInt32 *regs = portRegs();
    UInt32 ipm = (regs[kPxSSTS / 4] >> 8) & 0xF;
    if ((ipm == 2 || ipm == 6) && regs[kPxCI / 4] == 0 && regs[kPxSACT / 4] == 0 && (regs[kPxCMD / 4] & kCmdStart) != 0) {
        regs[kPxCMD / 4] = (regs[kPxCMD / 4] & ~kCmdIccMask) | kCmdIccActive;
        for (unsigned k = 0; k < 10 && ((regs[kPxSSTS / 4] >> 8) & 0xF) != 1; k++)
            IOSleep(1);
    }
    UInt32 diag = regs[kPxSERR / 4] & kSerrPhyRdyWake;
    if (diag != 0)
        regs[kPxSERR / 4] = diag;
}

// What a test phase does to SATA ALPM. The note goes into the phase's result line.
void PkgCProbe::sataApply(UInt32 action)
{
    alpmNote[0] = '\0';
    if (!alpmReady) {
        if (action != kSataAsFound)
            snprintf(alpmNote, sizeof(alpmNote), "SATA ALPM not used: %s", alpmWhy[0] != '\0' ? alpmWhy : "not prepared");
        return;
    }
    bool want = action == kSataAsFound ? alpmWasOn : (action & kSataAlpm) != 0;
    UInt32 status = 0;
    if (want != alpmOn) {
        status = alpmRun(want ? kCallEnable : kCallDisable);
        alpmOn = want;
    }
    UInt32 slumber = 0xFFFFFFFF;
    bool asked = (action & kSataSlumber) != 0;
    if (asked)
        slumber = alpmRun(kCallSlumber);
    UInt32 command = portRegs()[kPxCMD / 4];
    snprintf(alpmNote, sizeof(alpmNote), "ALPM %s (PxCMD 0x%08x, ALPE %u%s)%s%s", want ? "on" : "off",
             command, (command >> 26) & 1, status != 0 ? ", driver call FAILED" : "",
             asked ? ", Slumber " : "", asked ? slumberName(slumber) : "");
}

// Back to how the driver had ALPM before the test.
void PkgCProbe::alpmRestore()
{
    if (!alpmReady || alpmOn == alpmWasOn)
        return;
    alpmRun(alpmWasOn ? kCallEnable : kCallDisable);
    alpmOn = alpmWasOn;
}

// SataAlpm: ALPM on through the driver; Partial by the controller, Slumber by the driver's own timer.
void PkgCProbe::alpmKeep()
{
    alpmPending = false;
    if (!keepAlpm || !awake || !alpmPrepare())
        return;
    UInt32 status = alpmRun(kCallEnable);
    alpmOn = true;
    alpmKeepCount++;
    setProperty("SataAlpmCount", alpmKeepCount, 32);
    UInt32 command = portRegs()[kPxCMD / 4];
    bool on = (command & kCmdAlpe) != 0;
    if (alpmKeepCount <= kMaxApplyLogs || !on) {
        PcpLine line;
        line.add("SataAlpm %s: driver call 0x%x, PxCMD 0x%08x (ALPE %u), link %s", alpmWhen, status, command,
                 on ? 1 : 0, linkStateName((portRegs()[kPxSSTS / 4] >> 8) & 0xF));
        addResult(line.c_str());
    }
    // Right after a wake the driver may still be setting the port up: check once more later.
    if (!on && alpmRetries < 3 && timer != nullptr) {
        alpmRetries++;
        alpmPending = true;
        alpmWaitMS = 0;
        alpmWhen = "retry";
        timer->setTimeoutMS(kAlpmRetryMS);
    } else if (on) {
        alpmRetries = 0;
    }
}

void PkgCProbe::writeIgnore(UInt32 value)
{
    if (!writable || pwrm == nullptr)
        return;
    pwrm[kLtrIgnore / 4] = value;
    ignoreChanged = value != ignoreOriginal;
    UInt32 readBack = readPwrm(kLtrIgnore);
    if (readBack != value) {
        PcpLine line;
        line.add("LTR_IGN: wrote 0x%08x, read back 0x%08x", value, readBack);
        addResult(line.c_str());
    }
}

void PkgCProbe::takeSnapshot(PcpSnapshot *snapshot)
{
    snapshot->ns = uptimeNS();
    snapshot->tsc = pcpReadTsc();
    if (msrUsable) {
        snapshot->pc2 = pcpReadMsr(kMsrPkgC2Res);
        snapshot->pc3 = pcpReadMsr(kMsrPkgC3Res);
        snapshot->pc6 = pcpReadMsr(kMsrPkgC6Res);
        snapshot->pc7 = pcpReadMsr(kMsrPkgC7Res);
        snapshot->pc8 = pcpReadMsr(kMsrPkgC8Res);
        snapshot->pc9 = pcpReadMsr(kMsrPkgC9Res);
        snapshot->pc10 = pcpReadMsr(kMsrPkgC10Res);
        snapshot->busy = pcpReadMsr(kMsrPkgAnyCoreC0);
        snapshot->pkgEnergy = static_cast<UInt32>(pcpReadMsr(kMsrPkgEnergy));
        snapshot->dramEnergy = static_cast<UInt32>(pcpReadMsr(kMsrDramEnergy));
    } else {
        snapshot->pc2 = snapshot->pc3 = snapshot->pc6 = snapshot->pc7 = snapshot->busy = 0;
        snapshot->pc8 = snapshot->pc9 = snapshot->pc10 = 0;
        snapshot->pkgEnergy = snapshot->dramEnergy = 0;
    }
}

void PkgCProbe::resetSamples()
{
    for (unsigned i = 0; i < kLtrCount; i++) {
        ltrMin[i] = kNone;
        ltrMax[i] = 0;
    }
    platformMin = aggregatedMin = kNone;
    platformMax = aggregatedMax = 0;
    lowestCounted = kNone;
    lowestBy = -1;
    for (unsigned i = 0; i < 5; i++)
        linkCount[i] = 0;
    for (unsigned i = 0; i < kGatedCount; i++)
        gatedCount[i] = 0;
    limitMin = 0xFF;
    limitMax = 0;
    samples = 0;
}

// Called every 500 ms while a phase is measured.
void PkgCProbe::sample(UInt32 ignored)
{
    samples++;
    if (pwrm != nullptr) {
        for (unsigned i = 0; i < kLtrCount; i++) {
            UInt64 ns = ltrNS(readPwrm(kLtr[i].offset));
            if (ns < ltrMin[i])
                ltrMin[i] = ns;
            if (ns > ltrMax[i])
                ltrMax[i] = ns;
            if ((ignored & (1u << kLtr[i].ignoreBit)) == 0 && ns < lowestCounted) {
                lowestCounted = ns;
                lowestBy = static_cast<int>(i);
            }
        }
        UInt64 platform = ltrNS(readPwrm(kLtrPlatform));
        UInt64 aggregated = ltrNS(readPwrm(kLtrAggregated));
        if (platform < platformMin)
            platformMin = platform;
        if (platform > platformMax)
            platformMax = platform;
        if (aggregated < aggregatedMin)
            aggregatedMin = aggregated;
        if (aggregated > aggregatedMax)
            aggregatedMax = aggregated;
        const volatile UInt8 *pfear = reinterpret_cast<const volatile UInt8 *>(pwrm) + kPpfear;
        for (unsigned k = 0; k < kGatedCount; k++) {
            unsigned bit = kGatedShown[k];
            if (((pfear[bit / 8] >> (bit % 8)) & 1) != 0)
                gatedCount[k]++;
        }
    }

    if (ahci != nullptr && ahciPort >= 0) {
        UInt32 ipm = (portRegs()[kPxSSTS / 4] >> 8) & 0xF;
        switch (ipm) {
        case 1: linkCount[0]++; break;
        case 2: linkCount[1]++; break;
        case 6: linkCount[2]++; break;
        case 8: linkCount[3]++; break;
        default: linkCount[4]++; break;
        }
    }
    if (msrUsable) {
        UInt32 limit = static_cast<UInt32>(pcpReadMsr(kMsrPkgCstConfig) & 0xF);
        if (limit < limitMin)
            limitMin = limit;
        if (limit > limitMax)
            limitMax = limit;
    }
}

void PkgCProbe::addRange(PcpLine &line, UInt64 low, UInt64 high)
{
    line.addTime(low);
    if (high != low) {
        line.add(" to ");
        line.addTime(high);
    }
}

void PkgCProbe::addBusDelay(PcpLine &line)
{
    IOPlatformExpert *platform = IOService::getPlatform();
    OSObject *object = platform != nullptr ? platform->copyProperty("IOPlatformMaxBusDelay") : nullptr;
    OSNumber *number = OSDynamicCast(OSNumber, object);
    OSArray *array = OSDynamicCast(OSArray, object);
    if (number == nullptr && array != nullptr)
        number = OSDynamicCast(OSNumber, array->getObject(0));
    if (number == nullptr)
        line.add("unknown");
    else if (number->unsigned64BitValue() == kNone)
        line.add("no limit");
    else
        line.add("%llu ns", number->unsigned64BitValue());
    if (object != nullptr)
        object->release();
}

void PkgCProbe::report(const PcpSnapshot &last)
{
    const Phase &current = kPhases[phase];
    PcpLine line;
    if (writable)
        line.add("P%u %s (LTR_IGN 0x%08x):", phase, current.name, ignoreOriginal | current.mask);
    else
        line.add("P%u %s (read-only, LTR_IGN not changed):", phase, current.name);
    if (msrUsable) {
        UInt64 ticks = last.tsc - first.tsc;
        line.addPercent("PC2", permille(last.pc2 - first.pc2, ticks));
        line.addPercent("PC3", permille(last.pc3 - first.pc3, ticks));
        line.addPercent("PC6", permille(last.pc6 - first.pc6, ticks));
        line.addPercent("PC7", permille(last.pc7 - first.pc7, ticks));
        line.addPercent("PC8", permille(last.pc8 - first.pc8, ticks));
        line.addPercent("PC9", permille(last.pc9 - first.pc9, ticks));
        line.addPercent("PC10", permille(last.pc10 - first.pc10, ticks));
        line.addPercent("busy", permille(last.busy - first.busy, ticks));
        UInt64 ns = last.ns - first.ns;
        UInt64 package = milliwatts(last.pkgEnergy - first.pkgEnergy, energyUnit, ns);
        UInt64 dram = milliwatts(last.dramEnergy - first.dramEnergy, energyUnit, ns);
        line.add(" | package %llu.%02llu W, DRAM %llu.%02llu W",
                 package / 1000, (package % 1000) / 10, dram / 1000, (dram % 1000) / 10);
    }
    if (pwrm != nullptr && samples > 0) {
        line.add(" | platform LTR ");
        addRange(line, platformMin, platformMax);
        line.add(", aggregated ");
        addRange(line, aggregatedMin, aggregatedMax);
        line.add(" | lowest counted ");
        line.addTime(lowestCounted);
        if (lowestBy >= 0)
            line.add(" (%s)", kLtr[lowestBy].name);
        for (unsigned k = 0; k < sizeof(kShown) / sizeof(kShown[0]); k++) {
            unsigned i = kShown[k];
            line.add(" | %s ", kLtr[i].name);
            addRange(line, ltrMin[i], ltrMax[i]);
        }
        line.add(" | gated of %u samples:", samples);
        for (unsigned k = 0; k < kGatedCount; k++)
            line.add(" %s %u", kPfearNames[kGatedShown[k]], gatedCount[k]);
    }
    if (ahci != nullptr && ahciPort >= 0)
        line.add(" | SATA link active %u, partial %u, slumber %u, devsleep %u, other %u; ALPE at the end %u",
                 linkCount[0], linkCount[1], linkCount[2], linkCount[3], linkCount[4],
                 (portRegs()[kPxCMD / 4] >> 26) & 1);
    if (msrUsable && samples > 0) {
        line.add(" | limit %s", limitName(limitMin));
        if (limitMax != limitMin)
            line.add(" to %s", limitName(limitMax));
    }
    line.add(" | bus delay ");
    addBusDelay(line);
    if (alpmNote[0] != '\0')
        line.add(" | %s", alpmNote);
    addResult(line.c_str());
}

void PkgCProbe::beginTest()
{
    findAhci();
    describeLtr("LTR before the test:");
    describeIo();
    alpmPrepare();
    alpmWasOn = alpmReady && (portRegs()[kPxCMD / 4] & kCmdAlpe) != 0;
    alpmOn = alpmWasOn;
    state = kRunning;
    phase = 0;
    tick = 0;
    setProperty("State", "running");
    stepTest();
}

// One step every 500 ms: set the phase, wait 2 s, then sample for PhaseSeconds.
void PkgCProbe::stepTest()
{
    const UInt32 measureTicks = phaseSeconds * 1000 / kTickMS;
    const UInt32 ignored = ignoreOriginal | kPhases[phase].mask;

    if (tick == 0) {
        writeIgnore(ignored);
        sataApply(kPhases[phase].sata);
    } else if (tick == kSettleTicks) {
        takeSnapshot(&first);
        resetSamples();
    } else if (tick > kSettleTicks) {
        sample(ignored);
        if (tick >= kSettleTicks + measureTicks) {
            PcpSnapshot last;
            takeSnapshot(&last);
            report(last);
            tick = 0;
            phase++;
            if (phase >= kPhaseCount) {
                finishTest("done");
                return;
            }
            timer->setTimeoutMS(kTickMS);
            return;
        }
    }
    tick++;
    timer->setTimeoutMS(kTickMS);
}

void PkgCProbe::finishTest(const char *why)
{
    if (timer != nullptr)
        timer->cancelTimeout();
    alpmRestore();
    if (ignoreChanged)
        writeIgnore(ignoreOriginal);
    state = kDone;
    PcpLine line;
    line.add("%s; LTR_IGN is 0x%08x", why, pwrm != nullptr ? readPwrm(kLtrIgnore) : 0);
    if (ahci != nullptr && ahciPort >= 0) {
        volatile UInt32 *regs = portRegs();
        UInt32 command = regs[kPxCMD / 4];
        line.add("; AHCI port %d: PxCMD 0x%08x (ALPE %u), link %s, PxSERR 0x%08x", ahciPort, command,
                 (command >> 26) & 1, linkStateName((regs[kPxSSTS / 4] >> 8) & 0xF), regs[kPxSERR / 4]);
    }
    addResult(line.c_str());
    if (awake)
        describePpfear("after the test");
    setProperty("State", why);
    if (applyMask != 0 && awake)
        applyFix("after the test");
    if (keepAlpm && awake) {
        alpmWhen = "after the test";
        alpmKeep();
    }
}

void PkgCProbe::applyFix(const char *when)
{
    applyPending = false;
    if (!writable || pwrm == nullptr || applyMask == 0)
        return;
    UInt32 before = readPwrm(kLtrIgnore);
    pwrm[kLtrIgnore / 4] = before | applyMask;
    UInt32 after = readPwrm(kLtrIgnore);
    applyCount++;
    setProperty("ApplyCount", applyCount, 32);
    if (applyCount <= kMaxApplyLogs) {
        PcpLine line;
        line.add("ApplyMask 0x%x %s: LTR_IGN 0x%08x -> 0x%08x", applyMask, when, before, after);
        addResult(line.c_str());
    }
}

void PkgCProbe::onTimer()
{
    if (!awake)
        return;
    switch (state) {
    case kWaiting:
        beginTest();
        break;
    case kRunning:
        stepTest();
        break;
    default:
        if (applyPending)
            applyFix("after wake");
        if (alpmPending) {
            if (alpmWaitMS != 0) {
                UInt32 wait = alpmWaitMS;
                alpmWaitMS = 0;
                timer->setTimeoutMS(wait);
            } else {
                alpmKeep();
            }
        }
        break;
    }
}

void PkgCProbe::timerAction(OSObject *owner, IOTimerEventSource *sender)
{
    (void)sender;
    PkgCProbe *self = OSDynamicCast(PkgCProbe, owner);
    if (self != nullptr)
        self->onTimer();
}

// Runs on our work loop, so it never overlaps a timer step.
IOReturn PkgCProbe::setAwakeAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3)
{
    (void)arg1; (void)arg2; (void)arg3;
    PkgCProbe *self = OSDynamicCast(PkgCProbe, owner);
    if (self == nullptr || self->timer == nullptr)
        return kIOReturnSuccess;

    if (arg0 == nullptr) {
        self->awake = false;
        self->applyPending = false;
        self->alpmPending = false;
        self->timer->cancelTimeout();
        if (self->state == kRunning) {
            // finishTest puts ALPM and the ignore register back before the Mac sleeps; with awake
            // false it does not apply ApplyMask or SataAlpm (that happens after the wake).
            self->finishTest("stopped: the Mac went to sleep during the test");
        } else if (self->state == kWaiting) {
            self->state = kDone;
            self->addResult("skipped: the Mac went to sleep before the test started");
            self->setProperty("State", "skipped");
        }
    } else {
        self->awake = true;
        if (self->state == kDone && self->applyMask != 0 && self->writable)
            self->applyPending = true;
        if (self->state == kDone && self->keepAlpm) {
            self->alpmPending = true;
            self->alpmWaitMS = kWakeAlpmMS - kWakeApplyMS;
            self->alpmRetries = 0;
            self->alpmWhen = "after wake";
        }
        if (self->applyPending || self->alpmPending)
            self->timer->setTimeoutMS(kWakeApplyMS);
    }
    return kIOReturnSuccess;
}

// Root domain messages. Returning kIOReturnSuccess acknowledges at once.
IOReturn PkgCProbe::powerMessage(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument, vm_size_t argSize)
{
    (void)refCon; (void)provider; (void)messageArgument; (void)argSize;
    PkgCProbe *self = static_cast<PkgCProbe *>(target);
    if (self == nullptr || self->workLoop == nullptr)
        return kIOReturnSuccess;

    switch (messageType) {
    case kIOMessageSystemWillSleep:
        self->workLoop->runAction(&PkgCProbe::setAwakeAction, self, nullptr);
        break;
    case kIOMessageSystemHasPoweredOn:
        self->workLoop->runAction(&PkgCProbe::setAwakeAction, self, reinterpret_cast<void *>(static_cast<uintptr_t>(1)));
        break;
    default:
        break;
    }
    return kIOReturnSuccess;
}
