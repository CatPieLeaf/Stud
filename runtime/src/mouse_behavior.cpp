#include "mouse_behavior.h"

#include <Zydis/Zydis.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "stud/trap_recovery.h"

namespace stud::runtime {
namespace {

// What the decode found. Every field comes out of the loaded engine's own
// instructions; none of it is a number taken from one build.
struct Probe {
    void* (*getter)(int) = nullptr;  // the singleton getter
    int getter_arg = 0;              // and the argument the engine passes it
    // The loads from the getter's object to MouseBehavior: every entry but
    // the last is a pointer load, the last is the field itself.
    std::vector<int32_t> path;
};

Probe g_probe;
// Cleared for good the moment the decode is shown wrong (see
// verify_mouse_behavior), so a wrong field never drives the cursor twice.
std::atomic<bool> g_enabled{false};
bool g_verified = false;

// A register's value, as far as the walk can tell.
struct Value {
    enum Kind : uint8_t { kNothing, kImmediate, kEngine };
    Kind kind = kNothing;
    int64_t imm = 0;
    // kEngine: the getter's object, followed through these loads.
    std::vector<int32_t> loads;
};
using Registers = std::array<Value, ZYDIS_REGISTER_MAX_VALUE + 1>;

ZydisRegister full(ZydisRegister r) {
    return r == ZYDIS_REGISTER_NONE ? r
                                    : ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
}

void clobber_caller_saved(Registers& regs) {
    for (ZydisRegister r : {ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX,
                            ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI, ZYDIS_REGISTER_R8,
                            ZYDIS_REGISTER_R9, ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11}) {
        regs[r] = Value{};
    }
}

// How far one walk reads. Bounds the decode, not anything the engine does.
constexpr int kMaxInstructions = 256;
constexpr int kMaxCallDepth = 4;

// Follows the engine's predicate by what each instruction DOES: which
// register holds the getter's object, which loads lead from it, and which
// of them is compared with LockCenter. Calls that take the object are
// followed into, which is what survives the compiler moving the body into
// a helper (as 2.740 did); register choice, encodings and the lock it takes
// around the read do not matter.
bool walk(const ZydisDecoder& dec, const unsigned char* at, Registers regs, int depth,
          Probe& out) {
    for (int n = 0; n < kMaxInstructions; ++n) {
        ZydisDecodedInstruction ins;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, at, ZYDIS_MAX_INSTRUCTION_LENGTH, &ins,
                                                 ops))) {
            return false;
        }
        // The engine's object plus one load, if this memory operand is one.
        auto load_from = [&](const ZydisDecodedOperand& m, Value& v) {
            const ZydisRegister base = full(m.mem.base);
            if (m.mem.index != ZYDIS_REGISTER_NONE || base == ZYDIS_REGISTER_NONE) return false;
            if (regs[base].kind != Value::kEngine) return false;
            v = regs[base];
            v.loads.push_back(static_cast<int32_t>(m.mem.disp.value));
            return true;
        };
        switch (ins.mnemonic) {
            case ZYDIS_MNEMONIC_RET:
                return false;
            case ZYDIS_MNEMONIC_CALL:
            case ZYDIS_MNEMONIC_JMP: {
                const bool is_call = ins.mnemonic == ZYDIS_MNEMONIC_CALL;
                ZyanU64 target = 0;
                const bool direct =
                    ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[0].imm.is_relative &&
                    ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(
                        &ins, &ops[0], reinterpret_cast<ZyanU64>(at), &target));
                const Value rdi = regs[ZYDIS_REGISTER_RDI];
                if (direct && is_call && out.getter == nullptr && rdi.kind == Value::kImmediate) {
                    // The singleton getter, with the engine's own argument.
                    out.getter = reinterpret_cast<void* (*)(int)>(target);
                    out.getter_arg = static_cast<int>(rdi.imm);
                    clobber_caller_saved(regs);
                    regs[ZYDIS_REGISTER_RAX] = Value{Value::kEngine, 0, {}};
                    break;
                }
                if (direct && rdi.kind == Value::kEngine && rdi.loads.empty() &&
                    depth < kMaxCallDepth) {
                    // The object handed on: the body lives in there.
                    Registers inner{};
                    inner[ZYDIS_REGISTER_RDI] = rdi;
                    if (walk(dec, reinterpret_cast<const unsigned char*>(target), inner,
                             depth + 1, out)) {
                        return true;
                    }
                }
                if (!is_call) return false;
                clobber_caller_saved(regs);
                break;
            }
            case ZYDIS_MNEMONIC_CMP: {
                // The test itself: a value reached from the object, compared
                // with LockCenter's own enum value.
                if (ops[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
                    ops[1].imm.value.s != static_cast<int64_t>(MouseBehavior::kLockCenter)) {
                    break;
                }
                Value v;
                if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    v = regs[full(ops[0].reg.value)];
                } else if (ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY || !load_from(ops[0], v)) {
                    break;
                }
                if (v.kind == Value::kEngine && !v.loads.empty()) {
                    out.path = v.loads;
                    return true;
                }
                break;
            }
            case ZYDIS_MNEMONIC_MOV:
            case ZYDIS_MNEMONIC_MOVZX:
            case ZYDIS_MNEMONIC_MOVSX:
            case ZYDIS_MNEMONIC_MOVSXD: {
                if (ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) break;  // a store
                Value v;
                if (ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    v = regs[full(ops[1].reg.value)];
                } else if (ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    v = Value{Value::kImmediate, ops[1].imm.value.s, {}};
                } else if (ops[1].type != ZYDIS_OPERAND_TYPE_MEMORY || !load_from(ops[1], v)) {
                    v = Value{};
                }
                regs[full(ops[0].reg.value)] = v;
                break;
            }
            case ZYDIS_MNEMONIC_XOR:
                // xor r, r: the compiler's way of writing zero.
                if (ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                    full(ops[0].reg.value) == full(ops[1].reg.value)) {
                    regs[full(ops[0].reg.value)] = Value{Value::kImmediate, 0, {}};
                    break;
                }
                [[fallthrough]];
            default:
                // Anything else that writes a register leaves it unknown.
                for (int i = 0; i < ins.operand_count_visible; ++i) {
                    if (ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                        (ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
                        regs[full(ops[i].reg.value)] = Value{};
                    }
                }
                break;
        }
        at += ins.length;
    }
    return false;
}

