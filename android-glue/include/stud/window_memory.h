#pragma once

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>

// How the game window was left, for the next launch to open the same way.
// The size is the window's own in logical units, kept while it is neither
// maximized, tiled nor fullscreen, so that leaving either of those next time
// lands on the size the user chose rather than the desktop's.
namespace stud::android_glue {

enum class WindowMode : int { Windowed = 0, Maximized = 1, Fullscreen = 2 };

struct WindowMemory {
    WindowMode mode = WindowMode::Windowed;
    int32_t width = 0;
    int32_t height = 0;
};

inline std::string window_memory_text(const WindowMemory& m) {
    std::ostringstream out;
    out << static_cast<int>(m.mode) << ' ' << m.width << ' ' << m.height << '\n';
    return out.str();
}

// Empty for anything that is not exactly what window_memory_text() writes:
// the file is the user's to delete or damage, and the default window is
// always a fine answer.
inline std::optional<WindowMemory> parse_window_memory(const std::string& text) {
    std::istringstream in(text);
    int mode = -1;
    WindowMemory m;
    if (!(in >> mode >> m.width >> m.height)) return std::nullopt;
    if (mode < 0 || mode > 2 || m.width <= 0 || m.height <= 0) return std::nullopt;
    m.mode = static_cast<WindowMode>(mode);
    return m;
}

}  // namespace stud::android_glue
