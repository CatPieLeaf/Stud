#pragma once

#include <cstdint>
#include <vector>

// Game controllers, read straight from evdev.
//
// This process reads them for the same reason it owns the window: Process
// B runs inside a sandbox with a synthetic /dev and cannot see input
// devices at all, by design. What comes out of here is already in
// Android's own vocabulary -- keycodes and axis ids -- so the bridge on
// the other side only has to call the engine's entry points.
//
// Hot-plugging is handled by re-scanning, not by libudev: a scan is a
// readdir of /dev/input and a couple of ioctls per device, cheap enough
// to do once a second, and it keeps this free of another dependency.
namespace stud::render_host::gamepad {

struct Event {
    enum Type : uint32_t {
        kConnect = 1,     // device appeared; v0 is the Android gamepad type,
                          // v1 is 1 when the pad can rumble
        kDisconnect = 2,  // device went away
        kButton = 3,      // `code` is an Android keycode; v0 != 0 => pressed
        // `code` is an Android axis id and v0/v1/v2 are the three floats
        // the engine's own entry point takes. It is a VECTOR, not a
        // scalar: a stick reports both of its components on both of its
        // axis ids, and a trigger or hat reports its one value in the
        // THIRD float. See the real caller in the app's own
        // `onGenericMotion`.
        kAxis = 4,
        // What this pad actually has, answered before it is announced --
        // the real device does the same thing (`E(deviceId, type)` runs
        // before `nativeGamepadConnectEventWithGamepadType`), and a pad
        // that reports no keys is a pad with no bindings.
        // v0 != 0 => the pad really has it; v1 carries the gamepad type,
        // because these are sent before the connect event that would
        // otherwise be the only thing to carry it.
        kSupportedKey = 5,   // `code` is an Android keycode
        kSupportedAxis = 6,  // `code` is an Android axis id
    };
    uint32_t type = 0;
    int32_t device_id = 0;
    int32_t code = 0;
    float v0 = 0.0f;
    float v1 = 0.0f;
    float v2 = 0.0f;
};

// Opens what is already plugged in. Safe to call when nothing is, and
// when /dev/input cannot be read at all -- which is the ordinary case on
// a distribution that does not put users in the `input` group, and is
// reported once rather than per device.
void init();

// Re-scans for devices that appeared or went away, then reads whatever
// each one has sent. Never blocks.
void poll(std::vector<Event>& out);

// Rumble on one pad, by the device id its events carry. Both magnitudes
// are 0..1 -- the heavy and light motors a real pad has -- and zero for
// both stops it. `duration_ms` of 0 means "until told otherwise".
// Returns false when that pad has no force feedback, or when its device
// node could only be opened read-only.
bool set_rumble(int device_id, float strong, float weak, int duration_ms);

// Whether any open pad can rumble at all -- what the engine is told when
// it asks whether this platform supports haptics.
bool any_rumble_capable();

// How many controllers are open right now -- for the diagnostics command
// and the startup line.
int device_count();

}  // namespace stud::render_host::gamepad
