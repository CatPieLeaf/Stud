#pragma once

// Hook functions runtime/framework/patches/patch_libjnivm.cmake's patched
// jni.h and vm.cpp call by name, and it's how a patched jnivm reports a missing
// method. The canonical pointers work around a C++ multiple-inheritance layout
// issue (see bionic_jvm.h's GetBionicSafeJavaVM()): every JNI dispatch uses the
// one correct JavaVM, whichever pointer value its caller passed in.

namespace stud::jni_bridge {

// Call once, right after constructing the real BionicAwareJvm, with
// jvm.GetBionicSafeJavaVM()'s result, the one, ABI-correct JavaVM*
// stud_canonical_java_vm() (called from libjnivm's own patched jni.h)
// hands back to every JNI dispatch afterward.
void register_canonical_java_vm(void* vm);

}  // namespace stud::jni_bridge
