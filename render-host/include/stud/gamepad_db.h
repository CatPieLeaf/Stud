#pragma once

// SDL's controller database, read the way SDL reads it.
//
// SDL_GameControllerDB (third_party/SDL_GameControllerDB) knows the layout of
// several hundred controllers on Linux: which button is which, and which axis
// is the right stick or a trigger. Each entry is keyed by SDL's own id for the
// device and describes it in SDL's own numbering of that device's buttons,
// axes and hats, so both are reproduced here exactly as SDL's Linux joystick
// code builds them (src/joystick/linux/SDL_sysjoystick.c, ConfigJoystick and
// IsJoystick; src/joystick/SDL_joystick.c, SDL_CreateJoystickGUID) and the
// entry is chosen the way SDL chooses it (src/joystick/SDL_gamepad.c,
// SDL_PrivateGetGamepadMappingForGUID). What comes out is said in evdev's own
// terms, so the caller never sees SDL's numbering.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace stud::render_host::gamepad {

struct AbsInfo {
    int32_t minimum = 0;
    int32_t maximum = 0;
    int32_t fuzz = 0;
    int32_t flat = 0;
    int32_t resolution = 0;
};

// What evdev says about one device.
struct PadDescription {
    uint16_t bus = 0;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t version = 0;
    std::string name;
    std::vector<uint16_t> keys;        // every EV_KEY code it has
    std::map<uint16_t, AbsInfo> abs;   // every EV_ABS code it has
};

// One element of a mapping: an SDL gamepad control (`target`, "a",
// "leftx", "dpup", ...), optionally one half of it, and the evdev input
// that drives it.
struct DbBinding {
    std::string target;
    char target_half = 0;  // '+' or '-' for one half of an axis
    enum Kind { kKey, kAbs, kHat } kind = kKey;
    uint16_t code = 0;     // the EV_KEY or EV_ABS code (kKey, kAbs)
    float low = -1.0f;     // the part of the axis that drives it, low end ...
    float high = 1.0f;     // ... to high end, -1..1 being the whole axis (kAbs)
    int hat = 0;           // which of HAT0..HAT3 (kHat)
    int mask = 0;          // SDL's direction bits: 1 up, 2 right, 4 down, 8 left
};

// SDL's id for the device, as the 32 hex digits the database uses, with the
// name checksum left out as every entry leaves it out.
std::string sdl_guid(const PadDescription& pad);
// SDL's checksum of the device name, for an entry that names one (crc:).
uint16_t sdl_crc16(std::string_view text);
// Whether SDL takes this pair of hat axes as a digital hat (SDL's own guess).
bool digital_hat(const AbsInfo* x, const AbsInfo* y);

// The mapping for this pad from `db` (gamecontrollerdb.txt's format,
// Linux entries only), or empty when it has none. A later line naming the
// same device wins over an earlier one, as it does in SDL.
std::string find_mapping(std::string_view db, const PadDescription& pad);
// That mapping in evdev terms. An element naming an input the pad does not
// have is left out.
std::vector<DbBinding> parse_mapping(std::string_view mapping, const PadDescription& pad);

// The vendored database, compiled in.
extern const char* const kGameControllerDb;

}  // namespace stud::render_host::gamepad
