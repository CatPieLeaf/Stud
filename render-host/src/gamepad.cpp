#include "stud/gamepad.h"
#include "stud/gamepad_db.h"
#include "stud/gamepad_layout.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace stud::render_host::gamepad {

namespace {

// Android's own keycodes and axis ids, which is what the engine's entry
// points take. Named here rather than included because this process has
// no NDK headers, the values are Android's public API and fixed.
constexpr int kBtnA = 96, kBtnB = 97, kBtnX = 99, kBtnY = 100;
constexpr int kBtnL1 = 102, kBtnR1 = 103, kBtnL2 = 104, kBtnR2 = 105;
constexpr int kBtnThumbL = 106, kBtnThumbR = 107;
constexpr int kBtnSelect = 109, kBtnStart = 108, kBtnMode = 110;
constexpr int kDpadUp = 19, kDpadDown = 20, kDpadLeft = 21, kDpadRight = 22;

constexpr int kAxisX = 0, kAxisY = 1, kAxisZ = 11, kAxisRZ = 14;
constexpr int kAxisHatX = 15, kAxisHatY = 16;
constexpr int kAxisLTrigger = 17, kAxisRTrigger = 18;

// evdev code -> Android keycode, matching Android's own key layout
// (`Generic.kl`, and every vendor .kl for an Xbox-layout pad) rather than
// the positional NAMES in `linux/input-event-codes.h`. Used for a pad the
// SDL database does not know.
//
// This is the one trap in the whole file: `BTN_NORTH` is 0x133 and
// `BTN_WEST` is 0x134, but every kernel driver for an Xbox-layout pad
// emits 0x133 for the button labelled **X** and 0x134 for **Y**, the
// positional names were added as aliases over the legacy `BTN_X`/`BTN_Y`
// and do not describe what the drivers actually send. Android maps
// `key 0x133 BUTTON_X` / `key 0x134 BUTTON_Y`, so mapping by the
// positional name swaps X and Y, which is exactly what it looks like.
int android_button_for(uint16_t code) {
    switch (code) {
        case BTN_SOUTH: return kBtnA;
        case BTN_EAST: return kBtnB;
        case BTN_NORTH: return kBtnX;  // 0x133, the X button
        case BTN_WEST: return kBtnY;   // 0x134, the Y button
        case BTN_TL: return kBtnL1;
        case BTN_TR: return kBtnR1;
        case BTN_TL2: return kBtnL2;
        case BTN_TR2: return kBtnR2;
        case BTN_THUMBL: return kBtnThumbL;
        case BTN_THUMBR: return kBtnThumbR;
        case BTN_SELECT: return kBtnSelect;
        case BTN_START: return kBtnStart;
        case BTN_MODE: return kBtnMode;
        case BTN_DPAD_UP: return kDpadUp;
        case BTN_DPAD_DOWN: return kDpadDown;
        case BTN_DPAD_LEFT: return kDpadLeft;
        case BTN_DPAD_RIGHT: return kDpadRight;
        default: return -1;
    }
}

// Which Android axis id each slot is reported under.
constexpr int kSlotAxisId[kSlotCount] = {kAxisX,        kAxisY,        kAxisZ,
                                         kAxisRZ,       kAxisLTrigger, kAxisRTrigger,
                                         kAxisHatX,     kAxisHatY};

// The gamepad type the engine is told about, derived from the device
// name exactly the way the real app derives it:
// 3 Xbox, 2 DualSense, 1 DualShock, 0 anything else. Stud used to report
// a hardcoded 1, i.e. it told the engine every controller was a PS4 pad.
int gamepad_type_for_name(const std::string& name) {
    std::string upper = name;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (upper.find("XBOX") != std::string::npos) return 3;
    if (upper.find("DUALSENSE") != std::string::npos || upper.find("PS5") != std::string::npos) {
        return 2;
    }
    if (upper.find("DUALSHOCK") != std::string::npos || upper.find("PS4") != std::string::npos ||
        upper.find("PLAYSTATION") != std::string::npos) {
        return 1;
    }
    return 0;
}

// What a rule drives: an Android button, one of the eight axis slots, or
// one direction of the d-pad.
struct Output {
    enum Kind { kButton, kSlot, kDpad } kind = kButton;
    int key = -1;
    int slot = -1;
    int dpad = -1;          // 0 up, 1 right, 2 down, 3 left
    float low = -1.0f;      // a slot's value at the start of its input's range ...
    float high = 1.0f;      // ... and at the end
};

// One input of the pad wired to one output. Every pad's inputs are turned
// into these when it connects, from the SDL database when it is in there
// and from its evdev axes otherwise, so reading it is the same either way.
struct Rule {
    DbBinding in;
    Output out;
};

// SDL's control names -> what Stud drives. Positional, like SDL's: "a" is
// the bottom face button, "x" the left one, which are Android's BUTTON_A and
// BUTTON_X.
bool output_for(const DbBinding& b, Output& out) {
    static const std::map<std::string, int> buttons = {
        {"a", kBtnA},          {"b", kBtnB},          {"x", kBtnX},
        {"y", kBtnY},          {"back", kBtnSelect},  {"start", kBtnStart},
        {"guide", kBtnMode},   {"leftshoulder", kBtnL1}, {"rightshoulder", kBtnR1},
        {"leftstick", kBtnThumbL}, {"rightstick", kBtnThumbR}};
    static const std::map<std::string, int> dpad = {
        {"dpup", 0}, {"dpright", 1}, {"dpdown", 2}, {"dpleft", 3}};
    static const std::map<std::string, int> slots = {
        {"leftx", kSlotX},  {"lefty", kSlotY},  {"rightx", kSlotZ},
        {"righty", kSlotRZ}, {"lefttrigger", kSlotLT}, {"righttrigger", kSlotRT}};
    if (auto it = buttons.find(b.target); it != buttons.end()) {
        out.kind = Output::kButton;
        out.key = it->second;
        return true;
    }
    if (auto it = dpad.find(b.target); it != dpad.end()) {
        out.kind = Output::kDpad;
        out.dpad = it->second;
        return true;
    }
    if (auto it = slots.find(b.target); it != slots.end()) {
        out.kind = Output::kSlot;
        out.slot = it->second;
        // SDL's output ranges: a trigger runs 0..1, a stick -1..1, and
        // one half of a stick 0..1 or 0..-1.
        if (out.slot == kSlotLT || out.slot == kSlotRT) {
            out.low = 0.0f;
            out.high = 1.0f;
        } else if (b.target_half == '+') {
            out.low = 0.0f;
            out.high = 1.0f;
        } else if (b.target_half == '-') {
            out.low = 0.0f;
            out.high = -1.0f;
        }
        return true;
    }
    return false;  // paddles, touchpad, misc: nothing on Android to send them to
}

struct Axis {
    int32_t minimum = 0;
    int32_t maximum = 0;
    int32_t flat = 0;  // the driver's own idea of this stick's dead zone
};

struct Device {
    int fd = -1;
    int id = 0;
    int type = 0;
    std::string name;
    std::string guid;
    bool from_db = false;
    std::map<uint16_t, Axis> axes;
    std::vector<Rule> rules;
    // Current normalised value of each of the eight axis slots, and which
    // of them changed since the last SYN_REPORT. A stick is sent as a
    // vector, so its two components have to be emitted together from one
    // consistent snapshot, which is what a report boundary is.
    float slot[kSlotCount] = {0.0f};
    bool slot_dirty[kSlotCount] = {false};
    // A digital hat's position on each axis (-1, 0, 1), and the d-pad the
    // rules drive from it or from buttons.
    int hat_position[4][2] = {};
    bool dpad[4] = {false, false, false, false};
    // Buttons currently held, so two inputs driving one button press it once.
    std::map<int, bool> key_down;
    // The d-pad reported as a hat also has to reach the engine as DPAD
    // key presses: real Android synthesises them, and Roblox's own UI
    // navigation listens for the keys.
    std::map<int, bool> hat_key_state;
    // Whether each trigger is currently held, for the synthesised
    // BUTTON_L2/BUTTON_R2 presses; see flush_axes().
    bool trigger_held[2] = {false, false};
    // Force feedback: the id the kernel gave the uploaded rumble effect,
    // -1 when this pad has none or the fd is read-only.
    int rumble_effect = -1;
    bool can_rumble = false;
    int death_errno = 0;
};

// Rumble arrives on a dispatch thread and poll() runs on input's own, and
// both walk the pads, which poll() also adds to and removes from.
std::mutex g_devices_mutex;

std::map<std::string, Device>& devices() {
    static std::map<std::string, Device> d;
    return d;
}

bool bit_set(const unsigned long* bits, int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1ul;
}

// A gamepad is a device with the south face button and two stick axes.
// That is what every pad has and what nothing else does: a keyboard has
// no ABS_X, and a mouse has no BTN_SOUTH.
bool looks_like_a_gamepad(int fd) {
    unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1]{};
    unsigned long abs[(ABS_MAX / (8 * sizeof(unsigned long))) + 1]{};
    if (::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0) return false;
    if (::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0) return false;
    return bit_set(keys, BTN_SOUTH) && bit_set(abs, ABS_X) && bit_set(abs, ABS_Y);
}

