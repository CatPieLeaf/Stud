#pragma once

#include <fake-jni/fake-jni.h>

#include "stud/linker.h"

// FlagJniInterface.nativeInitializeNativeFlags(String[]), called where a
// real device calls it: between nativePostClientSettingsLoadedInitialization3
// and nativeAppBridgeAppStart.
//
// With no names. On a device the array is the app's own catalogue of
// Java-side feature flags, and the engine only looks each one up and hands
// the values back to that Java code; Stud runs none of it, so it reads no
// answer. It used to pass 142 names copied from one APK's catalogue: 2.740
// no longer knew 50 of them, and a session played with an empty list
// against one with the full list showed no difference in the engine's
// behaviour, warnings or errors. Passing none is the same on every build.

namespace stud::jni_bridge {

struct NativeFlagsBridgeResult {
    bool called = false;
    bool trapped_abort = false;
};

NativeFlagsBridgeResult run_native_flags_bridge(FakeJni::Jvm& jvm,
                                                 const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge
