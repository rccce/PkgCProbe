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
 * r5: read-only survey (PCH IP power gating, root ports and devices, xHCI ports, HD Audio
 *     streams, RTL8111 ASPM enables) and SATA ALPM through the AHCI driver. With ALPM on, the
 *     SATA drive's link never left Active and did not accept Slumber, so r6 drops the ALPM part.
 *
 * r6: (1) SATA ALPM removed (the SATA link is still counted in every phase).
 * (2) Wi-Fi link ASPM L1. AirportItlwm (ItlIwx::iwx_apm_config: "Disable L0s and L1s in PCIE
 *     register because we don't support ASPM now") clears ASPM L0s and L1 in the card's Link
 *     Control every time it starts the card, so this link never enters L1 although the root port
 *     above it allows L1. Test phases set only the card's L1 enable (Link Control bit 1), and only
 *     if the card is an Intel network controller (class 02.80) behind a PCH root port, in D0, with
 *     its link up, L1 supported, and L1 already enabled on the root port (which is not touched).
 *     Afterwards only the card's ASPM field is put back to what it was before the test.
 * (3) Idle root ports: Link Disable (Link Control bit 4) only on PCH root ports that have no
 *     device below them and no link (Data Link Layer Link Active 0); put back afterwards.
 * (4) Every phase also counts, for each connected USB port of the chipset's xHCI, the samples
 *     in U0 (active), U2 (USB 2 L1) and U3 (suspended). Read only.
 * (5) Everything is put back after the test, also if the Mac sleeps during the test.
 * (6) Optional WifiL1 (default false): after the test (or StartDelay seconds after start if
 *     RunTest is false) set the card's L1 enable, check it every 30 s and set it again when
 *     AirportItlwm has cleared it (it does so whenever it starts the card, e.g. after a wake).
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

// r6: PCI Express capability registers (offsets from the capability) and Link Control bits
constexpr UInt8  kExpDevCap   = 0x04;
constexpr UInt8  kExpLinkCap  = 0x0C;
constexpr UInt8  kExpLinkCtl  = 0x10;            // 16 bits; Link Status is the upper half of the dword
constexpr UInt8  kExpSlotCap  = 0x14;
constexpr UInt8  kExpSlotCtl  = 0x18;            // 16 bits; Slot Status is the upper half of the dword
constexpr UInt8  kExpDevCap2  = 0x24;
constexpr UInt8  kExpDevCtl2  = 0x28;
constexpr UInt16 kLinkAspmL0s = 1u << 0;
constexpr UInt16 kLinkAspmL1  = 1u << 1;
constexpr UInt16 kLinkDisable = 1u << 4;
constexpr UInt32 kLinkDllActive = 1u << 29;      // in the Link Control/Status dword
constexpr UInt32 kWifiClassPrefix = 0x0280;      // network controller, other (Intel Wi-Fi)
constexpr UInt32 kIntelVendor = 0x8086;
constexpr unsigned kMaxIdlePorts = 8;
constexpr unsigned kMaxUsbPorts = 32;
constexpr UInt8  kPchXhciDevFn = 0x14 << 3;      // 00:14.0

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

// r6: what a phase does besides the ignore register
enum PhaseAction : UInt32 {
    kAsFound      = 0,
    kWifiL1       = 1u << 0,    // ASPM L1 enable on the Wi-Fi card (root port already has L1)
    kIdlePortsOff = 1u << 1,    // Link Disable on PCH root ports with no device and no link
};

struct Phase {
    UInt32 mask;
    UInt32 actions;
    const char *name;
};

const Phase kPhases[] = {
    {kIgnoreSATA, kAsFound,                "ignore SATA (daily setting)"},
    {kIgnoreSATA, kWifiL1,                 "ignore SATA + Wi-Fi L1"},
    {kIgnoreAll,  kWifiL1,                 "ignore all + Wi-Fi L1"},
    {kIgnoreAll,  kWifiL1 | kIdlePortsOff, "ignore all + Wi-Fi L1 + idle root ports off"},
    {kIgnoreAll,  kIdlePortsOff,           "ignore all + idle root ports off (Wi-Fi as found)"},
    {kIgnoreSATA, kAsFound,                "ignore SATA (daily setting) again"},
};
constexpr unsigned kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

constexpr UInt32 kTickMS       = 500;
constexpr UInt32 kSettleTicks  = 4;          // 2 s between changing the settings and measuring
constexpr UInt32 kWakeApplyMS  = 1000;
constexpr UInt32 kKeepCheckMS  = 30000;      // WifiL1: how often the card's L1 enable is checked
constexpr UInt32 kMaxApplyLogs = 3;

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

// PCIe Link Capabilities bits 17:15 (L1 exit latency) and Device Capabilities bits 11:9
// (endpoint L1 acceptable latency).
const char *l1ExitName(UInt32 code)
{
    static const char *const names[] = {"<1 us", "1-2 us", "2-4 us", "4-8 us", "8-16 us", "16-32 us", "32-64 us", ">64 us"};
    return names[code & 7];
}

const char *l1AcceptName(UInt32 code)
{
    static const char *const names[] = {"1 us", "2 us", "4 us", "8 us", "16 us", "32 us", "64 us", "no limit"};
    return names[code & 7];
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
    bool empty() const { return length == 0; }
    void clear()
    {
        text[0] = '\0';
        length = 0;
    }

private:
    char text[1536];
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

    // r6: a PCH root port without device and link, and its Link Control before the test
    struct IdlePort {
        IOPCIDevice *port;
        UInt8 cap;
        int devfn;
        UInt16 original;
    };

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
    bool keepWifiL1 {false};

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
    UInt32 linkCount[5] {};                  // SATA link: active, partial, slumber, devsleep, other
    UInt32 gatedCount[kGatedCount] {};       // samples with the IP power gated
    UInt32 limitMin {0xFF};
    UInt32 limitMax {0};
    UInt32 samples {0};

    // r6: Intel Wi-Fi card and the root port above it (both retained)
    IOPCIDevice *wifiPort {nullptr};
    IOPCIDevice *wifiDevice {nullptr};
    UInt8 wifiPortCap {0};
    UInt8 wifiCap {0};
    UInt8 wifiPm {0};
    int wifiPortDevFn {-1};
    UInt32 wifiBus {0};
    UInt16 wifiOriginal {0};                 // the card's Link Control before the test
    bool wifiChecked {false};
    bool wifiReady {false};
    bool wifiOn {false};                     // the test has set the card's L1 enable
    UInt32 wifiKeepCount {0};
    const char *keepWhen {"check"};
    char wifiWhy[120] {};

    // r6: idle root ports (retained)
    IdlePort idlePorts[kMaxIdlePorts] {};
    unsigned idleCount {0};
    bool idleChecked {false};
    bool idleOff {false};

    // r6: the chipset xHCI's port states during a phase
    IOMemoryMap *xhciMap {nullptr};
    volatile UInt32 *xhciRegs {nullptr};
    UInt64 xhciSize {0};
    UInt32 xhciCapLength {0};
    UInt32 xhciPorts {0};
    UInt32 usbSeen[kMaxUsbPorts] {};
    UInt32 usbU0[kMaxUsbPorts] {};
    UInt32 usbU2[kMaxUsbPorts] {};
    UInt32 usbU3[kMaxUsbPorts] {};

    PcpLine phaseNote;

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
    bool wifiPrepare();
    void wifiForget();
    UInt16 wifiControl();
    const char *wifiSet(bool on);
    void wifiKeep(const char *when);
    void idlePrepare();
    void idleSet(bool off);
    void idleRelease();
    void xhciPrepare();
    void xhciRelease();
    void applyActions(UInt32 actions);
    void restoreActions();
    void writeIgnore(UInt32 value);
    void takeSnapshot(PcpSnapshot *snapshot);
    void resetSamples();
    void sample(UInt32 ignored);
    void addRange(PcpLine &line, UInt64 low, UInt64 high);
    void report(const PcpSnapshot &last);
    void beginTest();
    void stepTest();
    void finishTest(const char *why);
    void applyFix(const char *when);
    void releaseResources();
    void onTimer();

    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn setAwakeAction(OSObject *owner, void *arg0, void *arg1, void *arg2, void *arg3);
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
        if (keepWifiL1) {
            keepWhen = "at start";
            timer->setTimeoutMS(startDelay * 1000);   // the first WifiL1 check, then every 30 s
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
    if (state == kRunning) {
        restoreActions();
        if (ignoreChanged && writable && pwrm != nullptr) {
            pwrm[kLtrIgnore / 4] = ignoreOriginal;
            ignoreChanged = false;
        }
    }
    state = kDone;
    xhciRelease();
    idleRelease();
    wifiForget();
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
    flag = OSDynamicCast(OSBoolean, getProperty("WifiL1"));
    if (flag != nullptr)
        keepWifiL1 = flag->isTrue();
}

void PkgCProbe::describeStart()
{
    PcpLine line;
    UInt32 regs[4];
    pcpCpuid(1, regs);
    line.add("r6; CPUID 0x%08x; PMC config 0x48 = 0x%08x", regs[0], pwrmConfig);
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
    line.add("; StartDelay %u s, PhaseSeconds %u s, RunTest %s, ApplyMask 0x%x, WifiL1 %s",
             startDelay, phaseSeconds, runTest ? "yes" : "no", applyMask, keepWifiL1 ? "yes" : "no");
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
    UInt32 slot = 0;
    if (express != 0) {
        UInt32 lcap = port->configRead32(static_cast<UInt8>(express + kExpLinkCap));
        UInt32 link = port->configRead32(static_cast<UInt8>(express + kExpLinkCtl));
        slot = port->configRead32(static_cast<UInt8>(express + kExpSlotCtl));
        UInt32 dctl2 = port->configRead32(static_cast<UInt8>(express + kExpDevCtl2));
        line.add(" LNKCAP ASPM %s, LNKCTL 0x%04x (ASPM %s, CLKPM %u, link disable %u), LNKSTS 0x%04x (Gen%u x%u, "
                 "training %u, DLL active %u), presence %u, LTR %s",
                 aspmName((lcap >> 10) & 3), link & 0xFFFF, aspmName(link & 3), (link >> 8) & 1, (link >> 4) & 1,
                 link >> 16, (link >> 16) & 0xF, (link >> 20) & 0x3F, (link >> 27) & 1, (link >> 29) & 1,
                 (slot >> 22) & 1, ((dctl2 >> 10) & 1) != 0 ? "on" : "off");
    }
    addL1ss(line, ecam, 0, static_cast<UInt32>(devfn), id);

    IOPCIDevice *device = firstDeviceBehind(port);
    UInt32 deviceId = 0;
    UInt32 deviceClass = 0;
    if (device == nullptr) {
        line.add(" | no device");
        if (express != 0) {
            UInt32 slotCap = port->configRead32(static_cast<UInt8>(express + kExpSlotCap));
            line.add("; SLTCAP 0x%08x (hot plug capable %u, surprise %u, slot %u), SLTCTL 0x%04x, SLTSTA 0x%04x",
                     slotCap, (slotCap >> 6) & 1, (slotCap >> 5) & 1, slotCap >> 19, slot & 0xFFFF, slot >> 16);
        }
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

// r6: finds the Intel Wi-Fi card (vendor 8086, class 02.80) behind a PCH root port and decides
// once whether its ASPM L1 enable may be switched. Prints what it found. Only reads.
bool PkgCProbe::wifiPrepare()
{
    if (wifiChecked)
        return wifiReady;
    wifiChecked = true;
    wifiReady = false;
    wifiWhy[0] = '\0';

    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    OSIterator *children = bus != nullptr ? bus->getChildIterator(gIOServicePlane) : nullptr;
    if (children != nullptr) {
        OSObject *child = nullptr;
        while (wifiDevice == nullptr && (child = children->getNextObject()) != nullptr) {
            IOPCIDevice *port = OSDynamicCast(IOPCIDevice, child);
            if (port == nullptr)
                continue;
            int devfn = pciDevFn(port);
            if (devfn < (0x1B << 3) || devfn >= (0x1E << 3) ||
                (port->configRead32(kPciClassConfig) >> 8) != kBridgeClass)
                continue;
            IOPCIDevice *device = firstDeviceBehind(port);
            if (device == nullptr)
                continue;
            UInt32 id = device->configRead32(0x00);
            UInt32 deviceClass = device->configRead32(kPciClassConfig) >> 8;
            if ((id & 0xFFFF) == kIntelVendor && (deviceClass >> 8) == kWifiClassPrefix) {
                wifiDevice = device;                 // keeps the retain from firstDeviceBehind
                wifiPort = port;
                wifiPort->retain();
                wifiPortDevFn = devfn;
                wifiBus = (port->configRead32(0x18) >> 8) & 0xFF;
            } else {
                device->release();
            }
        }
        children->release();
    }

    const char *why = nullptr;
    if (wifiDevice == nullptr)
        why = "no Intel Wi-Fi card (class 02.80) behind a chipset root port";
    if (why == nullptr) {
        wifiPortCap = findCapability(wifiPort, 0x10);
        wifiCap = findCapability(wifiDevice, 0x10);
        wifiPm = findCapability(wifiDevice, 0x01);
        if (wifiPortCap == 0 || wifiCap == 0 || wifiPm == 0)
            why = "PCI Express or power management capability not found";
    }

    PcpLine line;
    if (wifiDevice == nullptr) {
        line.add("Wi-Fi");
    } else {
        UInt32 id = wifiDevice->configRead32(0x00);
        line.add("Wi-Fi %02x:00.0 %04x:%04x behind %02x.%u", wifiBus, id & 0xFFFF, id >> 16,
                 wifiPortDevFn >> 3, wifiPortDevFn & 7);
    }
    if (why == nullptr) {
        UInt32 cardLink = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpLinkCtl));
        UInt32 cardLcap = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpLinkCap));
        UInt32 cardDevCap = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpDevCap));
        UInt32 cardDevCap2 = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpDevCap2));
        UInt32 cardDevCtl2 = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpDevCtl2));
        UInt32 pmcsr = wifiDevice->configRead32(static_cast<UInt8>(wifiPm + 4));
        UInt32 portLink = wifiPort->configRead32(static_cast<UInt8>(wifiPortCap + kExpLinkCtl));
        UInt32 portLcap = wifiPort->configRead32(static_cast<UInt8>(wifiPortCap + kExpLinkCap));
        UInt32 portDevCtl2 = wifiPort->configRead32(static_cast<UInt8>(wifiPortCap + kExpDevCtl2));
        wifiOriginal = static_cast<UInt16>(cardLink & 0xFFFF);
        if (cardLink == 0xFFFFFFFF) {
            why = "the card reads all ones";
        } else {
            line.add(": D%u, card LNKCTL 0x%04x (ASPM %s, CLKPM %u), LNKCAP ASPM %s, L1 exit %s, accepts L1 latency %s, "
                     "LTR %s (supported %u); root port LNKCTL 0x%04x (ASPM %s), L1 exit %s, LTR %s, link %s",
                     pmcsr & 3, cardLink & 0xFFFF, aspmName(cardLink & 3), (cardLink >> 8) & 1,
                     aspmName((cardLcap >> 10) & 3), l1ExitName((cardLcap >> 15) & 7),
                     l1AcceptName((cardDevCap >> 9) & 7), ((cardDevCtl2 >> 10) & 1) != 0 ? "on" : "off",
                     (cardDevCap2 >> 11) & 1, portLink & 0xFFFF, aspmName(portLink & 3),
                     l1ExitName((portLcap >> 15) & 7), ((portDevCtl2 >> 10) & 1) != 0 ? "on" : "off",
                     (portLink & kLinkDllActive) != 0 ? "up" : "down");
            if (((cardLcap >> 10) & 2) == 0)
                why = "the card does not support L1";
        }
    }
    if (why != nullptr) {
        snprintf(wifiWhy, sizeof(wifiWhy), "%s", why);
        line.add("; L1 will not be changed (%s)", why);
        addResult(line.c_str());
        return false;
    }
    wifiReady = true;
    line.add("; ready (only the card's L1 enable is switched; the root port is not written)");
    addResult(line.c_str());
    return true;
}