// The id the engine keys a pad by. It maps this onto a Roblox gamepad
// slot (Gamepad1..Gamepad8), and some bindings, tool activation, most
// importantly, are bound to Gamepad1 specifically, so which id the
// first pad gets is load-bearing rather than cosmetic.
// STUD_PAD_FIRST_ID exists to test that mapping without a rebuild.
int next_device_id() {
    static int next = [] {
        if (const char* v = std::getenv("STUD_PAD_FIRST_ID")) return std::atoi(v);
        return 1;
    }();
    return next++;
}

// The SDL database, then SDL_GAMECONTROLLERCONFIG, SDL's own variable for
// mappings of one's own (Steam sets it from its controller settings).
// Later lines win, so those come after the database.
const std::string& mapping_source() {
    static const std::string source = [] {
        std::string s = kGameControllerDb;
        if (const char* extra = std::getenv("SDL_GAMECONTROLLERCONFIG")) {
            s += "\n";
            s += extra;
        }
        return s;
    }();
    return source;
}

// -1..1 over the axis's whole range. A stick also gets the driver's own
// flat/dead zone, so a resting stick reads as exactly zero rather than as a
// small permanent drift.
float normalise(const Axis& axis, int32_t raw, bool dead_zone) {
    const float minimum = static_cast<float>(axis.minimum);
    const float maximum = static_cast<float>(axis.maximum);
    if (maximum <= minimum) return 0.0f;
    const float centre = (minimum + maximum) * 0.5f;
    if (dead_zone && axis.flat > 0 &&
        std::abs(static_cast<float>(raw) - centre) <= static_cast<float>(axis.flat)) {
        return 0.0f;
    }
    return ((static_cast<float>(raw) - minimum) / (maximum - minimum)) * 2.0f - 1.0f;
}

