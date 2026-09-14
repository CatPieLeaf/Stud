#include "stud/cpu_compat.h"
#include "stud/x86_emulator.h"

#include <cpuid.h>
#include <signal.h>
#include <ucontext.h>

#include <cstdio>
#include <cstdlib>

namespace stud::cpu_compat {

CpuFeatures detect_cpu_features() {
    CpuFeatures features;
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0) {
        return features;  // CPUID leaf 1 unavailable, report nothing supported
    }
    // Standard, documented Intel/AMD CPUID leaf 1 ECX bit positions.
    features.ssse3 = (ecx & (1u << 9)) != 0;
    features.sse4_1 = (ecx & (1u << 19)) != 0;
    features.sse4_2 = (ecx & (1u << 20)) != 0;
    features.popcnt = (ecx & (1u << 23)) != 0;
    return features;
}

bool meets_minimum_requirements(const CpuFeatures& features) { return features.sse4_1; }

namespace {

void sigill_handler(int /*signum*/, siginfo_t* /*info*/, void* ucontext_raw) {
    auto* uctx = static_cast<ucontext_t*>(ucontext_raw);
    greg_t* gregs = uctx->uc_mcontext.gregs;
    auto* rip = reinterpret_cast<const uint8_t*>(gregs[REG_RIP]);

    DecodedPopcnt decoded = try_decode_popcnt(rip);
    if (decoded.length > 0) {
        emulate_popcnt(decoded, gregs);
        gregs[REG_RIP] += decoded.length;
        return;
    }

    DecodedBmi1 bmi1 = try_decode_bmi1(rip);
    if (bmi1.length > 0) {
        emulate_bmi1(bmi1, gregs);
        gregs[REG_RIP] += bmi1.length;
        return;
    }

    DecodedMovbe movbe = try_decode_movbe(rip);
    if (movbe.length > 0) {
        // RIP-relative addressing is measured from the end of the
        // instruction, so the emulator needs to know where that is.
        emulate_movbe(movbe, gregs,
                      static_cast<uint64_t>(gregs[REG_RIP]) + static_cast<uint64_t>(movbe.length));
        gregs[REG_RIP] += movbe.length;
        return;
    }

    std::fprintf(stderr,
                  "stud: SIGILL at %p, instruction not recognized or not yet "
                  "emulated (bytes: %02x %02x %02x %02x). This CPU is missing an "
                  "instruction Stud doesn't emulate yet.\n",
                  static_cast<const void*>(rip), rip[0], rip[1], rip[2], rip[3]);
    std::abort();
}

}  // namespace

bool install_sigill_handler() {
    struct sigaction sa {};
    sa.sa_sigaction = sigill_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    return sigaction(SIGILL, &sa, nullptr) == 0;
}

void uninstall_sigill_handler() { signal(SIGILL, SIG_DFL); }

}  // namespace stud::cpu_compat