void PkgCProbe::wifiForget()
{
    if (wifiDevice != nullptr) {
        wifiDevice->release();
        wifiDevice = nullptr;
    }
    if (wifiPort != nullptr) {
        wifiPort->release();
        wifiPort = nullptr;
    }
    wifiCap = wifiPortCap = wifiPm = 0;
    wifiChecked = wifiReady = wifiOn = false;
}

// The card's Link Control, or 0xFFFF if it cannot be read.
UInt16 PkgCProbe::wifiControl()
{
    if (wifiDevice == nullptr || wifiCap == 0)
        return 0xFFFF;
    UInt32 link = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpLinkCtl));
    return link == 0xFFFFFFFF ? 0xFFFF : static_cast<UInt16>(link & 0xFFFF);
}

// on: set the card's ASPM L1 enable, only while the card is in D0, its link is up and the root
// port has L1 enabled. off: put the card's ASPM field (bits 1:0) back to its value before the
// test. Only the card's Link Control is written (16 bits). Returns nullptr when done, else why not.
const char *PkgCProbe::wifiSet(bool on)
{
    if (!wifiReady || wifiDevice == nullptr || wifiPort == nullptr)
        return "no card";
    if (wifiDevice->isInactive() || wifiPort->isInactive())
        return "the card's PCI object is going away";
    UInt32 cardLink = wifiDevice->configRead32(static_cast<UInt8>(wifiCap + kExpLinkCtl));
    if (cardLink == 0xFFFFFFFF)
        return "the card reads all ones";
    UInt16 control = static_cast<UInt16>(cardLink & 0xFFFF);
    const UInt16 aspmBits = kLinkAspmL0s | kLinkAspmL1;
    if (!on) {
        UInt16 value = static_cast<UInt16>((control & ~aspmBits) | (wifiOriginal & aspmBits));
        if (value != control)
            wifiDevice->configWrite16(static_cast<UInt8>(wifiCap + kExpLinkCtl), value);
        wifiOn = false;
        return nullptr;
    }
    if ((wifiDevice->configRead32(static_cast<UInt8>(wifiPm + 4)) & 3) != 0)
        return "the card is not in D0";
    UInt32 portLink = wifiPort->configRead32(static_cast<UInt8>(wifiPortCap + kExpLinkCtl));
    if ((portLink & kLinkDllActive) == 0)
        return "the link is down";
    if ((portLink & kLinkAspmL1) == 0)
        return "L1 is not enabled on the root port";
    if ((control & kLinkAspmL1) == 0)
        wifiDevice->configWrite16(static_cast<UInt8>(wifiCap + kExpLinkCtl), static_cast<UInt16>(control | kLinkAspmL1));
    wifiOn = true;
    return nullptr;
}

