#include "stud/canonical_vm_registry.h"

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

void stud_register_canonical_jnivm_instance(void* vm) { g_canonical_jnivm_instance = vm; }
void* stud_canonical_jnivm_instance() { return g_canonical_jnivm_instance; }
void* stud_canonical_java_vm() { return g_canonical_java_vm; }

}  // extern "C"