bool is_stick(int slot) { return slot == kSlotX || slot == kSlotY || slot == kSlotZ || slot == kSlotRZ; }

// Drives one output from how far along its input's range the input is
// (`t`, 0..1; a button or hat direction is 0 or 1). `out` null sets the
// state quietly, for the values a pad already has when it connects.
void drive(Device& device, const Output& o, float t, bool in_range, std::vector<Event>* out) {
    if (o.kind == Output::kSlot) {
        const float value = in_range ? o.low + t * (o.high - o.low) : 0.0f;
        if (value == device.slot[o.slot]) return;
        device.slot[o.slot] = value;
        if (out != nullptr) device.slot_dirty[o.slot] = true;
        return;
    }
    const bool down = in_range && t >= 0.5f;
    if (o.kind == Output::kButton) {
        bool& was = device.key_down[o.key];
        if (was == down) return;
        was = down;
        if (out != nullptr) out->push_back(Event{Event::kButton, device.id, o.key, down ? 1.0f : 0.0f});
        return;
    }
    // The d-pad goes out as the hat, from which flush_axes() also makes
    // the DPAD key presses, as it does for a pad with a real hat.
    device.dpad[o.dpad] = down;
    const float x = static_cast<float>(device.dpad[1]) - static_cast<float>(device.dpad[3]);
    const float y = static_cast<float>(device.dpad[2]) - static_cast<float>(device.dpad[0]);
    for (auto [slot, value] : {std::pair{static_cast<int>(kSlotHatX), x}, std::pair{static_cast<int>(kSlotHatY), y}}) {
        if (device.slot[slot] == value) continue;
        device.slot[slot] = value;
        if (out != nullptr) device.slot_dirty[slot] = true;
    }
}

void apply_key(Device& device, uint16_t code, bool down, std::vector<Event>* out) {
    for (const Rule& rule : device.rules) {
        if (rule.in.kind == DbBinding::kKey && rule.in.code == code) {
            drive(device, rule.out, down ? 1.0f : 0.0f, true, out);
        }
    }
}

