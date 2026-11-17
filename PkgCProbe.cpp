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
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
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
constexpr UInt32 kIgnoreXHCI = 1u << 4;
constexpr UInt32 kIgnoreAll  = 0x0003FBDF;   // bits 0-4, 6-9 and 11-17; 5 and 10 are reserved

struct Phase {
    UInt32 mask;
    const char *name;
};

const Phase kPhases[] = {
    {0,                         "baseline"},
    {kIgnoreSATA,               "ignore SATA"},
    {kIgnoreXHCI,               "ignore XHCI"},
    {kIgnoreSATA | kIgnoreXHCI, "ignore SATA+XHCI"},
    {kIgnoreAll,                "ignore all"},
    {0,                         "baseline again"},
};
constexpr unsigned kPhaseCount = sizeof(kPhases) / sizeof(kPhases[0]);

constexpr UInt32 kTickMS       = 500;
constexpr UInt32 kSettleTicks  = 4;          // 2 s between changing the ignore register and measuring
constexpr UInt32 kWakeApplyMS  = 1000;
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
    char text[768];
    size_t length {0};
};

struct PcpSnapshot {
    UInt64 ns;
    UInt64 tsc;
    UInt64 pc2;
    UInt64 pc3;
    UInt64 pc6;
    UInt64 pc7;
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

    UInt32 readPwrm(UInt32 offset) const { return pwrm[offset / 4]; }
    bool tryPwrm(UInt32 base);
    void addResult(const char *text);
    void readOptions();
    void describeStart();
    void describeLtr(const char *title);
    void findAhci();
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
    // Nothing runs on the work loop any more: put the ignore register back if a test was cut short.
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
    IOMemoryMap *map = memory->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (map == nullptr)
        return false;

    volatile UInt32 *regs = reinterpret_cast<volatile UInt32 *>(map->getVirtualAddress());
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
        map->release();
        return false;
    }

    pwrmMap = map;
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
    line.add("CPUID 0x%08x; PMC config 0x48 = 0x%08x", regs[0], pwrmConfig);
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

void PkgCProbe::findAhci()
{
    OSDictionary *matching = IOService::serviceMatching("IOPCIDevice");
    if (matching == nullptr)
        return;
    OSString *classMatch = OSString::withCString("0x01060100&0xffffff00");   // SATA, AHCI
    if (classMatch != nullptr) {
        matching->setObject("IOPCIClassMatch", classMatch);
        classMatch->release();
    }
    IOService *service = IOService::copyMatchingService(matching);
    matching->release();

    IOPCIDevice *sata = OSDynamicCast(IOPCIDevice, service);
    if (sata != nullptr)
        ahciMap = sata->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5, kIOMapInhibitCache);
    if (service != nullptr)
        service->release();
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
        snapshot->busy = pcpReadMsr(kMsrPkgAnyCoreC0);
        snapshot->pkgEnergy = static_cast<UInt32>(pcpReadMsr(kMsrPkgEnergy));
        snapshot->dramEnergy = static_cast<UInt32>(pcpReadMsr(kMsrDramEnergy));
    } else {
        snapshot->pc2 = snapshot->pc3 = snapshot->pc6 = snapshot->pc7 = snapshot->busy = 0;
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
    addResult(line.c_str());
}

void PkgCProbe::beginTest()
{
    findAhci();
    describeLtr("LTR before the test:");
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
            self->finishTest("stopped: the Mac went to sleep during the test");
        } else if (self->state == kWaiting) {
            self->state = kDone;
            self->addResult("skipped: the Mac went to sleep before the test started");
            self->setProperty("State", "skipped");
        }
    } else {
        self->awake = true;
        if (self->state == kDone && self->applyMask != 0 && self->writable) {
            self->applyPending = true;
            self->timer->setTimeoutMS(kWakeApplyMS);
        }
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
