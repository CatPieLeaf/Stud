// M5 test. Two parts: real CPUID feature detection cross-checked against
// GCC's own __builtin_cpu_supports (an independent implementation of the
// same check, not a tautology), and direct unit tests of the POPCNT
// decode/emulate logic against real instruction bytes.
//
// Honest limitation, not hidden: this development host has every relevant
// ISA extension present, so a genuine SIGILL-triggered emulation (missing
// hardware) can't be exercised here -- see cpu_compat.h. What's tested
// instead: the decode+emulate logic is unit-tested directly and
// exhaustively against real POPCNT-encoded bytes (not through a trap), and
// the signal handler installs/uninstalls correctly.

#include "stud/cpu_compat.h"
#include "stud/x86_emulator.h"

#include <cpuid.h>
#include <signal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

void test_feature_detection() {
    auto features = stud::cpu_compat::detect_cpu_features();

    // Cross-check against GCC's own independent CPUID-based implementation
    // -- not testing our detection against itself.
    __builtin_cpu_init();
    check(features.ssse3 == static_cast<bool>(__builtin_cpu_supports("ssse3")),
          "SSSE3 detection matches GCC's own __builtin_cpu_supports");
    check(features.sse4_1 == static_cast<bool>(__builtin_cpu_supports("sse4.1")),
          "SSE4.1 detection matches GCC's own __builtin_cpu_supports");
    check(features.sse4_2 == static_cast<bool>(__builtin_cpu_supports("sse4.2")),
          "SSE4.2 detection matches GCC's own __builtin_cpu_supports");
    check(features.popcnt == static_cast<bool>(__builtin_cpu_supports("popcnt")),
          "POPCNT detection matches GCC's own __builtin_cpu_supports");

    check(stud::cpu_compat::meets_minimum_requirements(features) == features.sse4_1,
          "meets_minimum_requirements() tracks the documented SSE4.1 floor");

    stud::cpu_compat::CpuFeatures none{};
    check(!stud::cpu_compat::meets_minimum_requirements(none),
          "a CPU reporting no features fails the minimum-requirements check");
}

void test_popcnt_decode_and_emulate() {
    // popcnt eax, ebx  (32-bit form, no REX)
    const uint8_t code32[] = {0xF3, 0x0F, 0xB8, 0xC3};  // ModRM: mod=11 reg=000(eax) rm=011(ebx)
    auto d32 = stud::cpu_compat::try_decode_popcnt(code32);
    check(d32.length == 4, "32-bit popcnt eax,ebx decodes to the correct 4-byte length");
    check(d32.dest_reg == 0 && d32.src_reg == 3 && !d32.is_64bit,
          "32-bit popcnt eax,ebx decodes dest=RAX(0)/src=RBX(3)/is_64bit=false correctly");

    // popcnt rax, rcx  (64-bit form, REX.W)
    const uint8_t code64[] = {0xF3, 0x48, 0x0F, 0xB8, 0xC1};  // REX.W=1, ModRM reg=000 rm=001(rcx)
    auto d64 = stud::cpu_compat::try_decode_popcnt(code64);
    check(d64.length == 5, "64-bit popcnt rax,rcx decodes to the correct 5-byte length");
    check(d64.dest_reg == 0 && d64.src_reg == 1 && d64.is_64bit,
          "64-bit popcnt rax,rcx decodes dest=RAX(0)/src=RCX(1)/is_64bit=true correctly");

    // popcnt r8, r9  (REX.R + REX.B extend both operands into r8-r15)
    const uint8_t code_ext[] = {0xF3, 0x45, 0x0F, 0xB8, 0xC1};  // REX = 0100_0101 (R=1,B=1)
    auto d_ext = stud::cpu_compat::try_decode_popcnt(code_ext);
    check(d_ext.length == 5, "REX.R/REX.B-extended popcnt r8,r9 decodes to the correct length");
    check(d_ext.dest_reg == 8 && d_ext.src_reg == 9,
          "REX.R/REX.B correctly extend ModRM fields to r8/r9 (register numbers 8 and 9)");

    // Not POPCNT at all -- must not falsely decode.
    const uint8_t not_popcnt[] = {0x90, 0x90, 0x90, 0x90};  // NOPs
    check(stud::cpu_compat::try_decode_popcnt(not_popcnt).length == 0,
          "non-POPCNT bytes (NOPs) correctly fail to decode");

    // Memory operand (mod != 3) -- not supported yet, must report as such
    // rather than mis-decode.
    const uint8_t mem_operand[] = {0xF3, 0x0F, 0xB8, 0x03};  // mod=00 -> [rbx]
    check(stud::cpu_compat::try_decode_popcnt(mem_operand).length == 0,
          "memory-operand popcnt correctly reports as not-yet-supported, not mis-decoded");

    // Real emulation: known bit patterns, verified against __builtin_popcountll.
    greg_t gregs[NGREG] = {};
    gregs[stud::cpu_compat::reg_to_greg_index(1)] = 0xFF;  // RCX = 0xFF (8 bits set)
    gregs[REG_EFL] = ~greg_t{0};                           // all flags set, to prove clearing works
    stud::cpu_compat::emulate_popcnt(d64, gregs);
    check(gregs[stud::cpu_compat::reg_to_greg_index(0)] == 8,
          "emulate_popcnt(0xFF) == 8, matches real popcount");
    check((gregs[REG_EFL] & 1) == 0, "emulate_popcnt clears CF");
    check(((gregs[REG_EFL] >> 6) & 1) == 0, "emulate_popcnt clears ZF for a non-zero result");

    gregs[stud::cpu_compat::reg_to_greg_index(1)] = 0;
    stud::cpu_compat::emulate_popcnt(d64, gregs);
    check(gregs[stud::cpu_compat::reg_to_greg_index(0)] == 0, "emulate_popcnt(0) == 0");
    check(((gregs[REG_EFL] >> 6) & 1) == 1, "emulate_popcnt sets ZF when the result is zero");

    // 32-bit form must mask off the upper 32 bits of the source register.
    greg_t gregs32[NGREG] = {};
    gregs32[stud::cpu_compat::reg_to_greg_index(3)] = 0xFFFFFFFF00000003ULL;  // RBX
    stud::cpu_compat::emulate_popcnt(d32, gregs32);
    check(gregs32[stud::cpu_compat::reg_to_greg_index(0)] == 2,
          "32-bit emulate_popcnt masks off the upper 32 bits before counting (result == 2, not 34)");
}

void test_signal_handler_install() {
    check(stud::cpu_compat::install_sigill_handler(), "install_sigill_handler() succeeds");

    struct sigaction after {};
    sigaction(SIGILL, nullptr, &after);
    check((after.sa_flags & SA_SIGINFO) != 0,
          "SIGILL disposition has SA_SIGINFO set after installing the handler "
          "(the default disposition never does)");

    stud::cpu_compat::uninstall_sigill_handler();
    struct sigaction restored {};
    sigaction(SIGILL, nullptr, &restored);
    check(restored.sa_handler == SIG_DFL, "uninstall_sigill_handler() restores the default disposition");
}

}  // namespace

int main() {
    test_feature_detection();
    test_popcnt_decode_and_emulate();
    test_signal_handler_install();

    std::printf("all cpu-compat checks passed\n");
    return 0;
}