void apply_abs(Device& device, uint16_t code, int32_t raw, std::vector<Event>* out) {
    // A digital hat: SDL's own reading of each axis (its HandleHat), then
    // the directions the rules on that hat ask for.
    if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
        const int hat = (code - ABS_HAT0X) / 2;
        const int which = (code - ABS_HAT0X) % 2;
        bool hat_rules = false;
        for (const Rule& rule : device.rules) hat_rules |= rule.in.kind == DbBinding::kHat && rule.in.hat == hat;
        if (hat_rules) {
            const auto axis = device.axes.find(code);
            const int32_t minimum = axis == device.axes.end() ? -1 : axis->second.minimum;
            const int32_t maximum = axis == device.axes.end() ? 1 : axis->second.maximum;
            int position = 0;
            if (raw < 0 && (raw <= minimum || raw < minimum / 3)) position = -1;
            if (raw > 0 && (raw >= maximum || raw > maximum / 3)) position = 1;
            device.hat_position[hat][which] = position;
            const int x = device.hat_position[hat][0], y = device.hat_position[hat][1];
            const int mask = (y < 0 ? 1 : 0) | (x > 0 ? 2 : 0) | (y > 0 ? 4 : 0) | (x < 0 ? 8 : 0);
            for (const Rule& rule : device.rules) {
                if (rule.in.kind == DbBinding::kHat && rule.in.hat == hat) {
                    drive(device, rule.out, (mask & rule.in.mask) != 0 ? 1.0f : 0.0f, true, out);
                }
            }
        }
    }
    const auto axis = device.axes.find(code);
    if (axis == device.axes.end()) return;
    for (const Rule& rule : device.rules) {
        if (rule.in.kind != DbBinding::kAbs || rule.in.code != code) continue;
        const bool stick = rule.out.kind == Output::kSlot && is_stick(rule.out.slot);
        const float v = normalise(axis->second, raw, stick);
        const float span = rule.in.high - rule.in.low;
        const float t = span == 0.0f ? 0.0f : (v - rule.in.low) / span;
        // Outside a half axis's range the output it drives goes back to
        // rest, as SDL does when another binding takes the axis over.
        const bool in_range = t >= 0.0f && t <= 1.0f;
        drive(device, rule.out, t, in_range, out);
    }
}

// The rules for a pad the SDL database does not know: Android's own key
// layout for its buttons, and its axes by the conventions in
// gamepad_layout.h.
std::vector<Rule> evdev_rules(const PadDescription& pad) {
    std::vector<Rule> rules;
    for (uint16_t code : pad.keys) {
        const int key = android_button_for(code);
        if (key < 0) continue;
        Rule rule;
        rule.in.kind = DbBinding::kKey;
        rule.in.code = code;
        rule.out.kind = Output::kButton;
        rule.out.key = key;
        rules.push_back(rule);
    }
    std::set<uint16_t> has;
    for (const auto& [code, info] : pad.abs) {
        if (!(info.minimum == 0 && info.maximum == 0)) has.insert(code);
    }
    for (const auto& [code, slot] : axis_layout(has)) {
        Rule rule;
        rule.in.kind = DbBinding::kAbs;
        rule.in.code = code;
        rule.out.kind = Output::kSlot;
        rule.out.slot = slot;
        if (slot == kSlotLT || slot == kSlotRT) {
            rule.out.low = 0.0f;
            rule.out.high = 1.0f;
        }
        rules.push_back(rule);
    }
    return rules;
}

int g_unreadable_nodes = 0;

// Nodes already looked at that are not a pad, by the inode of their device
// node. The rescan runs on the input path, and opening an evdev node is not
// free (a USB device wakes from autosuspend): re-opening every mouse and
// keyboard each second held mouse motion back for ~190 ms at a time, on any
// system whose user can read /dev/input. A device plugged in later gets a new
// node, so it is still looked at once.
std::map<std::string, ino_t>& not_pads() {
    static std::map<std::string, ino_t> m;
    return m;
}

