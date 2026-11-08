#include "stud/gamepad_db.h"

#include <linux/input.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace stud::render_host::gamepad {

namespace {

using Guid = std::array<uint8_t, 16>;

void put16(Guid& guid, size_t at, uint16_t value) {
    guid[at] = static_cast<uint8_t>(value & 0xff);
    guid[at + 1] = static_cast<uint8_t>(value >> 8);
}

// SDL_CreateJoystickGUID with the checksum left at zero, which is how every
// database entry is written and what SDL compares against.
Guid guid_of(const PadDescription& pad) {
    Guid guid{};
    put16(guid, 0, pad.bus);
    if (pad.vendor != 0) {
        put16(guid, 4, pad.vendor);
        put16(guid, 8, pad.product);
        put16(guid, 12, pad.version);
    } else {
        // No vendor: the name fills the rest, as SDL's strlcpy into the
        // twelve bytes left does (eleven characters and a terminator).
        std::memcpy(guid.data() + 4, pad.name.data(), std::min<size_t>(pad.name.size(), 11));
    }
    return guid;
}

bool parse_guid(std::string_view hex, Guid& out) {
    if (hex.size() != 32) return false;
    for (size_t i = 0; i < 16; ++i) {
        const auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(separator, at);
        if (end == std::string_view::npos) end = text.size();
        parts.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return parts;
}

// The value of a `name:value` field in a mapping, if it has one.
bool field(std::string_view mapping, std::string_view name, std::string_view& value) {
    for (std::string_view part : split(mapping, ',')) {
        if (part.size() > name.size() && part.substr(0, name.size()) == name) {
            value = part.substr(name.size());
            return true;
        }
    }
    return false;
}

// SDL's numbering of the device's inputs (ConfigJoystick).
struct Numbering {
    std::vector<uint16_t> buttons;  // SDL button i is buttons[i]
    std::vector<uint16_t> axes;     // SDL axis i is axes[i]
    std::vector<int> hats;          // SDL hat i is HAT{hats[i]}
};

Numbering numbering_of(const PadDescription& pad) {
    Numbering n;
    std::vector<uint16_t> keys = pad.keys;
    std::sort(keys.begin(), keys.end());
    for (uint16_t code : keys) {
        if (code >= BTN_JOYSTICK && code < KEY_MAX) n.buttons.push_back(code);
    }
    for (uint16_t code : keys) {
        if (code < BTN_JOYSTICK) n.buttons.push_back(code);
    }
    bool digital[4] = {false, false, false, false};
    for (int hat = 0; hat < 4; ++hat) {
        const auto x = pad.abs.find(static_cast<uint16_t>(ABS_HAT0X + 2 * hat));
        const auto y = pad.abs.find(static_cast<uint16_t>(ABS_HAT0X + 2 * hat + 1));
        if (digital_hat(x == pad.abs.end() ? nullptr : &x->second,
                        y == pad.abs.end() ? nullptr : &y->second)) {
            digital[hat] = true;
            n.hats.push_back(hat);
        }
    }
    for (const auto& [code, info] : pad.abs) {
        (void)info;
        if (code >= ABS_MAX) continue;
        if (code >= ABS_HAT0X && code <= ABS_HAT3Y && digital[(code - ABS_HAT0X) / 2]) continue;
        n.axes.push_back(code);
    }
    return n;
}

}  // namespace

uint16_t sdl_crc16(std::string_view text) {
    uint16_t crc = 0;
    for (char c : text) {
        uint8_t r = static_cast<uint8_t>(crc) ^ static_cast<uint8_t>(c);
        uint16_t byte_crc = 0;
        for (int i = 0; i < 8; ++i) {
            byte_crc = static_cast<uint16_t>((((byte_crc ^ r) & 1) ? 0xA001 : 0) ^ (byte_crc >> 1));
            r >>= 1;
        }
        crc = static_cast<uint16_t>(byte_crc ^ (crc >> 8));
    }
    return crc;
}

bool digital_hat(const AbsInfo* x, const AbsInfo* y) {
    if (x == nullptr && y == nullptr) return false;
    const auto unit = [](const AbsInfo* a) { return a == nullptr || (a->minimum == -1 && a->maximum == 1); };
    if (unit(x) && unit(y)) return true;
    const auto bare = [](const AbsInfo* a) {
        return a == nullptr || (a->fuzz == 0 && a->flat == 0 && a->resolution == 0);
    };
    return bare(x) && bare(y);
}

std::string sdl_guid(const PadDescription& pad) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : guid_of(pad)) {
        out += kHex[byte >> 4];
        out += kHex[byte & 0xf];
    }
    return out;
}

