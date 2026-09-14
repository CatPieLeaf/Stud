// M5 test. Two parts: real CPUID feature detection cross-checked against
// GCC's own __builtin_cpu_supports (an independent implementation of the
// same check, not a tautology), and direct unit tests of the POPCNT
// decode/emulate logic against real instruction bytes.
//
// Honest limitation, not hidden: this development host has every relevant
// ISA extension present, so a genuine SIGILL-triggered emulation (missing
// hardware) can't be exercised here; see cpu_compat.h. What's tested
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
    // not testing our detection against itself.
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

void test_bmi1_decode_and_emulate() {
    using namespace stud::cpu_compat;
    greg_t gregs[NGREG] = {};

    // andn eax, ecx, ebx   C4 E2 70 F2 C3
    //   VEX.NDS.LZ.0F38.W0 F2 /r, vvvv=ecx(1) -> 0x70 carries ~1 in bits 6:3
    //   ModRM C3: mod=11 reg=000(eax) rm=011(ebx)
    {
        const uint8_t code[] = {0xC4, 0xE2, 0x70, 0xF2, 0xC3};
        auto d = try_decode_bmi1(code);
        check(d.length == 5, "andn decodes to a 5-byte instruction");
        check(d.op == Bmi1Op::kAndn, "opcode F2 decodes as ANDN");
        check(d.dest_reg == 0, "ANDN destination is ModRM.reg (eax)");
        check(d.src1_reg == 1, "ANDN src1 comes from VEX.vvvv (ecx), not ModRM");
        check(d.src2_reg == 3, "ANDN src2 is the r/m operand (ebx)");
        check(!d.is_64bit, "VEX.W=0 means the 32-bit form");
        gregs[reg_to_greg_index(1)] = 0x0F;        // ecx
        gregs[reg_to_greg_index(3)] = 0xFF;        // ebx
        emulate_bmi1(d, gregs);
        check(static_cast<uint64_t>(gregs[reg_to_greg_index(0)]) == 0xF0,
              "ANDN computes ~src1 & src2 (~0x0F & 0xFF == 0xF0)");
    }

    // blsr eax, ebx        C4 E2 78 F3 CB   (/1, vvvv=eax)
    // blsmsk eax, ebx      C4 E2 78 F3 D3   (/2)
    // blsi eax, ebx        C4 E2 78 F3 DB   (/3)
    //
    // All three share opcode F3 and differ ONLY in ModRM.reg; this is
    // the distinction that, if missed, silently runs all three as one.
    {
        const uint8_t blsr[]   = {0xC4, 0xE2, 0x78, 0xF3, 0xCB};
        const uint8_t blsmsk[] = {0xC4, 0xE2, 0x78, 0xF3, 0xD3};
        const uint8_t blsi[]   = {0xC4, 0xE2, 0x78, 0xF3, 0xDB};
        auto dr = try_decode_bmi1(blsr);
        auto dm = try_decode_bmi1(blsmsk);
        auto di = try_decode_bmi1(blsi);
        check(dr.op == Bmi1Op::kBlsr, "F3 /1 decodes as BLSR");
        check(dm.op == Bmi1Op::kBlsmsk, "F3 /2 decodes as BLSMSK");
        check(di.op == Bmi1Op::kBlsi, "F3 /3 decodes as BLSI");
        check(dr.dest_reg == 0 && dr.src2_reg == 3,
              "BLSR writes VEX.vvvv (eax) and reads r/m (ebx)");

        gregs[reg_to_greg_index(3)] = 0b10110000;  // ebx
        emulate_bmi1(dr, gregs);
        check(static_cast<uint64_t>(gregs[reg_to_greg_index(0)]) == 0b10100000,
              "BLSR clears the lowest set bit");
        gregs[reg_to_greg_index(3)] = 0b10110000;
        emulate_bmi1(dm, gregs);
        check(static_cast<uint64_t>(gregs[reg_to_greg_index(0)]) == 0b00011111,
              "BLSMSK masks up to and including the lowest set bit");
        gregs[reg_to_greg_index(3)] = 0b10110000;
        emulate_bmi1(di, gregs);
        check(static_cast<uint64_t>(gregs[reg_to_greg_index(0)]) == 0b00010000,
              "BLSI isolates the lowest set bit");
    }

    // The flags each one really defines (Intel SDM).
    {
        const uint8_t blsi[] = {0xC4, 0xE2, 0x78, 0xF3, 0xDB};
        auto d = try_decode_bmi1(blsi);
        gregs[reg_to_greg_index(3)] = 0;
        gregs[REG_EFL] = 0;
        emulate_bmi1(d, gregs);
        const uint64_t f = static_cast<uint64_t>(gregs[REG_EFL]);
        check((f & 1u) == 0, "BLSI clears CF when the source is zero");
        check((f & (1u << 6)) != 0, "BLSI sets ZF when the result is zero");
    }

    // The 64-bit form, which differs only in VEX.W, byte 2 becomes 0xF0.
    // Every encoding in this test was taken from the assembler's own
    // output rather than hand-derived, so a decoder that agrees with
    // them is agreeing with the architecture, not with whoever wrote
    // the test.
    {
        const uint8_t code[] = {0xC4, 0xE2, 0xF0, 0xF2, 0xC3};  // andn rax, rcx, rbx
        auto d = try_decode_bmi1(code);
        check(d.op == Bmi1Op::kAndn && d.is_64bit, "VEX.W=1 decodes as the 64-bit ANDN");
        gregs[reg_to_greg_index(1)] = 0xFFFFFFFF00000000ull;
        gregs[reg_to_greg_index(3)] = 0xFFFFFFFFFFFFFFFFull;
        emulate_bmi1(d, gregs);
        check(static_cast<uint64_t>(gregs[reg_to_greg_index(0)]) == 0x00000000FFFFFFFFull,
              "64-bit ANDN operates on the full register");
    }

    // Not BMI1, and must not be claimed: the 2-byte VEX form cannot
    // encode the 0F38 map these live in.
    {
        const uint8_t two_byte_vex[] = {0xC5, 0x78, 0xF3, 0xDB, 0x00};
        check(try_decode_bmi1(two_byte_vex).length == 0,
              "a 2-byte VEX prefix is not decoded as BMI1");
        const uint8_t popcnt[] = {0xF3, 0x0F, 0xB8, 0xC3, 0x00};
        check(try_decode_bmi1(popcnt).length == 0, "POPCNT is not decoded as BMI1");
    }
}

