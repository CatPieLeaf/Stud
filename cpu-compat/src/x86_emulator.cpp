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

DecodedBmi1 try_decode_bmi1(const uint8_t* code) {
    DecodedBmi1 result;
    size_t i = 0;

    // 3-byte VEX only -- see the header for why the 2-byte form cannot
    // encode these at all.
    if (code[i] != 0xC4) return result;
    i++;

    // byte 1: R X B mmmmm, with R/X/B stored inverted.
    const uint8_t b1 = code[i++];
    const bool vex_r = (b1 & 0x80) == 0;  // inverted
    const bool vex_b = (b1 & 0x20) == 0;  // inverted
    if ((b1 & 0x1F) != 0x02) return result;  // mmmmm must be the 0F38 map

    // byte 2: W vvvv L pp, with vvvv stored one's-complemented.
    const uint8_t b2 = code[i++];
    const bool vex_w = (b2 & 0x80) != 0;
    const int vvvv = (~(b2 >> 3)) & 0x0F;
    if ((b2 & 0x04) != 0) return result;  // L must be 0: these are scalar
    if ((b2 & 0x03) != 0) return result;  // pp must be 0: no 66/F2/F3 here

    const uint8_t opcode = code[i++];
    const uint8_t modrm = code[i++];
    if ((modrm >> 6) != 3) return result;  // register form only, as POPCNT

    const int modrm_reg = ((modrm >> 3) & 0x7) | (vex_r ? 8 : 0);
    const int modrm_rm = (modrm & 0x7) | (vex_b ? 8 : 0);

    if (opcode == 0xF2) {
        // ANDN is the only one with two sources, and the only one whose
        // destination is ModRM.reg rather than vvvv.
        result.op = Bmi1Op::kAndn;
        result.dest_reg = modrm_reg;
        result.src1_reg = vvvv;
        result.src2_reg = modrm_rm;
    } else if (opcode == 0xF3) {
        // One opcode, three instructions, chosen by ModRM.reg. The
        // destination is vvvv for all of them.
        switch ((modrm >> 3) & 0x7) {
            case 1: result.op = Bmi1Op::kBlsr; break;
            case 2: result.op = Bmi1Op::kBlsmsk; break;
            case 3: result.op = Bmi1Op::kBlsi; break;
            default: return result;
        }
        result.dest_reg = vvvv;
        result.src2_reg = modrm_rm;
    } else {
        return result;
    }

    result.is_64bit = vex_w;
    result.length = static_cast<int>(i);
    return result;
}

void emulate_bmi1(const DecodedBmi1& decoded, greg_t* gregs) {
    const auto narrow = [&](uint64_t v) {
        return decoded.is_64bit ? v : (v & 0xFFFFFFFFu);
    };
    const uint64_t src2 = narrow(static_cast<uint64_t>(gregs[reg_to_greg_index(decoded.src2_reg)]));

    uint64_t result = 0;
    bool carry = false;
    switch (decoded.op) {
        case Bmi1Op::kAndn: {
            const uint64_t src1 =
                narrow(static_cast<uint64_t>(gregs[reg_to_greg_index(decoded.src1_reg)]));
            result = ~src1 & src2;
            carry = false;
            break;
        }
        case Bmi1Op::kBlsr:
            result = src2 & (src2 - 1);
            carry = (src2 == 0);
            break;
        case Bmi1Op::kBlsmsk:
            result = src2 ^ (src2 - 1);
            carry = (src2 == 0);
            break;
        case Bmi1Op::kBlsi:
            result = src2 & (~src2 + 1);  // src & -src, without negating unsigned
            carry = (src2 != 0);
            break;
        case Bmi1Op::kNone:
            return;
    }
    result = narrow(result);
    // A 32-bit operation zeroes the upper half of the destination, which
    // narrow() has already done.
    gregs[reg_to_greg_index(decoded.dest_reg)] = static_cast<greg_t>(result);

    // EFLAGS bit positions: CF=0, ZF=6, SF=7, OF=11. AF and PF are left
    // untouched -- the SDM leaves them undefined for all four of these.
    const uint64_t sign_bit = decoded.is_64bit ? (uint64_t{1} << 63) : (uint64_t{1} << 31);
    const bool zero = (decoded.op == Bmi1Op::kBlsmsk) ? false : (result == 0);
    uint64_t eflags = static_cast<uint64_t>(gregs[REG_EFL]);
    eflags &= ~((uint64_t{1} << 0) | (uint64_t{1} << 6) | (uint64_t{1} << 7) | (uint64_t{1} << 11));
    if (carry) eflags |= uint64_t{1} << 0;
    if (zero) eflags |= uint64_t{1} << 6;
    if ((result & sign_bit) != 0) eflags |= uint64_t{1} << 7;
    gregs[REG_EFL] = static_cast<greg_t>(eflags);
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
