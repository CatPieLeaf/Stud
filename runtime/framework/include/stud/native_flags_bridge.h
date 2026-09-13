#pragma once

#include <fake-jni/fake-jni.h>

#include "stud/linker.h"

// Real fix for task #8 (the engineering notes, "V2 sequence now completes
// cleanly" entry): a real, complete Sober boot log
// (`sober_logs/2026-08-07_04-03-56.log`) showed the real device order
// includes `FlagJniInterface.nativeInitializeNativeFlags(String[])`
// between `nativePostClientSettingsLoadedInitialization3` and
// `nativeAppBridgeAppStart` -- never called anywhere in Stud's own
// boot sequence before this.
//
// Real correction, later session (the engineering notes, "the registry-gate
// registry" finding): the empty-array call this comment used to
// justify (citing a real Sober capture's own `flagCount = 0`) does not
// match the real Java caller's own logic -- traced through the app's own code directly
// (com.roblox.client.flags.b's real `d()` method, obfuscated from
// `FlagSettings.d()`): the real array comes from the app's own flag catalogue, a
// plain in-memory singleton (a large compiled-in catalogue,
// unconditionally populated at construction, no SharedPreferences
// read at all) filtered down to entries whose value is a real
// `com.roblox.client.flags.a.d` (a distinct boolean-flag variant)
// constructed with its third argument `true` -- 142 real, fixed,
// taken from the app's own catalogue names (`real_native_flag_names.h`), not dependent on
// install-freshness or any runtime state. The earlier `flagCount = 0`
// correlation was very likely a different counter (post-registration
// success count, not raw input array length) -- not re-verified this
// session, superseded by feeding the real, complete input set the app's own code
// shows a real device always sends.

namespace stud::jni_bridge {

struct NativeFlagsBridgeResult {
    bool called = false;
    bool trapped_abort = false;
};

NativeFlagsBridgeResult run_native_flags_bridge(FakeJni::Jvm& jvm,
                                                 const stud::linker::LoadedLibrary& lib);

}  // namespace stud::jni_bridge
