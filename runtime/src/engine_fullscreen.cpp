#include "engine_fullscreen.h"

#include <link.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "stud/trap_recovery.h"

namespace stud::runtime {
namespace {

struct Code {
    const uint8_t* begin = nullptr;
    size_t size = 0;
};

// The engine's executable segment, which holds its read-only strings too.
int find_segment(dl_phdr_info* info, size_t, void* data) {
    auto* wanted = static_cast<std::pair<const uint8_t*, Code>*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0) continue;
        const auto* begin = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        if (wanted->first >= begin && wanted->first < begin + ph.p_memsz) {
            wanted->second = {begin, ph.p_memsz};
            return 1;
        }
    }
    return 0;
}

// Where `name` is stored, as a whole string: the same text inside a longer one
// does not count, and neither does a second copy.
const uint8_t* find_name(const Code& code, std::string_view name) {
    const std::string_view all(reinterpret_cast<const char*>(code.begin), code.size);
    const size_t at = all.find(std::string(1, '\0').append(name).append(1, '\0'));
    if (at == std::string_view::npos) return nullptr;
    return code.begin + at + 1;
}

// `lea r64, [rip + disp32]`: REX.W, 8D, ModRM with mod 0 and rm 5.
constexpr size_t kLeaSize = 7;
const uint8_t* lea_target(const uint8_t* at) {
    if ((at[0] & 0xfb) != 0x48 || at[1] != 0x8d || (at[2] & 0xc7) != 0x05) return nullptr;
    int32_t disp = 0;
    std::memcpy(&disp, at + 3, sizeof(disp));
    return at + kLeaSize + disp;
}

// The one instruction taking a string's address, or null for none or several.
const uint8_t* only_lea_of(const Code& code, const uint8_t* target) {
    const uint8_t* found = nullptr;
    for (size_t i = kLeaSize; i + kLeaSize < code.size; ++i) {
        if (code.begin[i + 1] != 0x8d) continue;
        if (lea_target(code.begin + i) != target) continue;
        if (found != nullptr) return nullptr;
        found = code.begin + i;
    }
    return found;
}

// The engine registers a Lua method by loading its function into rsi and its
// name into rdx, in that order.
const uint8_t* method_named(const uint8_t* name_lea) {
    const uint8_t* function = name_lea - kLeaSize;
    return function[2] == 0x35 ? lea_target(function) : nullptr;  // ModRM of rsi
}

using SettingsGetter = uint8_t* (*)();
SettingsGetter g_settings = nullptr;
int32_t g_value_offset = 0;
using SettingsSetter = void (*)(void*, bool);
SettingsSetter g_setter = nullptr;
std::atomic<bool> g_wanted{false};

// InFullScreen, whole:  push rbp; mov rbp,rsp; call getter;
//                       mov al,[rax+offset]; pop rbp; ret
bool decode_value(const uint8_t* fn) {
    static const uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xe5, 0xe8};
    static const uint8_t kRead[] = {0x8a, 0x80};
    static const uint8_t kReturn[] = {0x5d, 0xc3};
    constexpr size_t kCallEnd = sizeof(kPrologue) + sizeof(int32_t);
    constexpr size_t kOffsetAt = kCallEnd + sizeof(kRead);
    constexpr size_t kReturnAt = kOffsetAt + sizeof(int32_t);
    if (std::memcmp(fn, kPrologue, sizeof(kPrologue)) != 0 ||
        std::memcmp(fn + kCallEnd, kRead, sizeof(kRead)) != 0 ||
        std::memcmp(fn + kReturnAt, kReturn, sizeof(kReturn)) != 0) {
        return false;
    }
    int32_t call = 0;
    std::memcpy(&call, fn + sizeof(kPrologue), sizeof(call));
    g_settings = reinterpret_cast<SettingsGetter>(const_cast<uint8_t*>(fn + kCallEnd + call));
    std::memcpy(&g_value_offset, fn + kOffsetAt, sizeof(g_value_offset));
    return true;
}

