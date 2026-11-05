// The window's remembered state reads back as written, and anything else in
// that file means the default window.

#include "check.h"
#include "stud/window_memory.h"

using namespace stud::android_glue;

int main() {
    for (const WindowMode mode : {WindowMode::Windowed, WindowMode::Maximized, WindowMode::Fullscreen}) {
        const auto back = parse_window_memory(window_memory_text({mode, 1382, 777}));
        check(back.has_value() && back->mode == mode && back->width == 1382 && back->height == 777,
              "a saved state reads back");
    }
    for (const char* bad : {"", "1", "1 800", "3 800 600", "-1 800 600", "0 0 600", "0 800 -1", "max 800 600"}) {
        check(!parse_window_memory(bad).has_value(), std::string("refused: \"") + bad + "\"");
    }
    return 0;
}
