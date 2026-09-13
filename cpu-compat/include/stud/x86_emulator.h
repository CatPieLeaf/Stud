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