// WifiL1: set the card's L1 enable again when it is off (AirportItlwm clears it whenever it
// starts the card). Does nothing while the card is not in D0 (for example Wi-Fi switched off).
void PkgCProbe::wifiKeep(const char *when)
{
    if (!keepWifiL1 || !awake)
        return;
    if (wifiDevice != nullptr && (wifiDevice->isInactive() || wifiPort->isInactive()))
        wifiForget();
    if (!wifiPrepare())
        return;
    UInt16 before = wifiControl();
    if (before == 0xFFFF || (before & kLinkAspmL1) != 0)
        return;
    if (wifiSet(true) != nullptr)
        return;
    wifiKeepCount++;
    setProperty("WifiL1Count", wifiKeepCount, 32);
    if (wifiKeepCount <= kMaxApplyLogs) {
        PcpLine line;
        line.add("WifiL1 %s: card LNKCTL 0x%04x -> 0x%04x", when, before, wifiControl());
        addResult(line.c_str());
    }
}

// r6: PCH root ports with no device below them and no link (DLL Link Active 0, not disabled).
void PkgCProbe::idlePrepare()
{
    if (idleChecked)
        return;
    idleChecked = true;
    PcpLine line;
    line.add("Idle root ports (no device, no link):");
    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    OSIterator *children = bus != nullptr ? bus->getChildIterator(gIOServicePlane) : nullptr;
    if (children != nullptr) {
        OSObject *child = nullptr;
        while (idleCount < kMaxIdlePorts && (child = children->getNextObject()) != nullptr) {
            IOPCIDevice *port = OSDynamicCast(IOPCIDevice, child);
            if (port == nullptr)
                continue;
            int devfn = pciDevFn(port);
            if (devfn < (0x1B << 3) || devfn >= (0x1E << 3) ||
                (port->configRead32(kPciClassConfig) >> 8) != kBridgeClass)
                continue;
            UInt8 cap = findCapability(port, 0x10);
            if (cap == 0)
                continue;
            UInt32 link = port->configRead32(static_cast<UInt8>(cap + kExpLinkCtl));
            if (link == 0xFFFFFFFF || (link & kLinkDllActive) != 0 || (link & kLinkDisable) != 0)
                continue;
            IOPCIDevice *below = firstDeviceBehind(port);
            if (below != nullptr) {
                below->release();
                continue;
            }
            port->retain();
            idlePorts[idleCount].port = port;
            idlePorts[idleCount].cap = cap;
            idlePorts[idleCount].devfn = devfn;
            idlePorts[idleCount].original = static_cast<UInt16>(link & 0xFFFF);
            idleCount++;
            line.add(" %02x.%u (LNKCTL 0x%04x)", devfn >> 3, devfn & 7, link & 0xFFFF);
        }
        children->release();
    }
    if (idleCount == 0)
        line.add(" none");
    addResult(line.c_str());
}

