#include "mouse_behavior.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "stud/trap_recovery.h"

namespace stud::runtime {
namespace {

// What the decode found in the engine's own exported predicate. Every
// field comes out of that function's instruction stream; none of it is a
// constant measured against one build of the APK.
struct Probe {
    bool valid = false;
    void* (*getter)(int) = nullptr;   // mov $imm,%edi ; call <getter>
    int getter_arg = 0;
    void (*guard_enter)(void*) = nullptr;  // the scope guard the engine
    void (*guard_leave)(void*) = nullptr;  // takes around the read
    int32_t guard_offset = 0;              // add $imm,%rax, guard member
    int32_t subsystem_offset = 0;          // mov <d1>(%rbx),%rax
    int32_t behavior_offset = 0;           // cmpl $1,<d2>(%rax)
};

Probe g_probe;

int32_t read_i32(const unsigned char* p) {
    int32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The function is small and straight-line (no branches at all; it ends
// in one compare and a stack-guard check), so a linear walk over its
// first bytes is enough; there is nothing to follow.
constexpr size_t kScanBytes = 0x90;

}  // namespace

bool init_mouse_behavior_probe(const stud::linker::LoadedLibrary& lib) {
    g_probe = Probe{};
    auto* fn = static_cast<const unsigned char*>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeInputInterface_"
        "nativeGetMainWindowIsMouseLockedCenter"));
    if (fn == nullptr) {
        std::printf("stud: MouseBehavior probe: the engine does not export the predicate to "
                    "decode, the cursor falls back to following the pointer\n");
        std::fflush(stdout);
        return false;
    }

    Probe p;
    const unsigned char* call_after_getter = nullptr;
    for (size_t i = 0; i + 6 < kScanBytes; ++i) {
        const unsigned char* at = fn + i;
        // mov $imm32,%edi ; call rel32  , the singleton getter, with the
        // engine's own argument rather than a guess at one.
        if (p.getter == nullptr && at[0] == 0xbf && at[5] == 0xe8) {
            p.getter_arg = read_i32(at + 1);
            const unsigned char* next = at + 10;
            p.getter = reinterpret_cast<void* (*)(int)>(
                const_cast<unsigned char*>(next + read_i32(at + 6)));
            call_after_getter = next;
            continue;
        }
        // add $imm32,%rax , the member the scope guard is built on.
        if (p.getter != nullptr && p.guard_offset == 0 && at[0] == 0x48 && at[1] == 0x05) {
            p.guard_offset = read_i32(at + 2);
            continue;
        }
        // call rel32, after the getter: first is the guard's constructor,
        // second (past the compare) its destructor.
        if (call_after_getter != nullptr && at > call_after_getter && at[0] == 0xe8) {
            const unsigned char* next = at + 5;
            auto* target = reinterpret_cast<void (*)(void*)>(
                const_cast<unsigned char*>(next + read_i32(at + 1)));
            if (p.guard_enter == nullptr) {
                p.guard_enter = target;
            } else if (p.guard_leave == nullptr) {
                p.guard_leave = target;
            }
            continue;
        }
        // mov <d1>(%rbx),%rax
        if (p.subsystem_offset == 0 && at[0] == 0x48 && at[1] == 0x8b && at[2] == 0x83) {
            p.subsystem_offset = read_i32(at + 3);
            continue;
        }
        // cmpl $0x1,<d2>(%rax) , the MouseBehavior test itself. The
        // immediate must be LockCenter's own value, or this is not the
        // comparison this decode is looking for.
        if (p.behavior_offset == 0 && at[0] == 0x83 && at[1] == 0xb8 && at[6] == 0x01) {
            p.behavior_offset = read_i32(at + 2);
            continue;
        }
    }

    // Every piece has to be there, and the offsets have to be sane
    // structure members rather than whatever a changed instruction stream
    // happened to decode to.
    const bool sane = p.getter != nullptr && p.guard_enter != nullptr && p.guard_leave != nullptr &&
                      p.guard_offset > 0 && p.guard_offset < (1 << 20) && p.subsystem_offset > 0 &&
                      p.subsystem_offset < (1 << 20) && p.behavior_offset > 0 &&
                      p.behavior_offset < (1 << 20);
    if (!sane) {
        std::printf("stud: MouseBehavior probe: the engine's predicate is not the shape this "
                    "decodes (getter=%d guard=%d/%d off=%d/%d/%d), the cursor falls back to "
                    "following the pointer\n",
                    p.getter != nullptr ? 1 : 0, p.guard_enter != nullptr ? 1 : 0,
                    p.guard_leave != nullptr ? 1 : 0, p.guard_offset, p.subsystem_offset,
                    p.behavior_offset);
        std::fflush(stdout);
        return false;
    }
    p.valid = true;
    g_probe = p;
    std::printf("stud: MouseBehavior probe ready: getter(%d), guard +0x%x, subsystem +0x%x, "
                "behavior +0x%x\n",
                p.getter_arg, p.guard_offset, p.subsystem_offset, p.behavior_offset);
    std::fflush(stdout);
    return true;
}

namespace {

// The engine's own body, replayed: take the object, take the guard it
// takes, read the field, drop the guard. Run through the trap machinery
// by its caller, so a stale pointer costs a reading rather than the
// process.
int read_behavior_locked() {
    const Probe& p = g_probe;
    auto* obj = static_cast<unsigned char*>(p.getter(p.getter_arg));
    if (obj == nullptr) return -1;
    // The guard is {pointer to the member, a flag byte}, built on the
    // stack exactly as the engine builds it.
    struct Guard {
        void* member;
        unsigned char engaged;
    } guard{};
    guard.member = obj + p.guard_offset;
    guard.engaged = 0;
    p.guard_enter(&guard);
    void* subsystem = nullptr;
    std::memcpy(&subsystem, obj + p.subsystem_offset, sizeof(subsystem));
    int behavior = -1;
    if (subsystem != nullptr) {
        std::memcpy(&behavior, static_cast<unsigned char*>(subsystem) + p.behavior_offset,
                    sizeof(behavior));
    }
    p.guard_leave(&guard);
    return behavior;
}

}  // namespace

MouseBehavior read_mouse_behavior() {
    if (!g_probe.valid) return MouseBehavior::kUnknown;
    int behavior = -1;
    const bool ok = stud::jni_bridge::call_trapping_abort_with_result(&read_behavior_locked,
                                                                      behavior);
    if (!ok) {
        // One failed read is not worth a line every 8ms, and a probe that
        // faults once will fault again: stop asking.
        static bool said = false;
        if (!said) {
            said = true;
            std::printf("stud: MouseBehavior probe trapped, disabled for this run\n");
            std::fflush(stdout);
        }
        g_probe.valid = false;
        return MouseBehavior::kUnknown;
    }
    // Only the real enum's own values; anything else means the decode
    // found the wrong field and must not drive the cursor.
    if (behavior < 0 || behavior > 2) return MouseBehavior::kUnknown;
    return static_cast<MouseBehavior>(behavior);
}

}  // namespace stud::runtime