void open_device(const std::string& path, std::vector<Event>& out) {
    // Read-write first: rumble is an ioctl plus a write on the SAME fd,
    // so a read-only open silently costs force feedback. Falls back to
    // read-only rather than giving up, since input matters more.
    int fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    bool writable = fd >= 0;
    if (fd < 0) {
        fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        writable = false;
    }
    if (fd < 0) {
        // Not being allowed to read input devices is the ordinary case on
        // a distribution that does not put desktop users in the `input`
        // group. Said once, with the fix, rather than per device per scan.
        // Most /dev/input nodes are unreadable to an ordinary user by
        // design, so this is not news on its own; it is only worth
        // saying if no controller turned up at all. Counted here,
        // reported (once) by init().
        if (errno == EACCES) ++g_unreadable_nodes;
        struct stat st {};
        if (::stat(path.c_str(), &st) == 0) not_pads()[path] = st.st_ino;
        return;
    }
    if (!looks_like_a_gamepad(fd)) {
        struct stat st {};
        if (::fstat(fd, &st) == 0) not_pads()[path] = st.st_ino;
        ::close(fd);
        return;
    }

    Device device;
    device.fd = fd;
    device.id = next_device_id();
    char name[256] = {0};
    if (::ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0) device.name = name;
    device.type = gamepad_type_for_name(device.name);

    // Force feedback, if the pad has it and the fd can be written to.
    if (writable) {
        unsigned long ff[(FF_MAX / (8 * sizeof(unsigned long))) + 1]{};
        if (::ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 && bit_set(ff, FF_RUMBLE)) {
            device.can_rumble = true;
        }
    }

    // Everything the pad reports, for the database's numbering of it.
    PadDescription pad;
    pad.name = device.name;
    input_id id{};
    if (::ioctl(fd, EVIOCGID, &id) >= 0) {
        pad.bus = id.bustype;
        pad.vendor = id.vendor;
        pad.product = id.product;
        pad.version = id.version;
    }
    unsigned long keys[(KEY_MAX / (8 * sizeof(unsigned long))) + 1]{};
    ::ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    for (int code = 0; code < KEY_MAX; ++code) {
        if (bit_set(keys, code)) pad.keys.push_back(static_cast<uint16_t>(code));
    }
    unsigned long abs[(ABS_MAX / (8 * sizeof(unsigned long))) + 1]{};
    ::ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    std::map<uint16_t, int32_t> initial;
    for (int code = 0; code < ABS_MAX; ++code) {
        if (!bit_set(abs, code)) continue;
        input_absinfo info{};
        if (::ioctl(fd, EVIOCGABS(code), &info) < 0) continue;
        pad.abs[static_cast<uint16_t>(code)] =
            AbsInfo{info.minimum, info.maximum, info.fuzz, info.flat, info.resolution};
        device.axes[static_cast<uint16_t>(code)] = Axis{info.minimum, info.maximum, info.flat};
        initial[static_cast<uint16_t>(code)] = info.value;
    }
    device.guid = sdl_guid(pad);

    // The SDL database first: it knows this exact pad, if it is in there.
    const std::string mapping = find_mapping(mapping_source(), pad);
    for (const DbBinding& binding : parse_mapping(mapping, pad)) {
        Rule rule;
        rule.in = binding;
        if (output_for(binding, rule.out)) device.rules.push_back(rule);
    }
    device.from_db = !device.rules.empty();
    if (!device.from_db) device.rules = evdev_rules(pad);
    for (const auto& [code, value] : initial) apply_abs(device, code, value, nullptr);

    std::printf("stud-render-host: gamepad connected: %s (id %d, type %d, SDL id %s, layout from %s, "
                "rumble %s)\n",
                device.name.empty() ? "unnamed" : device.name.c_str(), device.id, device.type,
                device.guid.c_str(), device.from_db ? "the SDL database" : "its evdev axes",
                device.can_rumble ? "yes" : "no");
    std::fflush(stdout);

    // What this pad really has, before it is announced, the same order
    // the real app uses. Answered from the rules rather than assumed, so a
    // pad without (say) stick clicks does not advertise them.
    const auto drives = [&](Output::Kind kind, int which) {
        for (const Rule& rule : device.rules) {
            if (rule.out.kind != kind) continue;
            if (kind == Output::kButton && rule.out.key == which) return true;
            if (kind == Output::kSlot && rule.out.slot == which) return true;
            if (kind == Output::kDpad) return true;
        }
        return false;
    };
    // A d-pad reaches the engine as DPAD keys either way, because this
    // file synthesises them from the hat. Reporting them supported is
    // therefore honest about what Stud actually sends.
    const bool has_dpad = drives(Output::kDpad, 0) || drives(Output::kSlot, kSlotHatX) ||
                          drives(Output::kSlot, kSlotHatY);
    const std::pair<int, bool> supported_keys[] = {
        {kBtnA, drives(Output::kButton, kBtnA)},
        {kBtnB, drives(Output::kButton, kBtnB)},
        {kBtnX, drives(Output::kButton, kBtnX)},
        {kBtnY, drives(Output::kButton, kBtnY)},
        {kDpadUp, has_dpad || drives(Output::kButton, kDpadUp)},
        {kDpadDown, has_dpad || drives(Output::kButton, kDpadDown)},
        {kDpadLeft, has_dpad || drives(Output::kButton, kDpadLeft)},
        {kDpadRight, has_dpad || drives(Output::kButton, kDpadRight)},
        {kBtnR1, drives(Output::kButton, kBtnR1)},
        {kBtnL1, drives(Output::kButton, kBtnL1)},
        {kBtnThumbL, drives(Output::kButton, kBtnThumbL)},
        {kBtnThumbR, drives(Output::kButton, kBtnThumbR)},
        {kBtnSelect, drives(Output::kButton, kBtnSelect)},
        {kBtnStart, drives(Output::kButton, kBtnStart)},
        // L2/R2 are not in the real app's own query list, because on a
        // real device an Xbox-layout pad reports them only as the analog
        // trigger AXES and `hasKeys()` therefore answers false for both.
        // Stud reports them present when the triggers exist, since that
        // is what Stud genuinely delivers; Roblox binds vehicle throttle
        // to ButtonR2/ButtonL2, and a pad that reports neither has nothing
        // for that binding to attach to.
        {kBtnL2, drives(Output::kButton, kBtnL2) || drives(Output::kSlot, kSlotLT)},
        {kBtnR2, drives(Output::kButton, kBtnR2) || drives(Output::kSlot, kSlotRT)},
    };
    for (const auto& [key, present] : supported_keys) {
        out.push_back(Event{Event::kSupportedKey, device.id, key, present ? 1.0f : 0.0f,
                            static_cast<float>(device.type)});
    }
    for (int slot = 0; slot < kSlotCount; ++slot) {
        const bool present = drives(Output::kSlot, slot) ||
                             ((slot == kSlotHatX || slot == kSlotHatY) && drives(Output::kDpad, 0));
        out.push_back(Event{Event::kSupportedAxis, device.id, kSlotAxisId[slot],
                            present ? 1.0f : 0.0f, static_cast<float>(device.type)});
    }

    // v1 carries whether this pad can rumble, so Process B can answer
    // the engine's haptics question without another round trip.
    out.push_back(Event{Event::kConnect, device.id, 0, static_cast<float>(device.type),
                        device.can_rumble ? 1.0f : 0.0f});
    devices()[path] = std::move(device);
}