// off: Link Disable on these idle root ports (skipped if a link has come up meanwhile);
// otherwise put their Link Disable bit back as it was before the test. 16-bit writes.
void PkgCProbe::idleSet(bool off)
{
    for (unsigned i = 0; i < idleCount; i++) {
        IdlePort &entry = idlePorts[i];
        if (entry.port == nullptr || entry.port->isInactive())
            continue;
        UInt32 link = entry.port->configRead32(static_cast<UInt8>(entry.cap + kExpLinkCtl));
        if (link == 0xFFFFFFFF)
            continue;
        UInt16 control = static_cast<UInt16>(link & 0xFFFF);
        UInt16 value;
        if (off) {
            if ((link & kLinkDllActive) != 0)
                continue;
            value = static_cast<UInt16>(control | kLinkDisable);
        } else {
            value = static_cast<UInt16>((control & ~kLinkDisable) | (entry.original & kLinkDisable));
        }
        if (value != control)
            entry.port->configWrite16(static_cast<UInt8>(entry.cap + kExpLinkCtl), value);
    }
    idleOff = off;
}

void PkgCProbe::idleRelease()
{
    for (unsigned i = 0; i < idleCount; i++) {
        if (idlePorts[i].port != nullptr) {
            idlePorts[i].port->release();
            idlePorts[i].port = nullptr;
        }
    }
    idleCount = 0;
    idleChecked = false;
    idleOff = false;
}

