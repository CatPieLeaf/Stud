#include "stud/x86_emulator.h"

namespace stud::cpu_compat {

int reg_to_greg_index(int reg) {
    static const int table[16] = {
        REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
        REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    };
    return table[reg];
}

DecodedPopcnt try_decode_popcnt(const uint8_t* code) {
    DecodedPopcnt result;
    size_t i = 0;

    if (code[i] != 0xF3) return result;
    i++;

    bool rex_w = false;
    bool rex_r = false;
    bool rex_b = false;
    if ((code[i] & 0xF0) == 0x40) {
        rex_w = (code[i] & 0x08) != 0;
        rex_r = (code[i] & 0x04) != 0;
        rex_b = (code[i] & 0x01) != 0;
        i++;
    }

    if (code[i] != 0x0F || code[i + 1] != 0xB8) return result;
    i += 2;

    uint8_t modrm = code[i];
    i++;
    int mod = (modrm >> 6) & 0x3;
    if (mod != 3) return result;  // memory operand -- not decoded yet

    int reg = ((modrm >> 3) & 0x7) | (rex_r ? 8 : 0);
    int rm = (modrm & 0x7) | (rex_b ? 8 : 0);

    result.length = static_cast<int>(i);
    result.dest_reg = reg;
    result.src_reg = rm;
    result.is_64bit = rex_w;
    return result;
}

void emulate_popcnt(const DecodedPopcnt& decoded, greg_t* gregs) {
    uint64_t src = static_cast<uint64_t>(gregs[reg_to_greg_index(decoded.src_reg)]);
    if (!decoded.is_64bit) {
        src &= 0xFFFFFFFFu;
    }
    uint64_t result = static_cast<uint64_t>(__builtin_popcountll(src));
    gregs[reg_to_greg_index(decoded.dest_reg)] = static_cast<greg_t>(result);

    // Real POPCNT semantics (Intel SDM): CF, OF, SF, AF, PF cleared; ZF set
    // iff the result is zero. EFLAGS bit positions: CF=0, PF=2, AF=4, ZF=6,
    // SF=7, OF=11.
    uint64_t eflags = static_cast<uint64_t>(gregs[REG_EFL]);
    eflags &= ~((uint64_t{1} << 0) | (uint64_t{1} << 2) | (uint64_t{1} << 4) | (uint64_t{1} << 6) |
                (uint64_t{1} << 7) | (uint64_t{1} << 11));
    if (result == 0) {
        eflags |= (uint64_t{1} << 6);
    }
    gregs[REG_EFL] = static_cast<greg_t>(eflags);
}

}  // namespace stud::cpu_compat