// Emits whatever changed since the last SYN_REPORT, in the exact shape
// the engine's own entry point takes.
//
// `nativeGamepadAxisEvent(deviceId, axis, f0, f1, f2)` is a VECTOR call,
// not one scalar per axis. The app's own caller
// sends a stick's BOTH components on BOTH of its axis ids, with the
// vertical component NEGATED, and sends a trigger or hat value in the
// THIRD float with the first two zero. Stud used to send
// `(value, 0, 0)` for everything, which meant the engine read every
// stick's vertical component as zero and never saw a trigger or d-pad
// axis at all.
void flush_axes(Device& device, std::vector<Event>& out) {
    const auto emit = [&](int axis_id, float f0, float f1, float f2) {
        out.push_back(Event{Event::kAxis, device.id, axis_id, f0, f1, f2});
    };

    if (device.slot_dirty[kSlotX] || device.slot_dirty[kSlotY]) {
        const float x = device.slot[kSlotX];
        const float y = -device.slot[kSlotY];
        emit(kAxisX, x, y, 0.0f);
        emit(kAxisY, x, y, 0.0f);
    }
    if (device.slot_dirty[kSlotZ] || device.slot_dirty[kSlotRZ]) {
        const float z = device.slot[kSlotZ];
        const float rz = -device.slot[kSlotRZ];
        emit(kAxisZ, z, rz, 0.0f);
        emit(kAxisRZ, z, rz, 0.0f);
    }
    if (device.slot_dirty[kSlotLT]) {
        emit(kAxisLTrigger, 0.0f, 0.0f, device.slot[kSlotLT]);
    }
    if (device.slot_dirty[kSlotRT]) {
        emit(kAxisRTrigger, 0.0f, 0.0f, device.slot[kSlotRT]);
    }
    if (device.slot_dirty[kSlotHatX]) {
        emit(kAxisHatX, 0.0f, 0.0f, device.slot[kSlotHatX]);
    }
    if (device.slot_dirty[kSlotHatY]) {
        // Negated, same as the real caller: the engine's hat Y is
        // positive upward, evdev's is negative upward.
        emit(kAxisHatY, 0.0f, 0.0f, -device.slot[kSlotHatY]);
    }

    // A trigger is also a button.
    //
    // The engine maps axis 17/18 to KeyCode ButtonL2/ButtonR2 (confirmed
    // in its own jump table) but an analog position alone only ever
    // produces an InputState.Change, and Roblox binds tool activation
    // and vehicle throttle to ButtonR2/ButtonL2 as a BUTTON, which needs
    // a real Begin. A pad that reports BTN_TL2/BTN_TR2 gives Android both
    // halves for free; one whose triggers are axes only (an Xbox-layout
    // pad, this 8BitDo included) gives neither, so the press is
    // synthesised here from the analog value.
    //
    // Hysteresis, not one threshold: a trigger resting near the trip
    // point would otherwise chatter press/release every report.
    {
        constexpr float kPress = 0.5f;
        constexpr float kRelease = 0.4f;
        const std::pair<int, int> triggers[] = {{kSlotLT, kBtnL2}, {kSlotRT, kBtnR2}};
        for (int i = 0; i < 2; ++i) {
            const auto [slot, key] = triggers[i];
            if (!device.slot_dirty[slot]) continue;
            const float value = device.slot[slot];
            const bool now = device.trigger_held[i] ? value > kRelease : value >= kPress;
            if (now == device.trigger_held[i]) continue;
            device.trigger_held[i] = now;
            out.push_back(Event{Event::kButton, device.id, key, now ? 1.0f : 0.0f});
        }
    }

    // The hat is also the d-pad. Real Android synthesises the key presses
    // and Roblox's own menu navigation listens for them rather than for
    // the axis.
    for (int slot : {kSlotHatX, kSlotHatY}) {
        if (!device.slot_dirty[slot]) continue;
        const float value = device.slot[slot];
        const int negative = slot == kSlotHatX ? kDpadLeft : kDpadUp;
        const int positive = slot == kSlotHatX ? kDpadRight : kDpadDown;
        for (auto [key, now] : {std::pair{negative, value <= -0.5f},
                                std::pair{positive, value >= 0.5f}}) {
            bool& was = device.hat_key_state[key];
            if (was == now) continue;
            was = now;
            out.push_back(Event{Event::kButton, device.id, key, now ? 1.0f : 0.0f});
        }
    }

    for (bool& dirty : device.slot_dirty) dirty = false;
}

