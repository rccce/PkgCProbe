/*
 * PkgCProbe.kext
 *
 * One-shot diagnostic for "the CPU package never goes deeper than PC2" on an
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
 * StartDelay seconds later (default 180) it runs six phases of PhaseSeconds (default 15):
 * baseline, ignore SATA LTR, ignore XHCI LTR, ignore both, ignore all, baseline again.
 * Each phase reports package C2/C3/C6/C7 residency, package and DRAM power, the LTR
 * values, the SATA link power state (AHCI PxSSTS.IPM), the package C-state limit and
 * macOS's current maximum bus delay. Afterwards the ignore register gets its original
 * value back. If the Mac sleeps during the test, the test stops and the value is restored.
 * If the PMC registers do not look as expected, the ignore register is never written and
 * the phases only measure.
 *
 * Optional ApplyMask (default 0): after the test, or at once if RunTest is false, keep
 * these ignore bits set and set them again after every wake.
 *
 * Results: ioreg property "Results" and kernel log lines starting with "PkgCProbe:".
 *
 * r2: the PMC registers are mapped with IOMemoryDescriptor::map() (no kernel_task), and the
 * AHCI controller is looked up among the PMC's siblings on PCI bus 0 (no matching dictionary).
 *
 * r3: also reads (never writes) the CPU side of the I/O: the DMI link control of the host
 * bridge (00:00.0, DMIBAR+0x84/0x88) and the state of the CPU's PCIe graphics port PEG0
 * (00:01.0): command, bus numbers, PMCSR, link and slot registers, and through the PCIe
 * configuration window (ECAM) the registers 0x248, 0xC20, 0xC38, 0x91C-0x97C and 0xC74
 * that the firmware's dGPU power-off sequence uses on Coffee Lake MacBook Pros.
 * Read at start, after the test and after each of the first three wakes.
 *
 * r4: (1) also reads (never writes) the chipset side of the DMI link through the sideband
 * registers (SBREG_BAR 0xFD000000, port 0xEF, offsets 0x21A4/0x21A8), checked against the TCO
 * base that the SMBus controller (00:1f.4, config 0x50) reports. (2) Three new test phases with
 * all LTRs ignored try what the firmware's own non-Windows-10 power-off path and Apple's PWRD do
 * to the parked PEG0 port: D3hot (PMCSR), lane power gating (0xC20 bits 5:4 = 3, 0xC38 bit 3)
 * and bundle power gating (bit 31 of 0x91C-0x9FC). Only if PEG0 is 8086:1901 with no device
 * behind it, its link down and the L2/L3 handshake done (0x248 bit 14). Every value is written
 * back right after its phase, also if the Mac goes to sleep or the kext stops.
 * Phases also report package C8/C9/C10.
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

// AHCI controller (00:17.0 on Sunrise Point)
constexpr UInt8  kPciClassConfig = 0x08;          // bits 31:8 class code, 7:0 revision
constexpr UInt32 kAhciClass      = 0x010601;      // mass storage, SATA, AHCI
constexpr UInt8  kAhciBarConfig  = 0x24;          // BAR5 (ABAR)

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

// r4: what a phase does to PEG0 (00:01.0) besides the ignore register
enum PegAction : UInt32 {
    kPegNone    = 0,
    kPegD3      = 1u << 0,      // PMCSR power state D3hot
    kPegLanes   = 1u << 1,      // 0xC20 bits 5:4 = 3 and 0xC38 bit 3 = 1 (lane power gating)
    kPegBundles = 1u << 2,      // bit 31 of the eight bundle registers 0x91C-0x9FC
};

struct Phase {
    UInt32 mask;
    UInt32 peg;
    const char *name;
};

const Phase kPhases[] = {
    {0,           kPegNone,                          "baseline"},
    {kIgnoreSATA, kPegNone,                          "ignore SATA"},
    {kIgnoreAll,  kPegNone,                          "ignore all"},
    {kIgnoreAll,  kPegD3,                            "ignore all + PEG0 D3hot"},
    {kIgnoreAll,  kPegLanes,                         "ignore all + PEG0 lanes gated"},
    {kIgnoreAll,  kPegD3 | kPegLanes | kPegBundles,  "ignore all + PEG0 D3hot, lanes and bundles gated"},
    {0,           kPegNone,                          "baseline again"},
};
constexpr unsigned kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

constexpr UInt32 kTickMS       = 500;
constexpr UInt32 kSettleTicks  = 4;          // 2 s between changing the ignore register and measuring
constexpr UInt32 kWakeApplyMS  = 1000;
constexpr UInt32 kMaxApplyLogs = 3;

// CPU side of the I/O (r3, read only)
constexpr UInt8  kHostDeven      = 0x54;          // host bridge 00:00.0: device enable
constexpr UInt8  kHostPciexbar   = 0x60;          // PCIe configuration window (ECAM) base
constexpr UInt8  kHostDmibar     = 0x68;
constexpr UInt32 kDmiLinkCap     = 0x84;
constexpr UInt32 kDmiLinkControl = 0x88;          // bits 15:0 link control, 31:16 link status
constexpr UInt32 kEcamPeg        = 1u << 15;      // bus 0, device 1, function 0
constexpr UInt32 kMapLength      = 0x1000;
constexpr UInt32 kPegL23         = 0x248;         // bit 7 L2/L3-ready request, bit 8 back to detect, bit 14 done
constexpr UInt32 kPegC20         = 0xC20;
constexpr UInt32 kPegC38         = 0xC38;
constexpr UInt32 kPegLtssm       = 0xC74;
const UInt32 kPegBundleRegs[] = {0x91C, 0x93C, 0x95C, 0x97C, 0x99C, 0x9BC, 0x9DC, 0x9FC};   // BSP1-BSP8 in the PegSsdt
constexpr unsigned kPegBundleCount = sizeof(kPegBundleRegs) / sizeof(kPegBundleRegs[0]);
constexpr unsigned kLtssmReads   = 20;            // 0.5 ms apart
constexpr UInt32 kMaxWakeDumps   = 3;
constexpr UInt32 kPegId          = 0x19018086;    // Skylake/Kaby Lake PEG10 (x16)

// Chipset side of DMI (r4, read only): sideband registers of the DMI port (port ID 0xEF)
constexpr UInt64 kSbregDefault   = 0xFD000000;    // Intel reference SBREG_BAR on Sunrise Point
constexpr UInt32 kPidDmi         = 0xEF;
constexpr UInt32 kPcrDmiWindow   = 0x2000;        // map 0x2000-0x2FFF of the port
constexpr UInt32 kPcrDmiLinkCap  = 0x21A4;
constexpr UInt32 kPcrDmiLinkCtl  = 0x21A8;        // bits 15:0 link control, 31:16 link status
constexpr UInt32 kPcrDmiTcoBase  = 0x2778;
constexpr UInt8  kSmbusTcoBase   = 0x50;          // SMBus 00:1f.4: TCO base on Sunrise Point
constexpr int    kDevFnSmbus     = 0xFC;          // 00:1f.4

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

UInt32 permille(UInt64 part, UInt64 whole)
{
    return whole == 0 ? 0 : static_cast<UInt32>((part * 1000) / whole);
}

const char *aspmName(UInt32 bits)
{
    static const char *const names[] = {"off", "L0s", "L1", "L0s+L1"};
    return names[bits & 3];
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

    IOPCIDevice *pmc {nullptr};
    IOMemoryMap *pwrmMap {nullptr};
    volatile UInt32 *pwrm {nullptr};
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

    State state {kIdle};
    bool awake {true};
    bool applyPending {false};
    UInt32 applyCount {0};
    bool dumpPending {false};
    UInt32 wakeDumps {0};
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
    UInt32 limitMin {0xFF};
    UInt32 limitMax {0};
    UInt32 samples {0};

    // r4: PEG0 test phases
    IOPCIDevice *pegDevice {nullptr};
    IOMemoryMap *pegMap {nullptr};
    volatile UInt32 *pegCfg {nullptr};
    UInt32 pegPm {0};                        // PCI power management capability offset
    UInt32 pegExpress {0};                   // PCI Express capability offset
    bool pegReady {false};
    UInt32 pegApplied {0};                   // kPeg* bits that are changed right now
    UInt32 pegPmcsr {0};                     // values as found before the test
    UInt32 pegC20 {0};
    UInt32 pegC38 {0};
    UInt32 pegBundle[kPegBundleCount] {};
    char pegWhy[96] {};
    char pegNote[320] {};

    UInt32 readPwrm(UInt32 offset) const { return pwrm[offset / 4]; }
    bool tryPwrm(UInt32 base);
    void addResult(const char *text);
    void readOptions();
    void describeStart();
    void describeLtr(const char *title);
    void findAhci();
    UInt64 ecamBase(IOPCIDevice *host);
    void findBusZero(IOPCIDevice **host, IOPCIDevice **peg, IOPCIDevice **smbus);
    void describeDmi(const char *when, IOPCIDevice *host);
    void describePchDmi(const char *when, IOPCIDevice *smbus);
    void describePeg(const char *when, IOPCIDevice *host, IOPCIDevice *peg);
    void describeCpuIo(const char *when);
    bool pegHasDevice(IOPCIDevice *peg);
    void pegPrepare();
    void pegApply(UInt32 action);
    bool pegRestore();
    void pegRelease();
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
    static IOReturn powerMessage(void *target, void *refCon, UInt32 messageType,
                                 IOService *provider, void *messageArgument, vm_size_t argSize);
};

OSDefineMetaClassAndStructors(PkgCProbe, IOService);

bool PkgCProbe::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;

    pmc = OSDynamicCast(IOPCIDevice, provider);
    results = OSArray::withCapacity(16);
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
    describeCpuIo("at start");

    // Without usable PMC registers the test still runs, but only measures (nothing is written).
    if (runTest) {
        state = kWaiting;
        setProperty("State", writable ? "waiting" : "waiting (read-only)");
        timer->setTimeoutMS(startDelay * 1000);
    } else {
        state = kDone;
        setProperty("State", "no test");
        if (applyMask != 0)
            applyFix("at start");
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
    // Nothing runs on the work loop any more: put PEG0 and the ignore register back if a test was cut short.
    pegRestore();
    pegRelease();
    if (state == kRunning && ignoreChanged && writable && pwrm != nullptr) {
        pwrm[kLtrIgnore / 4] = ignoreOriginal;
        ignoreChanged = false;
    }
    state = kDone;
    if (ahciMap != nullptr) {
        ahciMap->release();
        ahciMap = nullptr;
        ahci = nullptr;
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
}

void PkgCProbe::describeStart()
{
    PcpLine line;
    UInt32 regs[4];
    pcpCpuid(1, regs);
    line.add("r4; CPUID 0x%08x; PMC config 0x48 = 0x%08x", regs[0], pwrmConfig);
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
    line.add("; StartDelay %u s, PhaseSeconds %u s, RunTest %s, ApplyMask 0x%x",
             startDelay, phaseSeconds, runTest ? "yes" : "no", applyMask);
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
// for class code 01.06.01 and map its BAR5. Only reads its registers.
void PkgCProbe::findAhci()
{
    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    OSIterator *children = bus != nullptr ? bus->getChildIterator(gIOServicePlane) : nullptr;
    if (children != nullptr) {
        OSObject *child = nullptr;
        while (ahciMap == nullptr && (child = children->getNextObject()) != nullptr) {
            IOPCIDevice *device = OSDynamicCast(IOPCIDevice, child);
            if (device != nullptr && (device->configRead32(kPciClassConfig) >> 8) == kAhciClass)
                ahciMap = device->mapDeviceMemoryWithRegister(kAhciBarConfig, kIOMapInhibitCache);
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
        UInt32 status = regs[0x28 / 4];
        if ((status & 0xF) != 3)
            continue;
        UInt32 command = regs[0x18 / 4];
        UInt32 control = regs[0x2C / 4];
        UInt32 devSleep = regs[0x44 / 4];
        if (ahciPort < 0)
            ahciPort = static_cast<int>(port);
        PcpLine line;
        line.add("AHCI port %u: PxCMD 0x%08x (ALPE %u, ASP %u, APSTE %u), PxSCTL 0x%08x (IPM %u), "
                 "PxSSTS 0x%08x (link %s), PxDEVSLP 0x%08x; CAP 0x%08x (SALP %u, partial %u, slumber %u), "
                 "CAP2 0x%08x (APST %u, DevSleep %u)",
                 port, command, (command >> 26) & 1, (command >> 27) & 1, (command >> 23) & 1,
                 control, (control >> 8) & 0xF, status, linkStateName((status >> 8) & 0xF), devSleep,
                 cap, (cap >> 26) & 1, (cap >> 13) & 1, (cap >> 14) & 1,
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

// Host bridge: which CPU-side devices are enabled, and the DMI link (to the chipset).
void PkgCProbe::describeDmi(const char *when, IOPCIDevice *host)
{
    PcpLine line;
    line.add("DMI %s:", when);
    if (host == nullptr) {
        line.add(" no PCI device 00:00.0");
        addResult(line.c_str());
        return;
    }
    UInt32 id = host->configRead32(0x00);
    UInt32 deven = host->configRead32(kHostDeven);
    UInt64 bar = (static_cast<UInt64>(host->configRead32(kHostDmibar + 4)) << 32) | host->configRead32(kHostDmibar);
    line.add(" host bridge %04x:%04x, DEVEN 0x%08x (PEG10 %u, PEG11 %u, PEG12 %u, IGPU %u, 00:04.0 %u), DMIBAR 0x%llx",
             id & 0xFFFF, id >> 16, deven, (deven >> 3) & 1, (deven >> 2) & 1, (deven >> 1) & 1,
             (deven >> 4) & 1, (deven >> 7) & 1, bar);
    UInt64 base = bar & 0x7FFFFFF000ULL;
    IOMemoryMap *mapping = nullptr;
    if ((bar & 1) != 0 && base != 0) {
        IODeviceMemory *memory = IODeviceMemory::withRange(base, kMapLength);
        if (memory != nullptr) {
            mapping = memory->map(kIOMapInhibitCache);
            memory->release();
        }
    }
    if (mapping == nullptr) {
        line.add("; DMI registers not read");
    } else {
        volatile UInt32 *dmi = reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress());
        UInt32 cap = dmi[kDmiLinkCap / 4];
        UInt32 control = dmi[kDmiLinkControl / 4];
        UInt32 lctl = control & 0xFFFF;
        UInt32 lsts = control >> 16;
        line.add("; LCAP 0x%08x (ASPM support %s, L1 exit %u), LCTL 0x%04x (ASPM %s), LSTS 0x%04x (Gen%u x%u)",
                 cap, aspmName((cap >> 10) & 3), (cap >> 15) & 7, lctl, aspmName(lctl & 3),
                 lsts, lsts & 0xF, (lsts >> 4) & 0x3F);
        mapping->release();
    }
    addResult(line.c_str());
}

// PEG0 (00:01.0): standard registers through IOPCIDevice, the rest through ECAM. Read only.
void PkgCProbe::describePeg(const char *when, IOPCIDevice *host, IOPCIDevice *peg)
{
    PcpLine line;
    line.add("PEG0 %s:", when);
    if (peg == nullptr) {
        line.add(" no PCI device 00:01.0");
        addResult(line.c_str());
        return;
    }
    UInt32 id = peg->configRead32(0x00);
    UInt32 command = peg->configRead32(0x04);
    UInt32 buses = peg->configRead32(0x18);
    line.add(" %04x:%04x, command/status 0x%08x, buses 0x%08x (secondary %u, subordinate %u)",
             id & 0xFFFF, id >> 16, command, buses, (buses >> 8) & 0xFF, (buses >> 16) & 0xFF);
    UInt8 pm = findCapability(peg, 0x01);
    if (pm != 0) {
        UInt32 pmcsr = peg->configRead32(static_cast<UInt8>(pm + 4)) & 0xFFFF;
        line.add(", PMCSR 0x%04x (D%u)", pmcsr, pmcsr & 3);
    }
    UInt8 express = findCapability(peg, 0x10);
    if (express != 0) {
        UInt32 link = peg->configRead32(static_cast<UInt8>(express + 0x10));
        UInt32 slot = peg->configRead32(static_cast<UInt8>(express + 0x18));
        UInt32 link2 = peg->configRead32(static_cast<UInt8>(express + 0x30));
        UInt32 lctl = link & 0xFFFF;
        UInt32 lsts = link >> 16;
        line.add(", LNKCTL 0x%04x (ASPM %s, link disable %u, clock PM %u), LNKSTS 0x%04x (Gen%u x%u, training %u, "
                 "DLL active %u), SLTCTL 0x%04x, SLTSTS 0x%04x (presence %u), LNKCTL2 0x%04x",
                 lctl, aspmName(lctl & 3), (lctl >> 4) & 1, (lctl >> 8) & 1,
                 lsts, lsts & 0xF, (lsts >> 4) & 0x3F, (lsts >> 11) & 1, (lsts >> 13) & 1,
                 slot & 0xFFFF, slot >> 16, (slot >> 22) & 1, link2 & 0xFFFF);
    }
    addResult(line.c_str());

    PcpLine extra;
    extra.add("PEG0 %s, extended:", when);
    UInt64 ecam = ecamBase(host);
    IOMemoryMap *mapping = nullptr;
    if (ecam != 0) {
        IODeviceMemory *memory = IODeviceMemory::withRange(ecam + kEcamPeg, kMapLength);
        if (memory != nullptr) {
            mapping = memory->map(kIOMapInhibitCache);
            memory->release();
        }
    }
    volatile UInt32 *cfg = mapping != nullptr ? reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress()) : nullptr;
    if (cfg == nullptr) {
        extra.add(" ECAM 0x%llx could not be mapped; not read", ecam);
    } else if (cfg[0] != id) {
        extra.add(" ECAM 0x%llx reads 0x%08x, not 00:01.0; not read", ecam, cfg[0]);
    } else {
        UInt32 l23 = cfg[kPegL23 / 4];
        UInt32 c20 = cfg[kPegC20 / 4];
        UInt32 c38 = cfg[kPegC38 / 4];
        extra.add(" ECAM 0x%llx; 0x248 0x%08x (bit7 %u, bit8 %u, bit14 %u), 0xC20 0x%08x (bits5:4 %u), 0xC38 0x%08x (bit3 %u)",
                  ecam, l23, (l23 >> 7) & 1, (l23 >> 8) & 1, (l23 >> 14) & 1, c20, (c20 >> 4) & 3, c38, (c38 >> 3) & 1);
        for (unsigned i = 0; i < sizeof(kPegBundleRegs) / sizeof(kPegBundleRegs[0]); i++)
            extra.add(", 0x%X 0x%08x", kPegBundleRegs[i], cfg[kPegBundleRegs[i] / 4]);
        UInt32 values[4] = {};
        UInt32 counts[4] = {};
        unsigned distinct = 0;
        unsigned other = 0;
        for (unsigned i = 0; i < kLtssmReads; i++) {
            UInt32 value = cfg[kPegLtssm / 4];
            unsigned k = 0;
            while (k < distinct && values[k] != value)
                k++;
            if (k < distinct) {
                counts[k]++;
            } else if (distinct < 4) {
                values[distinct] = value;
                counts[distinct] = 1;
                distinct++;
            } else {
                other++;
            }
            IODelay(500);
        }
        extra.add(", 0xC74");
        for (unsigned k = 0; k < distinct; k++)
            extra.add(" 0x%08x x%u", values[k], counts[k]);
        if (other != 0)
            extra.add(" +%u other", other);
        extra.add(" (%u reads)", kLtssmReads);
    }
    if (mapping != nullptr)
        mapping->release();
    addResult(extra.c_str());
}

// Host bridge 00:00.0, PEG0 00:01.0 and SMBus 00:1f.4 among the PMC's siblings on bus 0 (retained).
void PkgCProbe::findBusZero(IOPCIDevice **host, IOPCIDevice **peg, IOPCIDevice **smbus)
{
    IOService *bus = pmc != nullptr ? pmc->getProvider() : nullptr;
    OSIterator *children = bus != nullptr ? bus->getChildIterator(gIOServicePlane) : nullptr;
    if (children == nullptr)
        return;
    OSObject *child = nullptr;
    while ((child = children->getNextObject()) != nullptr) {
        IOPCIDevice *device = OSDynamicCast(IOPCIDevice, child);
        if (device == nullptr)
            continue;
        int devfn = pciDevFn(device);
        IOPCIDevice **slot = nullptr;
        if (devfn == 0x00)
            slot = host;
        else if (devfn == 0x08)
            slot = peg;
        else if (devfn == kDevFnSmbus)
            slot = smbus;
        if (slot != nullptr && *slot == nullptr) {
            *slot = device;
            device->retain();
        }
    }
    children->release();
}

// Chipset side of DMI, read through the sideband window. Trusted only if the TCO base there
// matches the one the SMBus controller reports.
void PkgCProbe::describePchDmi(const char *when, IOPCIDevice *smbus)
{
    PcpLine line;
    line.add("PCH DMI %s:", when);
    UInt32 smbusTco = smbus != nullptr ? (smbus->configRead32(kSmbusTcoBase) & 0xFFE0) : 0;
    IOMemoryMap *mapping = nullptr;
    IODeviceMemory *memory = IODeviceMemory::withRange(kSbregDefault + (static_cast<UInt64>(kPidDmi) << 16) + kPcrDmiWindow,
                                                       kMapLength);
    if (memory != nullptr) {
        mapping = memory->map(kIOMapInhibitCache);
        memory->release();
    }
    if (mapping == nullptr) {
        line.add(" sideband window 0x%llx not mapped; not read", kSbregDefault);
        addResult(line.c_str());
        return;
    }
    volatile UInt32 *pcr = reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress());
    UInt32 tco = pcr[(kPcrDmiTcoBase - kPcrDmiWindow) / 4] & 0xFFE0;
    UInt32 cap = pcr[(kPcrDmiLinkCap - kPcrDmiWindow) / 4];
    UInt32 control = pcr[(kPcrDmiLinkCtl - kPcrDmiWindow) / 4];
    mapping->release();
    bool verified = smbusTco != 0 && tco == smbusTco;
    UInt32 lctl = control & 0xFFFF;
    UInt32 lsts = control >> 16;
    line.add(" SBREG 0x%llx, TCO base 0x%04x, SMBus TCO base 0x%04x (%s); LCAP 0x%08x (ASPM support %s, L0s exit %u, "
             "L1 exit %u), LCTL 0x%04x (ASPM %s), LSTS 0x%04x (Gen%u x%u)",
             kSbregDefault, tco, smbusTco, verified ? "match" : "NO MATCH: the values may not be the DMI port",
             cap, aspmName((cap >> 10) & 3), (cap >> 12) & 7, (cap >> 15) & 7, lctl, aspmName(lctl & 3),
             lsts, lsts & 0xF, (lsts >> 4) & 0x3F);
    addResult(line.c_str());
}

void PkgCProbe::describeCpuIo(const char *when)
{
    IOPCIDevice *host = nullptr;
    IOPCIDevice *peg = nullptr;
    IOPCIDevice *smbus = nullptr;
    findBusZero(&host, &peg, &smbus);
    describeDmi(when, host);
    describePchDmi(when, smbus);
    describePeg(when, host, peg);
    if (host != nullptr)
        host->release();
    if (peg != nullptr)
        peg->release();
    if (smbus != nullptr)
        smbus->release();
}

// True if any PCI device is registered behind PEG0 (two levels down: bridge driver, then devices).
bool PkgCProbe::pegHasDevice(IOPCIDevice *peg)
{
    bool found = false;
    OSIterator *level1 = peg->getChildIterator(gIOServicePlane);
    if (level1 == nullptr)
        return true;                         // cannot tell: treat as "has a device"
    OSObject *a = nullptr;
    while (!found && (a = level1->getNextObject()) != nullptr) {
        if (OSDynamicCast(IOPCIDevice, a) != nullptr) {
            found = true;
            break;
        }
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, a);
        OSIterator *level2 = entry != nullptr ? entry->getChildIterator(gIOServicePlane) : nullptr;
        if (level2 == nullptr)
            continue;
        OSObject *b = nullptr;
        while ((b = level2->getNextObject()) != nullptr) {
            if (OSDynamicCast(IOPCIDevice, b) != nullptr) {
                found = true;
                break;
            }
        }
        level2->release();
    }
    level1->release();
    return found;
}

// Before the test: decide whether the PEG0 phases may write, and keep the values to restore.
void PkgCProbe::pegPrepare()
{
    pegReady = false;
    pegApplied = 0;
    pegWhy[0] = '\0';
    IOPCIDevice *host = nullptr;
    IOPCIDevice *peg = nullptr;
    findBusZero(&host, &peg, nullptr);
    const char *why = nullptr;
    UInt32 id = peg != nullptr ? peg->configRead32(0x00) : 0;
    if (!writable)
        why = "read-only mode";
    else if (!msrUsable)
        why = "not a Skylake/Kaby Lake CPU";
    else if (peg == nullptr)
        why = "no PCI device 00:01.0";
    else if (id != kPegId)
        why = "00:01.0 is not 8086:1901";
    else if (pegHasDevice(peg))
        why = "a device is attached behind PEG0";

    IOMemoryMap *mapping = nullptr;
    volatile UInt32 *cfg = nullptr;
    if (why == nullptr) {
        UInt64 ecam = ecamBase(host);
        IODeviceMemory *memory = ecam != 0 ? IODeviceMemory::withRange(ecam + kEcamPeg, kMapLength) : nullptr;
        if (memory != nullptr) {
            mapping = memory->map(kIOMapInhibitCache);
            memory->release();
        }
        cfg = mapping != nullptr ? reinterpret_cast<volatile UInt32 *>(mapping->getVirtualAddress()) : nullptr;
        if (cfg == nullptr)
            why = "ECAM not mapped";
        else if (cfg[0] != id)
            why = "ECAM does not show 00:01.0";
    }
    UInt32 pm = 0;
    UInt32 express = 0;
    if (why == nullptr) {
        pm = findCapability(peg, 0x01);
        express = findCapability(peg, 0x10);
        if (pm == 0 || express == 0) {
            why = "PEG0 capabilities not found";
        } else {
            UInt32 pmcsr = cfg[(pm + 4) / 4];
            UInt32 lsts = cfg[(express + 0x10) / 4] >> 16;
            if ((pmcsr & 3) != 0)
                why = "PEG0 is not in D0";
            else if (((lsts >> 13) & 1) != 0)
                why = "PEG0 link is up";
            else if (((cfg[kPegL23 / 4] >> 14) & 1) == 0)
                why = "PEG0 L2/L3 handshake not done (0x248 bit 14 is 0)";
        }
    }
    if (host != nullptr)
        host->release();
    if (why != nullptr) {
        snprintf(pegWhy, sizeof(pegWhy), "%s", why);
        if (mapping != nullptr)
            mapping->release();
        if (peg != nullptr)
            peg->release();
        PcpLine line;
        line.add("PEG0 phases: nothing will be written (%s)", why);
        addResult(line.c_str());
        return;
    }

    pegDevice = peg;                         // keeps the retain from findBusZero
    pegMap = mapping;
    pegCfg = cfg;
    pegPm = pm;
    pegExpress = express;
    pegPmcsr = cfg[(pm + 4) / 4];
    pegC20 = cfg[kPegC20 / 4];
    pegC38 = cfg[kPegC38 / 4];
    for (unsigned i = 0; i < kPegBundleCount; i++)
        pegBundle[i] = cfg[kPegBundleRegs[i] / 4];
    pegReady = true;
    PcpLine line;
    line.add("PEG0 phases: ready; will restore PMCSR 0x%04x, 0xC20 0x%08x, 0xC38 0x%08x, bundles",
             pegPmcsr & 0xFFFF, pegC20, pegC38);
    for (unsigned i = 0; i < kPegBundleCount; i++)
        line.add(" 0x%08x", pegBundle[i]);
    addResult(line.c_str());
}

// Changes PEG0 for one phase. The order follows the firmware's PGOF: lanes, bundles, then D3hot.
void PkgCProbe::pegApply(UInt32 action)
{
    pegNote[0] = '\0';
    if (action == kPegNone)
        return;
    if (!pegReady || pegCfg == nullptr) {
        snprintf(pegNote, sizeof(pegNote), "PEG0 not changed: %s", pegWhy[0] != '\0' ? pegWhy : "not prepared");
        return;
    }
    if ((action & kPegLanes) != 0) {
        pegApplied |= kPegLanes;
        pegCfg[kPegC38 / 4] = pegC38 | 0x8;
        pegCfg[kPegC20 / 4] = (pegC20 & ~0x30u) | 0x30;
    }
    if ((action & kPegBundles) != 0) {
        pegApplied |= kPegBundles;
        for (unsigned i = 0; i < kPegBundleCount; i++)
            pegCfg[kPegBundleRegs[i] / 4] = pegBundle[i] | 0x80000000u;
    }
    if ((action & kPegD3) != 0) {
        pegApplied |= kPegD3;
        pegCfg[(pegPm + 4) / 4] = (pegPmcsr & ~0x8003u) | 3;     // D3hot; bit 15 (PME status) is never written as 1
        IOSleep(10);
    }
    unsigned gated = 0;
    for (unsigned i = 0; i < kPegBundleCount; i++)
        gated += (pegCfg[kPegBundleRegs[i] / 4] >> 31) & 1;
    snprintf(pegNote, sizeof(pegNote), "PEG0 set: PMCSR 0x%04x, 0xC20 0x%08x, 0xC38 0x%08x, bundles gated %u/%u, LNKSTS 0x%04x",
             pegCfg[(pegPm + 4) / 4] & 0xFFFF, pegCfg[kPegC20 / 4], pegCfg[kPegC38 / 4], gated, kPegBundleCount,
             pegCfg[(pegExpress + 0x10) / 4] >> 16);
}

// Puts back what pegApply changed: D0 first, then lanes, then bundles (as the firmware's PGON).
bool PkgCProbe::pegRestore()
{
    if (pegApplied == 0 || pegCfg == nullptr)
        return false;
    if ((pegApplied & kPegD3) != 0) {
        pegCfg[(pegPm + 4) / 4] = pegPmcsr & ~0x8000u;
        IOSleep(10);
    }
    if ((pegApplied & kPegLanes) != 0) {
        pegCfg[kPegC20 / 4] = pegC20;
        pegCfg[kPegC38 / 4] = pegC38;
    }
    if ((pegApplied & kPegBundles) != 0) {
        for (unsigned i = 0; i < kPegBundleCount; i++)
            pegCfg[kPegBundleRegs[i] / 4] = pegBundle[i];
    }
    pegApplied = 0;
    bool same = (pegCfg[(pegPm + 4) / 4] & 3) == (pegPmcsr & 3) && pegCfg[kPegC20 / 4] == pegC20 &&
                pegCfg[kPegC38 / 4] == pegC38;
    for (unsigned i = 0; i < kPegBundleCount; i++)
        same = same && pegCfg[kPegBundleRegs[i] / 4] == pegBundle[i];
    size_t used = strlen(pegNote);
    if (used < sizeof(pegNote))
        snprintf(pegNote + used, sizeof(pegNote) - used, "%sPEG0 %s (0x248 0x%08x, 0xC74 0x%08x)",
                 used != 0 ? "; " : "", same ? "restored" : "RESTORE MISMATCH", pegCfg[kPegL23 / 4], pegCfg[kPegLtssm / 4]);
    return true;
}

void PkgCProbe::pegRelease()
{
    pegReady = false;
    pegCfg = nullptr;
    if (pegMap != nullptr) {
        pegMap->release();
        pegMap = nullptr;
    }
    if (pegDevice != nullptr) {
        pegDevice->release();
        pegDevice = nullptr;
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
    }

    if (ahci != nullptr && ahciPort >= 0) {
        UInt32 ipm = (ahci[(0x100 + ahciPort * 0x80 + 0x28) / 4] >> 8) & 0xF;
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
        line.add("P%u %s (read-only, not applied):", phase, current.name);
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
    }
    if (ahciPort >= 0)
        line.add(" | SATA link active %u, partial %u, slumber %u, devsleep %u, other %u",
                 linkCount[0], linkCount[1], linkCount[2], linkCount[3], linkCount[4]);
    if (msrUsable && samples > 0) {
        line.add(" | limit %s", limitName(limitMin));
        if (limitMax != limitMin)
            line.add(" to %s", limitName(limitMax));
    }
    line.add(" | bus delay ");
    addBusDelay(line);
    if (current.peg != kPegNone && pegNote[0] != '\0')
        line.add(" | %s", pegNote);
    addResult(line.c_str());
}

void PkgCProbe::beginTest()
{
    findAhci();
    describeLtr("LTR before the test:");
    pegPrepare();
    state = kRunning;
    phase = 0;
    tick = 0;
    setProperty("State", "running");
    stepTest();
}

// One step every 500 ms: write the ignore register, wait 2 s, then sample for PhaseSeconds.
void PkgCProbe::stepTest()
{
    const UInt32 measureTicks = phaseSeconds * 1000 / kTickMS;
    const UInt32 ignored = ignoreOriginal | kPhases[phase].mask;

    if (tick == 0) {
        writeIgnore(ignored);
        pegApply(kPhases[phase].peg);
    } else if (tick == kSettleTicks) {
        takeSnapshot(&first);
        resetSamples();
    } else if (tick > kSettleTicks) {
        sample(ignored);
        if (tick >= kSettleTicks + measureTicks) {
            PcpSnapshot last;
            takeSnapshot(&last);
            pegRestore();
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
    if (pegRestore())
        addResult(pegNote);
    pegRelease();
    if (ignoreChanged)
        writeIgnore(ignoreOriginal);
    state = kDone;
    if (ahciMap != nullptr) {
        ahciMap->release();
        ahciMap = nullptr;
        ahci = nullptr;
    }
    PcpLine line;
    line.add("%s; LTR_IGN is 0x%08x", why, pwrm != nullptr ? readPwrm(kLtrIgnore) : 0);
    addResult(line.c_str());
    if (awake)
        describeCpuIo("after the test");
    setProperty("State", why);
    if (applyMask != 0 && awake)
        applyFix("after the test");
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
        if (dumpPending) {
            dumpPending = false;
            wakeDumps++;
            describeCpuIo("after wake");
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
        self->dumpPending = false;
        self->timer->cancelTimeout();
        if (self->state == kRunning) {
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
        if (self->state == kDone && self->wakeDumps < kMaxWakeDumps)
            self->dumpPending = true;
        if (self->applyPending || self->dumpPending)
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
