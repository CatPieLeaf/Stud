#pragma once

// Which of a controller's evdev axes is which, decided per device from the
// axes it actually reports.
//
// There is more than one convention, and the kernel drivers do not agree.
// Xbox and PlayStation pads, and the Linux gamepad specification, put the
// right stick on ABS_RX/ABS_RY and the triggers on ABS_Z/ABS_RZ. Many
// others (a lot of Bluetooth pads, an Xbox controller over Bluetooth on the
// stock driver, generic USB pads) follow Android's own convention instead:
// the right stick on ABS_Z/ABS_RZ and the triggers on ABS_GAS/ABS_BRAKE.
// Android's Generic.kl maps them that way, and only its per-device layouts
// for Xbox-style pads move the right stick to RX/RY. Stud used to apply the
// Xbox layout to every pad, so on the others the right stick arrived as two
// triggers and never moved anything.
//
// The rule is SDL's own fallback for a pad it has no mapping for
// (SDL_sysjoystick.c): the right stick is RX/RY when the pad has both, else
// Z/RZ; a trigger is HAT2Y/HAT2X (the gamepad specification), else
// BRAKE/GAS (Android), else Z/RZ (Xbox and PlayStation). Unlike SDL, an axis
// already taken by the right stick is not also used as a trigger.

#include <linux/input.h>

#include <cstdint>
#include <map>
#include <set>

namespace stud::render_host::gamepad {

// The eight axis slots the engine's caller keeps, in its own order
// (a `float[8]`): left stick, right stick, the two triggers, then the hat.
// Indices are load-bearing: a stick is sent as a vector of slots 0/1, 2/3.
enum Slot { kSlotX = 0, kSlotY, kSlotZ, kSlotRZ, kSlotLT, kSlotRT, kSlotHatX, kSlotHatY,
            kSlotCount };

// Every evdev axis any of the conventions uses.
inline constexpr uint16_t kLayoutAxes[] = {ABS_X,     ABS_Y,     ABS_Z,     ABS_RX,
                                           ABS_RY,    ABS_RZ,    ABS_GAS,   ABS_BRAKE,
                                           ABS_HAT0X, ABS_HAT0Y, ABS_HAT2X, ABS_HAT2Y};

// evdev axis -> slot, for the axes the device has.
inline std::map<uint16_t, int> axis_layout(const std::set<uint16_t>& has) {
    std::map<uint16_t, int> slot;
    const auto present = [&](uint16_t code) { return has.count(code) != 0; };
    if (present(ABS_X)) slot[ABS_X] = kSlotX;
    if (present(ABS_Y)) slot[ABS_Y] = kSlotY;
    if (present(ABS_HAT0X)) slot[ABS_HAT0X] = kSlotHatX;
    if (present(ABS_HAT0Y)) slot[ABS_HAT0Y] = kSlotHatY;
    bool z_is_stick = false;
    if (present(ABS_RX) && present(ABS_RY)) {
        slot[ABS_RX] = kSlotZ;  // Android calls the right stick Z/RZ
        slot[ABS_RY] = kSlotRZ;
    } else if (present(ABS_Z) && present(ABS_RZ)) {
        slot[ABS_Z] = kSlotZ;
        slot[ABS_RZ] = kSlotRZ;
        z_is_stick = true;
    }
    for (const auto& [codes, trigger] :
         {std::pair{std::initializer_list<uint16_t>{ABS_HAT2Y, ABS_BRAKE, ABS_Z}, kSlotLT},
          std::pair{std::initializer_list<uint16_t>{ABS_HAT2X, ABS_GAS, ABS_RZ}, kSlotRT}}) {
        for (uint16_t code : codes) {
            if (!present(code)) continue;
            if (z_is_stick && (code == ABS_Z || code == ABS_RZ)) continue;
            slot[code] = trigger;
            break;
        }
    }
    return slot;
}

}  // namespace stud::render_host::gamepad