// r6: maps the chipset xHCI (00:14.0) registers to count its port states during the phases. Read only.
void PkgCProbe::xhciPrepare()
{
    if (xhciMap != nullptr)
        return;
    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    OSIterator *children = bus != nullptr ? bus->getChildIterator(gIOServicePlane) : nullptr;
    if (children == nullptr)
        return;
    OSObject *child = nullptr;
    while (xhciMap == nullptr && (child = children->getNextObject()) != nullptr) {
        IOPCIDevice *device = OSDynamicCast(IOPCIDevice, child);
        if (device != nullptr && pciDevFn(device) == kPchXhciDevFn &&
            (device->configRead32(kPciClassConfig) >> 8) == kXhciClass)
            xhciMap = device->mapDeviceMemoryWithRegister(kBar0Config, kIOMapInhibitCache);
    }
    children->release();
    if (xhciMap == nullptr)
        return;
    xhciRegs = reinterpret_cast<volatile UInt32 *>(xhciMap->getVirtualAddress());
    xhciSize = xhciMap->getSize();
    UInt32 cap0 = xhciSize >= 8 ? xhciRegs[0] : 0xFFFFFFFF;
    if (cap0 == 0xFFFFFFFF) {
        xhciRelease();
        return;
    }
    xhciCapLength = cap0 & 0xFF;
    xhciPorts = (xhciRegs[1] >> 24) & 0xFF;
    if (xhciPorts > kMaxUsbPorts)
        xhciPorts = kMaxUsbPorts;
}

