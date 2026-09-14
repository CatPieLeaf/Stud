#pragma once

#include <ucontext.h>

#include <cstddef>
#include <cstdint>

// Real x86-64 instruction decode + emulation, split out from the signal
// handler itself so it's directly unit-testable as pure functions against
// real instruction bytes -- no signal/ucontext trickery needed to verify
// correctness. See cpu_compat.h for the overall module scope and honest
// testing limitations.

namespace stud::cpu_compat {

struct DecodedPopcnt {
    // 0 means "not a POPCNT instruction (or an addressing mode not yet
    // supported -- memory operands aren't decoded, only register-to-
    // register)". Non-zero is the real encoded instruction length in
    // bytes, needed to advance RIP past it.
    int length = 0;
    int dest_reg = -1;  // x86-64 register number 0-15 (REX-extended)
    int src_reg = -1;   // x86-64 register number 0-15 (REX-extended)
    bool is_64bit = false;
};

// Decodes a POPCNT instruction at `code` (F3 [REX] 0F B8 /r). Only the
// register-to-register form is supported (ModRM.mod == 3) -- memory
// operands aren't decoded yet, matching this module's "grow iteratively
// against real traps" scope (see cpu_compat.h).
DecodedPopcnt try_decode_popcnt(const uint8_t* code);

// The four BMI1 instructions, which share one decoder because they share
// one encoding shape: VEX.LZ.0F38.W0/W1, one ModRM byte, no immediate.
enum class Bmi1Op {
    kNone,
    kAndn,    // ANDN r32a/r64a, r32b/r64b, r/m : dest = ~src1 & src2
    kBlsr,    // BLSR  r32/r64, r/m             : dest = src & (src - 1)
    kBlsmsk,  // BLSMSK r32/r64, r/m            : dest = src ^ (src - 1)
    kBlsi,    // BLSI  r32/r64, r/m             : dest = src & -src
};

struct DecodedBmi1 {
    // 0 means "not one of these (or an addressing mode not decoded --
    // memory operands, as with POPCNT)". Non-zero is the real encoded
    // length, needed to advance RIP past it.
    int length = 0;
    Bmi1Op op = Bmi1Op::kNone;
    int dest_reg = -1;
    // ANDN alone has two sources. For the BLS* group src1 is unused and
    // src2 carries the single operand, so the emulator reads src2 in
    // every case and src1 only for ANDN.
    int src1_reg = -1;
    int src2_reg = -1;
    bool is_64bit = false;
};

// Decodes ANDN/BLSR/BLSMSK/BLSI at `code`. Register form only
// (ModRM.mod == 3), matching try_decode_popcnt's scope.
//
// Three things about this encoding are easy to get wrong, and each one
// silently produces plausible-looking wrong arithmetic rather than a
// crash -- which is worse than not emulating at all, since the engine
// carries the bad value onward:
//
//   * The operands live in VEX.vvvv, which is stored ONE'S-COMPLEMENTED.
//     Miss it and every one of these reads the wrong source and writes
//     the wrong destination.
//   * BLSR, BLSMSK and BLSI all share opcode 0xF3. They are told apart
//     ONLY by ModRM.reg (/1, /2, /3) -- there is no distinct opcode byte
//     for each, and treating them as if there were makes two of the three
//     execute as the remaining one.
//   * Operand size comes from VEX.W alone. The 0x66 prefix does not mean
//     16-bit here; these instructions have no 16-bit form.
//
// Only the 3-byte VEX (0xC4) form is accepted, and that is not a
// limitation: the 2-byte form (0xC5) implies the 0F opcode map, and BMI1
// lives in 0F38, which 0xC5 cannot encode at all.
DecodedBmi1 try_decode_bmi1(const uint8_t* code);

// Emulates the decoded BMI1 instruction against `gregs`, writing the
// result and the flags each one really defines (Intel SDM):
//
//   ANDN    CF=0, OF=0, SF/ZF from the result
//   BLSR    CF=(src==0), OF=0, SF/ZF from the result
//   BLSMSK  CF=(src==0), OF=0, SF from the result, ZF=0
//   BLSI    CF=(src!=0), OF=0, SF/ZF from the result
//
// AF and PF are left alone: the SDM leaves them undefined for all four,
// so writing a value would be inventing behaviour real hardware does not
// promise.
void emulate_bmi1(const DecodedBmi1& decoded, greg_t* gregs);

// Real x86-64 register-number-to-ucontext-REG_*-index mapping (standard
// ModRM/REX encoding order: 0=RAX,1=RCX,...,7=RDI,8-15=R8-R15), needed to
// read/write the right slot in a gregset_t.
int reg_to_greg_index(int reg);

// Emulates the decoded POPCNT: reads `decoded.src_reg` from `gregs`,
// counts set bits (32 or 64 depending on `decoded.is_64bit`), writes the
// result to `decoded.dest_reg`, and updates EFLAGS the same way real
// POPCNT does (clears CF/OF/SF/AF/PF, sets ZF iff the result is zero).
// Pure function over a real gregset_t-shaped array -- no signal handling,
// directly unit-testable with a plain local array standing in for a real
// mcontext_t's gregs.
void emulate_popcnt(const DecodedPopcnt& decoded, greg_t* gregs);

}  // namespace stud::cpu_compat
