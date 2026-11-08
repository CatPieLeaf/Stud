// Stud's reading of SDL's controller database, checked against real entries,
// and the fallback layout for a pad the database does not know.

#include <linux/input.h>

#include <algorithm>
#include <set>
#include <string>

#include "check.h"
#include "stud/gamepad_db.h"
#include "stud/gamepad_layout.h"

using namespace stud::render_host::gamepad;

namespace {

// A wired Xbox 360 pad as the kernel's xpad driver reports it.
PadDescription xbox360(uint16_t version) {
    PadDescription pad;
    pad.bus = BUS_USB;
    pad.vendor = 0x045e;
    pad.product = 0x028e;
    pad.version = version;
    pad.name = "Microsoft X-Box 360 pad";
    pad.keys = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
                BTN_THUMBL, BTN_THUMBR};
    const AbsInfo stick{-32768, 32767, 16, 128, 0};
    const AbsInfo trigger{0, 255, 0, 0, 0};
    const AbsInfo hat{-1, 1, 0, 0, 0};
    pad.abs = {{ABS_X, stick}, {ABS_Y, stick}, {ABS_Z, trigger}, {ABS_RX, stick},
               {ABS_RY, stick}, {ABS_RZ, trigger}, {ABS_HAT0X, hat}, {ABS_HAT0Y, hat}};
    return pad;
}

const DbBinding* find(const std::vector<DbBinding>& bindings, const std::string& target) {
    for (const DbBinding& b : bindings) {
        if (b.target == target) return &b;
    }
    return nullptr;
}

}  // namespace

int main() {
    check(sdl_crc16("123456789") == 0xBB3D, "SDL's checksum is CRC-16/ARC (check value 0xBB3D)");
    check_eq(sdl_guid(xbox360(0x0114)), "030000005e0400008e02000014010000",
             "the id SDL gives a wired Xbox 360 pad");

    // The database's own entry for it, and not the Mac entry of the same id.
    const std::string mapping = find_mapping(kGameControllerDb, xbox360(0x0114));
    check(mapping.find("platform:Linux") != std::string::npos, "the pad is found, its Linux entry");
    const auto bindings = parse_mapping(mapping, xbox360(0x0114));
    const auto key = [&](const char* target) {
        const DbBinding* b = find(bindings, target);
        return b != nullptr && b->kind == DbBinding::kKey ? b->code : 0;
    };
    const auto axis = [&](const char* target) {
        const DbBinding* b = find(bindings, target);
        return b != nullptr && b->kind == DbBinding::kAbs ? b->code : 0;
    };
    // SDL's button numbering lines up with what xpad sends only if it is
    // reproduced exactly: b2 is the X button, b8 the guide button.
    check(key("a") == BTN_A && key("b") == BTN_B, "a and b are the bottom and right buttons");
    check(key("x") == BTN_X && key("y") == BTN_Y, "x and y are the X and Y buttons");
    check(key("back") == BTN_SELECT && key("start") == BTN_START && key("guide") == BTN_MODE,
          "back, start and guide");
    check(key("leftstick") == BTN_THUMBL && key("rightstick") == BTN_THUMBR, "the stick clicks");
    check(axis("leftx") == ABS_X && axis("lefty") == ABS_Y, "the left stick");
    check(axis("rightx") == ABS_RX && axis("righty") == ABS_RY, "the right stick is RX/RY on this pad");
    check(axis("lefttrigger") == ABS_Z && axis("righttrigger") == ABS_RZ, "the triggers are Z/RZ");
    const DbBinding* up = find(bindings, "dpup");
    check(up != nullptr && up->kind == DbBinding::kHat && up->hat == 0 && up->mask == 1,
          "the d-pad is the digital hat");

    // A firmware version the database has no line for still finds the pad,
    // as SDL falls back to ignoring the version.
    check(!find_mapping(kGameControllerDb, xbox360(0x0999)).empty(),
          "another version of the same pad still matches");

    // A line given later for the same id wins (SDL_GAMECONTROLLERCONFIG).
    const std::string own = std::string(kGameControllerDb) +
                            "\n030000005e0400008e02000014010000,Mine,a:b1,platform:Linux,\n";
    check(find_mapping(own, xbox360(0x0114)).find(",Mine,") != std::string::npos,
          "a later line for the same pad wins");

    // A pad nobody has heard of has no entry.
    PadDescription unknown = xbox360(0x0114);
    unknown.vendor = 0x1234;
    unknown.product = 0x5678;
    check(find_mapping(kGameControllerDb, unknown).empty(), "an unknown pad has no entry");

    // The fallback for it: Xbox and PlayStation layout, right stick on
    // RX/RY and the triggers on Z/RZ, exactly as before.
    {
        const auto layout = axis_layout({ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ});
        check(layout.at(ABS_RX) == kSlotZ && layout.at(ABS_RY) == kSlotRZ, "Xbox layout: right stick");
        check(layout.at(ABS_Z) == kSlotLT && layout.at(ABS_RZ) == kSlotRT, "Xbox layout: triggers");
    }
    // Android's convention: the right stick on Z/RZ, the triggers on
    // BRAKE/GAS. This is the pad whose right stick used to do nothing.
    {
        const auto layout = axis_layout({ABS_X, ABS_Y, ABS_Z, ABS_RZ, ABS_GAS, ABS_BRAKE});
        check(layout.at(ABS_Z) == kSlotZ && layout.at(ABS_RZ) == kSlotRZ, "Android layout: right stick");
        check(layout.at(ABS_BRAKE) == kSlotLT && layout.at(ABS_GAS) == kSlotRT, "Android layout: triggers");
    }
    // Z/RZ taken by the right stick are not also triggers.
    {
        const auto layout = axis_layout({ABS_X, ABS_Y, ABS_Z, ABS_RZ});
        check(layout.at(ABS_Z) == kSlotZ && layout.size() == 4, "no trigger is invented from the stick");
    }
    return 0;
}