void read_device(Device& device, std::vector<Event>& out, bool& died) {
    input_event events[64];
    for (;;) {
        const ssize_t n = ::read(device.fd, events, sizeof(events));
        if (n <= 0) {
            // ENODEV is the pad being unplugged mid-read.
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                died = true;
                device.death_errno = errno;
            }
            // Anything read but not yet reported still has to go out,
            // a batch can end on a report boundary the loop never sees.
            flush_axes(device, out);
            return;
        }
        const size_t count = static_cast<size_t>(n) / sizeof(input_event);
        for (size_t i = 0; i < count; ++i) {
            const input_event& e = events[i];
            if (e.type == EV_SYN && e.code == SYN_REPORT) {
                flush_axes(device, out);
            } else if (e.type == EV_KEY) {
                // Value 2 is the kernel's auto-repeat; the engine wants
                // edges, and a held button is not a new press.
                if (e.value != 2) apply_key(device, e.code, e.value != 0, &out);
            } else if (e.type == EV_ABS) {
                apply_abs(device, e.code, e.value, &out);
            }
        }
    }
}

void scan(std::vector<Event>& out) {
    DIR* dir = ::opendir("/dev/input");
    if (dir == nullptr) return;
    while (const dirent* entry = ::readdir(dir)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) continue;
        const std::string path = std::string("/dev/input/") + entry->d_name;
        if (devices().count(path) != 0) continue;
        auto known = not_pads().find(path);
        if (known != not_pads().end()) {
            struct stat st {};
            if (::stat(path.c_str(), &st) == 0 && st.st_ino == known->second) continue;
            not_pads().erase(known);
        }
        open_device(path, out);
    }
    ::closedir(dir);
}

}  // namespace

