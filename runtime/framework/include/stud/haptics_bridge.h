#pragma once

#include <fake-jni/fake-jni.h>

#include <functional>
#include <string>

#include "stud/linker.h"

// Controller rumble, as the engine's own HapticProtocol.
//
// This is a MessageBus protocol, not a native call. A real device's own
// Java code subscribes to four protocol methods and
// answers them out of Android's VibratorManager:
//
//   SupportsHaptics          -> respond {"supportsHaptics": <bool>}
//   GetMotors                -> respond {"<motor>": {"x":..,"y":..,"z":..}}
//   UpdateSingletonVibration -> request {"intensity": "<float>"}
//   SetMotorState            -> request {"<motor>": {"millisecondsPerSample": f,
//                                                    "intensities": [..]}}
//
// and then reports the same pair to the engine through
// `NativeGLInterface.nativeUpdateMobileHapticsState(bool, String)`.
//
// Stud runs no DEX, so none of that ever happened and every experience
// that asks for haptics was told the platform has none. The motors here
// are a connected controller's own force-feedback motors rather than a
// phone's vibrator, which is the honest equivalent on a desktop, a
// real device names them "vibrator_<n>", so a pad is named the same way.
namespace stud::jni_bridge {

// Drives one pad's rumble. Magnitudes are 0..1 (heavy, light) and a
// duration of 0 means "until told otherwise". Returns whether the pad
// actually rumbled. Supplied by the process that owns the device node.
using RumbleFn = std::function<bool(int device_id, float strong, float weak, int duration_ms)>;

// Registers the four protocol handlers and reports the current state.
// `rumble` may be null, in which case the platform honestly reports no
// haptics. Safe to call again when a controller arrives or leaves.
bool run_haptics_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                        RumbleFn rumble);

// Called when a pad appears or goes away, so the engine is told the
// truth rather than whatever was true at startup. `device_id` is the id
// the pad's own events carry; `can_rumble` is false for a pad with no
// force feedback (and for one whose device node opened read-only).
void set_haptics_device(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib, int device_id,
                        bool can_rumble);

// Stops any rumble in progress, on shutdown, and when the pad the
// waveform was playing on disappears.
void stop_haptics();

}  // namespace stud::jni_bridge
