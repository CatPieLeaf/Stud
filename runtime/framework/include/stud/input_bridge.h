#pragma once

// Real desktop input, end to end.
//
// Process C (stud-render-host) owns the real Wayland compositor
// connection and therefore the only real `wl_seat` in the whole
// architecture; Process B owns the JNI bridge into libroblox's own real
// `NativeInputInterface`/`NativeGLInterface` entry points. This bridge
// joins the two: a dedicated poll thread asks the render host for queued
// seat events (`CallId::PollInputEvents`) and replays each one through
// the exact real native entry point the app's own Java code uses.
//
// The real call shapes are confirmed against the app's own code from the app's own
// the app's own SurfaceView input handlers (the real SurfaceView input handlers), not
// guessed:
//   nativePassMouseMove(x, y, dx, dy)             , x/y in surface px / density
//   nativePassMouseButton(x, y, isDown, button)   , button = getActionButton() - 1
//   nativePassMouseWheel(x, y, delta)             , delta = AXIS_VSCROLL
//   nativePassKeyEvent(isDown, scanCode, keyCode, isRepeat)
//
// Real, honest limitation: `keyCode` is Android's own virtual key code,
// which Wayland does not provide, only the evdev scan code, which
// Android reports identically via `KeyEvent.getScanCode()`. The mapping
// below covers the real ASCII/navigation keys a login screen and normal
// gameplay need; anything unmapped is still delivered with a real scan
// code and a zero key code rather than being dropped.

#include <fake-jni/fake-jni.h>

#include "stud/game_activity_stubs.h"
#include "stud/linker.h"

namespace stud::jni_bridge {

// Starts the real input poll thread. Returns false if none of the real
// input entry points are exported by this build (nothing to feed).
// Idempotent, a second call is a no-op.
// `activity`/`activity_handle` are AGDK's own real GameActivity instance and
// native handle. When supplied, every real pointer/key event is ALSO
// delivered through `onTouchEventNative`/`onKeyDownNative`/`onKeyUpNative`
// carrying a real InputDevice source (SOURCE_MOUSE) and tool type
// (TOOL_TYPE_MOUSE), the only path that tells the engine what kind of
// device produced the input, and therefore the only one that can make it
// draw its own in-frame cursor.
// Whether the wheel eases the camera toward the new zoom distance or
// steps straight to it. Off is the Android build's own behaviour, which
// is what Sober does. Must be set before the bridge starts.
void set_smooth_zoom_enabled(bool enabled);

bool start_input_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                        std::shared_ptr<MainGameActivityStub> activity = nullptr,
                        long activity_handle = 0);

// Stops the poll thread started above (best effort, used at shutdown).
void stop_input_bridge();

}  // namespace stud::jni_bridge