// Events produced before anything was draining the queue.
//
// init() runs at startup, long before Process B has connected and begun
// polling, so a controller that was already plugged in produced its
// connect event into nothing and the engine was never told it existed,
// only ones plugged in AFTER launch worked. They are held here and handed
// over on the first poll instead.
std::vector<Event>& pending() {
    static std::vector<Event> p;
    return p;
}

void init() {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    scan(pending());
    if (!devices().empty()) return;
    if (g_unreadable_nodes > 0) {
        std::printf("stud-render-host: no game controllers found (%d input device(s) could not "
                    "be read, if a controller is connected, this user needs read access to "
                    "/dev/input, usually via the `input` group)\n",
                    g_unreadable_nodes);
    } else {
        std::printf("stud-render-host: no game controllers found\n");
    }
    std::fflush(stdout);
}

// Plays a rumble on one pad, or stops it when both magnitudes are zero.
//
// The kernel's own rumble effect takes two magnitudes, the heavy and
// light motors a real pad has, so an effect is uploaded once per pad
// and re-uploaded (same id) whenever the strength changes, which is what
// the API is for. Playing it is an ordinary write of an EV_FF event.
bool set_rumble(int device_id, float strong, float weak, int duration_ms) {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    Device* found = nullptr;
    for (auto& [path, candidate] : devices()) {
        (void)path;
        if (candidate.id == device_id) found = &candidate;
    }
    if (found == nullptr) return false;
    Device& device = *found;
    if (!device.can_rumble) return false;
    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    strong = clamp01(strong);
    weak = clamp01(weak);

    if (strong <= 0.0f && weak <= 0.0f) {
        if (device.rumble_effect >= 0) {
            input_event stop{};
            stop.type = EV_FF;
            stop.code = static_cast<uint16_t>(device.rumble_effect);
            stop.value = 0;
            (void)::write(device.fd, &stop, sizeof(stop));
        }
        return true;
    }

    ff_effect effect{};
    effect.type = FF_RUMBLE;
    effect.id = static_cast<int16_t>(device.rumble_effect);  // -1 uploads a new one
    effect.replay.length = static_cast<uint16_t>(duration_ms > 0 ? duration_ms : 0);
    effect.u.rumble.strong_magnitude = static_cast<uint16_t>(strong * 65535.0f);
    effect.u.rumble.weak_magnitude = static_cast<uint16_t>(weak * 65535.0f);
    if (::ioctl(device.fd, EVIOCSFF, &effect) < 0) {
        // A re-upload can fail if the pad went away between calls; drop
        // the id so the next attempt uploads a fresh effect.
        device.rumble_effect = -1;
        return false;
    }
    device.rumble_effect = effect.id;

    input_event play{};
    play.type = EV_FF;
    play.code = static_cast<uint16_t>(effect.id);
    play.value = 1;
    return ::write(device.fd, &play, sizeof(play)) == sizeof(play);
}

void poll(std::vector<Event>& out) {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    if (!pending().empty()) {
        out.insert(out.end(), pending().begin(), pending().end());
        pending().clear();
    }
    // Re-scan about once a second, which is what makes plugging a pad in
    // mid-game work without libudev.
    static auto last_scan = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last_scan > std::chrono::seconds(1)) {
        last_scan = now;
        scan(out);
    }

    for (auto it = devices().begin(); it != devices().end();) {
        bool died = false;
        // A pad that goes away usually takes its device node with it,
        // a wireless receiver destroys the node when the pad powers off.
        // Reading it may not fail for a while, so the node's own absence
        // is what says the pad is gone. Without this the engine keeps
        // believing a controller is attached long after it is not, which
        // leaves the app in controller mode with nothing to drive it.
        if (::access(it->first.c_str(), F_OK) != 0) {
            died = true;
            it->second.death_errno = errno;
        }
        if (!died) read_device(it->second, out, died);
        if (died) {
            std::printf("stud-render-host: gamepad disconnected: %s (id %d, %s)\n",
                        it->second.name.empty() ? "unnamed" : it->second.name.c_str(),
                        it->second.id, std::strerror(it->second.death_errno));
            std::fflush(stdout);
            out.push_back(Event{Event::kDisconnect, it->second.id, 0, 0.0f});
            ::close(it->second.fd);
            it = devices().erase(it);
        } else {
            ++it;
        }
    }
}

int device_count() {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    return static_cast<int>(devices().size());
}

}  // namespace stud::render_host::gamepad
