#include "stud/canonical_vm_registry.h"

#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace {
void* g_canonical_java_vm = nullptr;
void* g_canonical_jnivm_instance = nullptr;
}  // namespace

namespace stud::jni_bridge {

void register_canonical_java_vm(void* vm) { g_canonical_java_vm = vm; }

}  // namespace stud::jni_bridge

extern "C" {

int stud_is_bionic_address(const void*) { return 0; }
void stud_enter_bionic_context() {}
void stud_leave_bionic_context() {}
void* stud_wrap_native_for_bionic_caller(void* fn) { return fn; }

// The engine asked for a Java method or field Stud does not define. Usually
// harmless, and the first thing to look at when a new Roblox misbehaves:
// each is said once, and `!!!` finds them all in a log.
void stud_note_java_miss(const char* owner, const char* member, const char* signature) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!seen.insert(std::string(owner) + "." + member + signature).second) return;
    }
    std::printf("stud: !!! the engine asked for Java %s.%s %s, which Stud does not have\n", owner,
                member, signature);
    std::fflush(stdout);
}

void stud_register_canonical_jnivm_instance(void* vm) { g_canonical_jnivm_instance = vm; }
void* stud_canonical_jnivm_instance() { return g_canonical_jnivm_instance; }
void* stud_canonical_java_vm() { return g_canonical_java_vm; }

}  // extern "C"