// The value's own setter, which is what tells the menu: the only function that
// calls the getter, compares the value with its bool, and stores it:
//     call getter; mov [rbp-n],bl; cmp [rax+offset],bl; je +n; mov [rax+offset],bl
// Null if this build has none, or several.
SettingsSetter find_setter(const Code& code) {
    uint8_t shape[14] = {0x38, 0x98, 0, 0, 0, 0, 0x74, 0, 0x88, 0x98, 0, 0, 0, 0};
    std::memcpy(shape + 2, &g_value_offset, sizeof(g_value_offset));
    std::memcpy(shape + 10, &g_value_offset, sizeof(g_value_offset));
    constexpr size_t kJumpSize = 1;  // the je's distance, which varies
    constexpr size_t kCallToCompare = 8;  // call rel32, then the three-byte store
    constexpr size_t kMostPrologue = 64;
    constexpr uint8_t kPrologue[] = {0x55, 0x48, 0x89, 0xe5};
    SettingsSetter found = nullptr;
    const uint8_t* const end = code.begin + code.size - sizeof(shape);
    for (const uint8_t* at = code.begin + kMostPrologue + kCallToCompare; at < end; ++at) {
        at = static_cast<const uint8_t*>(std::memchr(at, shape[0], static_cast<size_t>(end - at)));
        if (at == nullptr) break;
        if (std::memcmp(at, shape, 7) != 0 || std::memcmp(at + 7 + kJumpSize, shape + 8, 6) != 0) {
            continue;
        }
        const uint8_t* call = at - kCallToCompare;
        if (call[0] != 0xe8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, call + 1, sizeof(rel));
        if (reinterpret_cast<SettingsGetter>(const_cast<uint8_t*>(call + 5 + rel)) != g_settings) continue;
        const uint8_t* start = call;
        while (start > call - kMostPrologue && std::memcmp(start, kPrologue, sizeof(kPrologue)) != 0) --start;
        if (std::memcmp(start, kPrologue, sizeof(kPrologue)) != 0 || found != nullptr) return nullptr;
        found = reinterpret_cast<SettingsSetter>(const_cast<uint8_t*>(start));
    }
    return found;
}

// ToggleFullscreen looks up the action "ToggleFullScreen" within its first few
// instructions; a function that does not is not the one wanted.
bool looks_like_toggle(const uint8_t* fn, const uint8_t* action_name) {
    constexpr size_t kPrologueBytes = 64;
    for (size_t i = 0; i < kPrologueBytes; ++i) {
        if (lea_target(fn + i) == action_name) return true;
    }
    return false;
}

int g_counter = -1;
uint64_t g_seen = 0;

void start_counting(const uint8_t* fn) {
    perf_event_attr attr{};
    attr.type = PERF_TYPE_BREAKPOINT;
    attr.size = sizeof(attr);
    attr.bp_type = HW_BREAKPOINT_X;
    attr.bp_addr = reinterpret_cast<uintptr_t>(fn);
    attr.bp_len = sizeof(long);
    attr.inherit = 1;  // every thread this one goes on to create
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    g_counter = static_cast<int>(
        ::syscall(__NR_perf_event_open, &attr, 0, -1, -1, PERF_FLAG_FD_CLOEXEC));
    if (g_counter < 0) {
        std::printf("stud: the in-game Fullscreen option stays inactive: the system refused the "
                    "counter (%s)\n", std::strerror(errno));
    }
}

int sync_value() {
    uint8_t* settings = g_settings();
    if (settings == nullptr) return 0;
    const bool wanted = g_wanted.load();
    if ((settings[g_value_offset] != 0) != wanted) g_setter(settings, wanted);
    return 1;
}

}  // namespace

void init_engine_fullscreen(const void* inside_engine) {
    std::pair<const uint8_t*, Code> wanted{static_cast<const uint8_t*>(inside_engine), {}};
    if (inside_engine == nullptr || dl_iterate_phdr(find_segment, &wanted) == 0) return;
    const Code& code = wanted.second;
    const auto found = [&code](std::string_view name) {
        const uint8_t* text = find_name(code, name);
        return text != nullptr ? only_lea_of(code, text) : nullptr;
    };
    if (const uint8_t* lea = found("InFullScreen")) {
        const uint8_t* fn = method_named(lea);
        if (fn != nullptr && !decode_value(fn)) g_settings = nullptr;
    }
    if (g_settings == nullptr) {
        std::printf("stud: the in-game Fullscreen option will not follow the window: this build's "
                    "InFullScreen has an unknown shape\n");
    } else {
        g_setter = find_setter(code);
        if (g_setter == nullptr) {
            std::printf("stud: the in-game Fullscreen option will not follow the window: this "
                        "build's setter has an unknown shape\n");
        }
    }
    const uint8_t* action = find_name(code, "ToggleFullScreen");
    const uint8_t* lea = found("ToggleFullscreen");
    const uint8_t* fn = lea != nullptr ? method_named(lea) : nullptr;
    if (fn != nullptr && action != nullptr && looks_like_toggle(fn, action)) {
        start_counting(fn);
    } else {
        std::printf("stud: the in-game Fullscreen option stays inactive: this build's "
                    "ToggleFullscreen has an unknown shape\n");
    }
    std::fflush(stdout);
}

bool engine_fullscreen_tick(bool window_fullscreen) {
    if (g_setter != nullptr) {
        g_wanted.store(window_fullscreen);
        int ok = 0;
        if (!stud::jni_bridge::call_trapping_abort_with_result(&sync_value, ok)) {
            g_setter = nullptr;
            std::printf("stud: reading the engine's fullscreen value trapped, disabled for this "
                        "run\n");
            std::fflush(stdout);
        }
    }
    if (g_counter < 0) return false;
    uint64_t count = 0;
    if (::read(g_counter, &count, sizeof(count)) != sizeof(count)) return false;
    const bool used = count != g_seen;
    g_seen = count;
    return used;
}

}  // namespace stud::runtime