void PkgCProbe::xhciRelease()
{
    if (xhciMap != nullptr) {
        xhciMap->release();
        xhciMap = nullptr;
    }
    xhciRegs = nullptr;
    xhciSize = 0;
    xhciPorts = 0;
}

// What a phase does besides the ignore register. The note goes into the phase's result line.
void PkgCProbe::applyActions(UInt32 actions)
{
    phaseNote.clear();
    bool wantWifi = (actions & kWifiL1) != 0;
    if (wifiReady) {
        const char *why = nullptr;
        if (wantWifi)
            why = wifiSet(true);
        else if (wifiOn)
            why = wifiSet(false);
        UInt16 control = wifiControl();
        phaseNote.add("Wi-Fi card LNKCTL 0x%04x (ASPM %s)", control, aspmName(control & 3));
        if (why != nullptr)
            phaseNote.add(", not changed: %s", why);
    } else if (wantWifi) {
        phaseNote.add("Wi-Fi L1 not used: %s", wifiWhy[0] != '\0' ? wifiWhy : "not prepared");
    }

    bool wantOff = (actions & kIdlePortsOff) != 0;
    if (idleCount > 0) {
        if (wantOff != idleOff)
            idleSet(wantOff);
        phaseNote.add("%sidle root ports:", phaseNote.empty() ? "" : "; ");
        for (unsigned i = 0; i < idleCount; i++) {
            if (idlePorts[i].port == nullptr)
                continue;
            UInt32 link = idlePorts[i].port->configRead32(static_cast<UInt8>(idlePorts[i].cap + kExpLinkCtl));
            phaseNote.add(" %02x.%u %s", idlePorts[i].devfn >> 3, idlePorts[i].devfn & 7,
                          link == 0xFFFFFFFF ? "unreadable" : ((link & kLinkDisable) != 0 ? "link disabled" : "as found"));
        }
    } else if (wantOff) {
        phaseNote.add("%sno idle root port to switch off", phaseNote.empty() ? "" : "; ");
    }
}