std::string find_mapping(std::string_view db, const PadDescription& pad) {
    const Guid device = guid_of(pad);
    const uint16_t crc = sdl_crc16(pad.name);
    std::vector<std::pair<Guid, std::string_view>> entries;
    for (std::string_view line : split(db, '\n')) {
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (line.find("platform:Linux") == std::string_view::npos) continue;
        Guid guid{};
        if (!parse_guid(line.substr(0, line.find(',')), guid)) continue;
        if (guid == Guid{}) continue;
        entries.emplace_back(guid, line);
    }
    // SDL's four tries, in its order: exact, then ignoring the checksum,
    // then both again with the version ignored, for a device whose id has
    // one.
    const auto search = [&](bool match_version, bool exact_crc) -> std::string_view {
        Guid want = device;
        if (!match_version) want[12] = want[13] = 0;
        std::string_view best;
        for (auto [guid, line] : entries) {
            if (!match_version) guid[12] = guid[13] = 0;
            if (guid != want) continue;
            std::string_view crc_text;
            if (field(line, "crc:", crc_text)) {
                if (std::strtoul(std::string(crc_text).c_str(), nullptr, 16) != crc) continue;
                return line;
            }
            if (crc != 0 && exact_crc) continue;
            best = line;
        }
        return best;
    };
    std::string_view found = search(true, true);
    if (found.empty()) found = search(true, false);
    if (found.empty() && pad.vendor != 0 && pad.product != 0) {
        found = search(false, true);
        if (found.empty()) found = search(false, false);
    }
    return std::string(found);
}

std::vector<DbBinding> parse_mapping(std::string_view mapping, const PadDescription& pad) {
    const Numbering n = numbering_of(pad);
    std::vector<DbBinding> out;
    const std::vector<std::string_view> parts = split(mapping, ',');
    for (size_t i = 2; i < parts.size(); ++i) {
        const std::string_view part = parts[i];
        const size_t colon = part.find(':');
        if (colon == std::string_view::npos || colon == 0) continue;
        DbBinding b;
        std::string_view target = part.substr(0, colon);
        std::string_view input = part.substr(colon + 1);
        if (target.front() == '+' || target.front() == '-') {
            b.target_half = target.front();
            target.remove_prefix(1);
        }
        b.target = std::string(target);
        char input_half = 0;
        if (!input.empty() && (input.front() == '+' || input.front() == '-')) {
            input_half = input.front();
            input.remove_prefix(1);
        }
        bool invert = false;
        if (!input.empty() && input.back() == '~') {
            invert = true;
            input.remove_suffix(1);
        }
        if (input.size() < 2 || !std::isdigit(static_cast<unsigned char>(input[1]))) continue;
        const std::string number(input.substr(1));
        const size_t index = std::strtoul(number.c_str(), nullptr, 10);
        if (input.front() == 'a') {
            if (index >= n.axes.size()) continue;
            b.kind = DbBinding::kAbs;
            b.code = n.axes[index];
            b.low = input_half == 0 ? -1.0f : 0.0f;
            b.high = input_half == '-' ? -1.0f : 1.0f;
            if (invert) std::swap(b.low, b.high);
        } else if (input.front() == 'b') {
            if (index >= n.buttons.size()) continue;
            b.kind = DbBinding::kKey;
            b.code = n.buttons[index];
        } else if (input.front() == 'h') {
            const size_t dot = number.find('.');
            if (dot == std::string::npos || index >= n.hats.size()) continue;
            b.kind = DbBinding::kHat;
            b.hat = n.hats[index];
            b.mask = std::atoi(number.c_str() + dot + 1);
        } else {
            continue;
        }
        out.push_back(b);
    }
    return out;
}

}  // namespace stud::render_host::gamepad