const char* name_of(MouseBehavior b) {
    switch (b) {
        case MouseBehavior::kDefault: return "Default";
        case MouseBehavior::kLockCenter: return "LockCenter";
        case MouseBehavior::kLockCurrentPosition: return "LockCurrentPosition";
        default: return "unknown";
    }
}

}  // namespace

bool init_mouse_behavior_probe(const stud::linker::LoadedLibrary& lib) {
    g_enabled.store(false);
    g_probe = Probe{};
    g_verified = false;
    const auto* fn = static_cast<const unsigned char*>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_"
        "nativeGetMainWindowIsMouseLockedCenter"));
    if (fn == nullptr) {
        std::printf("stud: MouseBehavior: the engine does not export its LockCenter predicate, "
                    "the cursor falls back to following the pointer\n");
        std::fflush(stdout);
        return false;
    }
    ZydisDecoder dec;
    ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    Probe p;
    if (!walk(dec, fn, Registers{}, 0, p) || p.getter == nullptr) {
        std::printf("stud: MouseBehavior: could not follow the engine's LockCenter predicate to "
                    "the field it tests (getter %s), the cursor falls back to the predicate\n",
                    p.getter != nullptr ? "found" : "not found");
        std::fflush(stdout);
        return false;
    }
    g_probe = p;
    g_enabled.store(true);
    // Found is not the same as right; verify_mouse_behavior() says which.
    std::printf("stud: MouseBehavior: decoded a %zu-load path from the engine's predicate, "
                "checking it against the predicate itself\n",
                p.path.size());
    std::fflush(stdout);
    return true;
}

namespace {

int read_behavior() {
    const Probe& p = g_probe;
    auto* at = static_cast<unsigned char*>(p.getter(p.getter_arg));
    for (size_t i = 0; at != nullptr && i + 1 < p.path.size(); ++i) {
        void* next = nullptr;
        std::memcpy(&next, at + p.path[i], sizeof(next));
        at = static_cast<unsigned char*>(next);
    }
    if (at == nullptr) return -1;
    // Read without the engine's lock: one aligned 32-bit load cannot tear,
    // and replaying the lock means calling the engine's own lock functions
    // with whatever shape this build gives them. Trapped by the caller, so
    // an object torn down mid-read costs a reading, not the process.
    int32_t v = -1;
    std::memcpy(&v, at + p.path.back(), sizeof(v));
    return v;
}

}  // namespace

MouseBehavior read_mouse_behavior() {
    if (!g_enabled.load()) return MouseBehavior::kUnknown;
    int behavior = -1;
    if (!stud::jni_bridge::call_trapping_abort_with_result(&read_behavior, behavior)) {
        g_enabled.store(false);
        std::printf("stud: MouseBehavior: reading the decoded field trapped, disabled for this "
                    "run\n");
        std::fflush(stdout);
        return MouseBehavior::kUnknown;
    }
    // Only the real enum's own values; anything else is not this field.
    if (behavior < 0 || behavior > 2) return MouseBehavior::kUnknown;
    return static_cast<MouseBehavior>(behavior);
}

bool verify_mouse_behavior(MouseBehavior field, bool predicate_locks_center) {
    if (!g_enabled.load() || field == MouseBehavior::kUnknown) return g_enabled.load();
    const bool field_locks_center = field == MouseBehavior::kLockCenter;
    if (field_locks_center != predicate_locks_center) {
        g_enabled.store(false);
        std::printf("stud: MouseBehavior: the decoded field says %s but the engine's own "
                    "predicate says LockCenter=%d; the decode is wrong for this build and is "
                    "off, the cursor follows the predicate\n",
                    name_of(field), predicate_locks_center ? 1 : 0);
        std::fflush(stdout);
        return false;
    }
    if (!g_verified && field_locks_center) {
        g_verified = true;
        std::printf("stud: MouseBehavior: decoded field confirmed by the engine's predicate\n");
        std::fflush(stdout);
    }
    return true;
}

}  // namespace stud::runtime