// Puts back what the phases changed: the card's ASPM field and Link Disable on idle root ports.
void PkgCProbe::restoreActions()
{
    if (wifiOn)
        wifiSet(false);
    if (idleOff)
        idleSet(false);
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
    for (unsigned i = 0; i < kMaxUsbPorts; i++)
        usbSeen[i] = usbU0[i] = usbU2[i] = usbU3[i] = 0;
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
    if (xhciRegs != nullptr) {
        for (UInt32 n = 0; n < xhciPorts; n++) {
            UInt64 offset = xhciCapLength + 0x400 + 0x10 * n;
            if (offset + 4 > xhciSize)
                break;
            UInt32 portsc = xhciRegs[offset / 4];
            if (portsc == 0xFFFFFFFF)
                break;
            if ((portsc & 1) == 0)
                continue;
            usbSeen[n]++;
            switch ((portsc >> 5) & 0xF) {
            case 0: usbU0[n]++; break;
            case 2: usbU2[n]++; break;
            case 3: usbU3[n]++; break;
            default: break;
            }
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
        line.add(" | SATA link active %u, partial %u, slumber %u, other %u",
                 linkCount[0], linkCount[1], linkCount[2], linkCount[3] + linkCount[4]);
    if (xhciRegs != nullptr && samples > 0) {
        line.add(" | USB U0/U2/U3 of %u:", samples);
        bool any = false;
        for (UInt32 n = 0; n < xhciPorts; n++) {
            if (usbSeen[n] == 0)
                continue;
            any = true;
            line.add(" p%u %u/%u/%u", n + 1, usbU0[n], usbU2[n], usbU3[n]);
        }
        if (!any)
            line.add(" no device");
    }
    if (msrUsable && samples > 0) {
        line.add(" | limit %s", limitName(limitMin));
        if (limitMax != limitMin)
            line.add(" to %s", limitName(limitMax));
    }
    if (!phaseNote.empty())
        line.add(" | %s", phaseNote.c_str());
    if (wifiOn) {
        UInt16 control = wifiControl();
        if (control != 0xFFFF && (control & kLinkAspmL1) == 0)
            line.add(" | Wi-Fi L1 was cleared during the phase (card LNKCTL 0x%04x)", control);
    }
    addResult(line.c_str());
}

void PkgCProbe::beginTest()
{
    findAhci();
    describeLtr("LTR before the test:");
    describeIo();
    wifiPrepare();
    idlePrepare();
    xhciPrepare();
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
        applyActions(kPhases[phase].actions);
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
    restoreActions();
    if (ignoreChanged)
        writeIgnore(ignoreOriginal);
    state = kDone;
    PcpLine line;
    line.add("%s; LTR_IGN is 0x%08x", why, pwrm != nullptr ? readPwrm(kLtrIgnore) : 0);
    if (wifiReady) {
        UInt16 control = wifiControl();
        line.add("; Wi-Fi card LNKCTL 0x%04x (ASPM %s; before the test 0x%04x)", control, aspmName(control & 3),
                 wifiOriginal);
    }
    for (unsigned i = 0; i < idleCount; i++) {
        if (idlePorts[i].port == nullptr)
            continue;
        UInt32 link = idlePorts[i].port->configRead32(static_cast<UInt8>(idlePorts[i].cap + kExpLinkCtl));
        line.add("; root port %02x.%u LNKCTL 0x%04x (before the test 0x%04x)", idlePorts[i].devfn >> 3,
                 idlePorts[i].devfn & 7, link & 0xFFFF, idlePorts[i].original);
    }
    if (ahci != nullptr && ahciPort >= 0)
        line.add("; SATA link %s", linkStateName((portRegs()[kPxSSTS / 4] >> 8) & 0xF));
    addResult(line.c_str());
    xhciRelease();
    if (awake)
        describePpfear("after the test");
    setProperty("State", why);
    if (applyMask != 0 && awake)
        applyFix("after the test");
    if (keepWifiL1 && awake) {
        wifiKeep("after the test");
        if (timer != nullptr)
            timer->setTimeoutMS(kKeepCheckMS);
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
        if (keepWifiL1) {
            wifiKeep(keepWhen);
            keepWhen = "check";
            timer->setTimeoutMS(kKeepCheckMS);
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
        self->timer->cancelTimeout();
        if (self->state == kRunning) {
            // finishTest puts the Wi-Fi card, the idle root ports and the ignore register back
            // before the Mac sleeps; with awake false it does not apply ApplyMask or WifiL1.
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
        bool keep = self->state == kDone && self->keepWifiL1;
        if (keep)
            self->keepWhen = "after wake";
        if (self->applyPending || keep)
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