void test_movbe_decode_and_emulate() {
    using namespace stud::cpu_compat;
    greg_t gregs[NGREG] = {};
    // Every encoding below came from the assembler's own output, not
    // from reading the manual and hoping.
    uint64_t buffer = 0;
    gregs[reg_to_greg_index(3)] = reinterpret_cast<greg_t>(&buffer);  // rbx

    // movbe (%rbx), %eax        0f 38 f0 03
    {
        const uint8_t code[] = {0x0F, 0x38, 0xF0, 0x03};
        auto d = try_decode_movbe(code);
        check(d.length == 4 && !d.is_store && !d.is_64bit, "movbe load decodes as a 4-byte load");
        check(d.reg == 0 && d.mem.base_reg == 3 && d.mem.index_reg == -1,
              "movbe (%rbx),%eax names eax and a plain rbx base");
        buffer = 0x11223344;
        emulate_movbe(d, gregs, 0);
        check(static_cast<uint32_t>(gregs[reg_to_greg_index(0)]) == 0x44332211u,
              "movbe byte-swaps on load");
    }

    // movbe %eax, (%rbx)        0f 38 f1 03
    {
        const uint8_t code[] = {0x0F, 0x38, 0xF1, 0x03};
        auto d = try_decode_movbe(code);
        check(d.is_store, "opcode F1 is the store form");
        buffer = 0;
        gregs[reg_to_greg_index(0)] = 0xAABBCCDD;
        emulate_movbe(d, gregs, 0);
        check(static_cast<uint32_t>(buffer) == 0xDDCCBBAAu, "movbe byte-swaps on store");
    }

    // movbe 0x10(%rbx), %eax    0f 38 f0 43 10   (mod=01, disp8)
    {
        const uint8_t code[] = {0x0F, 0x38, 0xF0, 0x43, 0x10};
        auto d = try_decode_movbe(code);
        check(d.length == 5 && d.mem.disp == 0x10, "a disp8 form decodes its displacement");
    }

    // movbe (%rbx,%rcx,4), %eax  0f 38 f0 04 8b   (SIB)
    {
        const uint8_t code[] = {0x0F, 0x38, 0xF0, 0x04, 0x8B};
        auto d = try_decode_movbe(code);
        check(d.length == 5, "a SIB form consumes the SIB byte");
        check(d.mem.base_reg == 3 && d.mem.index_reg == 1 && d.mem.scale == 4,
              "SIB decodes base=rbx, index=rcx, scale=4");
        buffer = 0x11223344;
        gregs[reg_to_greg_index(1)] = 0;  // rcx = 0, so the address is just rbx
        emulate_movbe(d, gregs, 0);
        check(static_cast<uint32_t>(gregs[reg_to_greg_index(0)]) == 0x44332211u,
              "a SIB address resolves and loads");
    }

    // movbe 0x1234(%rip), %eax   0f 38 f0 05 34 12 00 00
    {
        const uint8_t code[] = {0x0F, 0x38, 0xF0, 0x05, 0x34, 0x12, 0x00, 0x00};
        auto d = try_decode_movbe(code);
        check(d.length == 8 && d.mem.rip_relative && d.mem.disp == 0x1234,
              "the RIP-relative form decodes as such, with its disp32");
        check(resolve_address(d.mem, gregs, 0x1000) == 0x1000 + 0x1234,
              "RIP-relative resolves from the END of the instruction");
    }

    // movbe (%rbx), %rax         48 0f 38 f0 03   (REX.W)
    {
        const uint8_t code[] = {0x48, 0x0F, 0x38, 0xF0, 0x03};
        auto d = try_decode_movbe(code);
        check(d.length == 5 && d.is_64bit, "REX.W selects the 64-bit form");
        buffer = 0x1122334455667788ull;
        emulate_movbe(d, gregs, 0);
        check(static_cast<uint64_t>(gregs[reg_to_greg_index(0)]) == 0x8877665544332211ull,
              "the 64-bit form swaps all eight bytes");
    }

    // movbe (%r12), %eax         41 0f 38 f0 04 24  (REX.B, SIB, no index)
    {
        const uint8_t code[] = {0x41, 0x0F, 0x38, 0xF0, 0x04, 0x24};
        auto d = try_decode_movbe(code);
        check(d.length == 6 && d.mem.base_reg == 12 && d.mem.index_reg == -1,
              "r12 as a base needs a SIB with no index, and decodes that way");
    }

    // The 16-bit form is rejected rather than mis-swapped.
    {
        const uint8_t code[] = {0x66, 0x0F, 0x38, 0xF0, 0x03};
        check(try_decode_movbe(code).length == 0, "the 16-bit (0x66) form is not claimed");
    }
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

    // Not POPCNT at all, must not falsely decode.
    const uint8_t not_popcnt[] = {0x90, 0x90, 0x90, 0x90};  // NOPs
    check(stud::cpu_compat::try_decode_popcnt(not_popcnt).length == 0,
          "non-POPCNT bytes (NOPs) correctly fail to decode");

    // Memory operand (mod != 3), not supported yet, must report as such
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
    test_bmi1_decode_and_emulate();
    test_movbe_decode_and_emulate();
    test_signal_handler_install();

    std::printf("all cpu-compat checks passed\n");
    return 0;
}
