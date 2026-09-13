#pragma once

// Real hook functions jni-bridge/patches/patch_libjnivm.cmake's patched
// jni.h/vm.cpp call by name (see that file's own extensive doc comments
// for the real bugs each patched call site fixes). Two genuinely
// distinct concerns bundled under one patch:
//
//   - stud_is_bionic_address/stud_enter_bionic_context/stud_leave_
//     bionic_context/stud_wrap_native_for_bionic_caller: purely the old
//     dual-ABI-same-process design's %fs-swapping concern. Under
//     Process B's real bionic-native architecture, jnivm itself is
//     bionic-compiled and every caller is bionic too -- nothing ever
//     needs wrapping. Implemented here as the correct degenerate case
//     (is_bionic_address always false, enter/leave no-ops, wrap is the
//     identity function), not deleted from the patch, since the patch's
//     OTHER fixes (the degrade-not-crash dispatch wrapper, the
//     canonical-VM substitution) are still real and still needed.
//   - stud_canonical_java_vm/stud_register_canonical_jnivm_instance/
//     stud_canonical_jnivm_instance: a real, still-needed pointer
//     registry, entirely unrelated to %fs -- works around a genuine C++
//     Itanium-ABI multiple-inheritance issue (see bionic_jvm.h's
//     GetBionicSafeJavaVM() doc comment) by substituting the one,
//     correct, registered pointer at every JNI dispatch regardless of
//     which (possibly ABI-incompatible) pointer value a given caller
//     passed in.

namespace stud::jni_bridge {

// Call once, right after constructing the real BionicAwareJvm, with
// jvm.GetBionicSafeJavaVM()'s result -- the one, ABI-correct JavaVM*
// stud_canonical_java_vm() (called from libjnivm's own patched jni.h)
// hands back to every JNI dispatch afterward.
void register_canonical_java_vm(void* vm);

}  // namespace stud::jni_bridge
