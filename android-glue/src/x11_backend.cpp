#include "x11_backend.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>

namespace stud::android_glue::x11 {

namespace {

// Every Xlib entry point this backend uses, resolved from libX11 at
// runtime. Declared with the real header's own types, so the compiler
// checks each signature -- what dlopen buys here is only that a machine
// without libX11 still runs Stud on Wayland, not a hand-written ABI.
struct Xlib {
    void* handle = nullptr;
    Display* (*OpenDisplay)(const char*) = nullptr;
    int (*CloseDisplay)(Display*) = nullptr;
    Window (*CreateSimpleWindow)(Display*, Window, int, int, unsigned int, unsigned int,
                                 unsigned int, unsigned long, unsigned long) = nullptr;
    int (*DestroyWindow)(Display*, Window) = nullptr;
    int (*MapWindow)(Display*, Window) = nullptr;
    int (*SelectInput)(Display*, Window, long) = nullptr;
    int (*StoreName)(Display*, Window, const char*) = nullptr;
    int (*SetClassHint)(Display*, Window, XClassHint*) = nullptr;
    Atom (*InternAtom)(Display*, const char*, Bool) = nullptr;
    Status (*SetWMProtocols)(Display*, Window, Atom*, int) = nullptr;
    int (*Flush)(Display*) = nullptr;
    int (*Pending)(Display*) = nullptr;
    int (*NextEvent)(Display*, XEvent*) = nullptr;
    int (*ChangeProperty)(Display*, Window, Atom, Atom, int, int, const unsigned char*,
                          int) = nullptr;
    int (*ConnectionNumber_)(Display*) = nullptr;
};

Xlib& xlib() {
    static Xlib x;
    return x;
}

Display* g_display = nullptr;
Window g_window = 0;
Atom g_wm_delete = 0;
std::atomic<bool> g_close_requested{false};
// X11's own answer to "can anybody see this": the window is either
// unmapped (minimised, another workspace) or fully obscured by other
// windows. VisibilityNotify is what the server sends for the second.
std::atomic<bool> g_visible{true};
// FocusIn/FocusOut: X11's answer to whether this is the window being
// used, which is a different question from whether it can be seen.
std::atomic<bool> g_focused{true};
std::atomic<int32_t> g_width{0};
std::atomic<int32_t> g_height{0};

bool load_xlib() {
    Xlib& x = xlib();
    if (x.handle != nullptr) return true;
    x.handle = ::dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (x.handle == nullptr) {
        std::fprintf(stderr, "stud: android-glue: no libX11.so.6 (%s)\n", ::dlerror());
        return false;
    }
    bool ok = true;
    auto sym = [&](const char* name) {
        void* p = ::dlsym(x.handle, name);
        if (p == nullptr) {
            std::fprintf(stderr, "stud: android-glue: libX11 has no %s\n", name);
            ok = false;
        }
        return p;
    };
#define LOAD(field, name) \
    x.field = reinterpret_cast<decltype(x.field)>(sym(name))
    LOAD(OpenDisplay, "XOpenDisplay");
    LOAD(CloseDisplay, "XCloseDisplay");
    LOAD(CreateSimpleWindow, "XCreateSimpleWindow");
    LOAD(DestroyWindow, "XDestroyWindow");
    LOAD(MapWindow, "XMapWindow");
    LOAD(SelectInput, "XSelectInput");
    LOAD(StoreName, "XStoreName");
    LOAD(SetClassHint, "XSetClassHint");
    LOAD(InternAtom, "XInternAtom");
    LOAD(SetWMProtocols, "XSetWMProtocols");
    LOAD(Flush, "XFlush");
    LOAD(Pending, "XPending");
    LOAD(NextEvent, "XNextEvent");
    LOAD(ChangeProperty, "XChangeProperty");
    LOAD(ConnectionNumber_, "XConnectionNumber");
#undef LOAD
    if (!ok) {
        ::dlclose(x.handle);
        x.handle = nullptr;
    }
    return ok;
}

}  // namespace

bool available() {
    static const bool answer = [] {
        const char* display_name = std::getenv("DISPLAY");
        if (display_name == nullptr || *display_name == '\0') return false;
        if (!load_xlib()) return false;
        // Opened once and kept: this is the connection the window, EGL and
        // Vulkan all use. Closing and reopening would hand out a stale
        // Display* to whichever of them asked first.
        g_display = xlib().OpenDisplay(nullptr);
        if (g_display == nullptr) {
            std::fprintf(stderr, "stud: android-glue: DISPLAY=%s is set but XOpenDisplay failed\n",
                         display_name);
            return false;
        }
        return true;
    }();
    return answer;
}

bool create_window(int32_t width, int32_t height) {
    if (!available()) return false;
    if (g_window != 0) return true;
    Xlib& x = xlib();

    const int screen = DefaultScreen(g_display);
    g_window = x.CreateSimpleWindow(g_display, RootWindow(g_display, screen), 0, 0,
                                     static_cast<unsigned int>(width),
                                     static_cast<unsigned int>(height), 0,
                                     BlackPixel(g_display, screen),
                                     BlackPixel(g_display, screen));
    if (g_window == 0) {
        std::fprintf(stderr, "stud: android-glue: XCreateSimpleWindow failed\n");
        return false;
    }

    // StructureNotify is what carries ConfigureNotify (the resizes this
    // backend exists to notice) and the map/unmap pair. Input masks are
    // added when the input backend lands; asking for them now would
    // queue events nothing drains.
    x.SelectInput(g_display, g_window,
                  StructureNotifyMask | VisibilityChangeMask | FocusChangeMask);

    x.StoreName(g_display, g_window, "Stud");
    // WM_CLASS is X11's answer to Wayland's app_id: it is what ties this
    // window to the desktop entry, and therefore to an icon and a
    // taskbar name. Both fields, instance then class, as ICCCM wants.
    char instance_name[] = "stud";
    char class_name[] = STUD_APP_ID;
    XClassHint hint{instance_name, class_name};
    x.SetClassHint(g_display, g_window, &hint);

    // Without this the window manager's close button kills the
    // connection outright instead of telling Stud, and the engine never
    // gets to shut down.
    g_wm_delete = x.InternAtom(g_display, "WM_DELETE_WINDOW", False);
    x.SetWMProtocols(g_display, g_window, &g_wm_delete, 1);

    g_width.store(width);
    g_height.store(height);

    x.MapWindow(g_display, g_window);
    x.Flush(g_display);

    std::printf("stud: android-glue: X11 window %lux%lu, WM_CLASS=\"%s\", title=\"Stud\"\n",
                static_cast<unsigned long>(width), static_cast<unsigned long>(height),
                STUD_APP_ID);
    std::fflush(stdout);
    return true;
}

void* display() { return g_display; }

unsigned long window() { return g_window; }

void pump() {
    if (g_display == nullptr || g_window == 0) return;
    Xlib& x = xlib();
    // Only what has already arrived: XNextEvent blocks, and this is
    // called from the same loop that services the render socket.
    while (x.Pending(g_display) > 0) {
        XEvent event{};
        x.NextEvent(g_display, &event);
        switch (event.type) {
            case ConfigureNotify: {
                const auto& configure = event.xconfigure;
                if (configure.width > 0 && configure.height > 0) {
                    g_width.store(configure.width);
                    g_height.store(configure.height);
                }
                break;
            }
            case VisibilityNotify: {
                const bool visible = event.xvisibility.state != VisibilityFullyObscured;
                if (visible != g_visible.exchange(visible)) {
                    std::printf("stud: android-glue: window %s\n",
                                 visible ? "visible again" : "no longer visible (obscured)");
                    std::fflush(stdout);
                }
                break;
            }
            case UnmapNotify:
                if (g_visible.exchange(false)) {
                    std::printf("stud: android-glue: window no longer visible (unmapped)\n");
                    std::fflush(stdout);
                }
                break;
            case MapNotify:
                if (!g_visible.exchange(true)) {
                    std::printf("stud: android-glue: window visible again\n");
                    std::fflush(stdout);
                }
                break;
            case FocusIn:
                if (!g_focused.exchange(true)) {
                    std::printf("stud: android-glue: window focused\n");
                    std::fflush(stdout);
                }
                break;
            case FocusOut:
                if (g_focused.exchange(false)) {
                    std::printf("stud: android-glue: window in the background\n");
                    std::fflush(stdout);
                }
                break;
            case ClientMessage:
                if (static_cast<Atom>(event.xclient.data.l[0]) == g_wm_delete) {
                    g_close_requested.store(true);
                }
                break;
            default:
                break;
        }
    }
}

bool close_requested() { return g_close_requested.load(); }

bool visible() { return g_visible.load(); }

bool focused() { return g_focused.load(); }

int connection_fd() {
    if (g_display == nullptr) return -1;
    return xlib().ConnectionNumber_(g_display);
}

int32_t width() { return g_width.load(); }

int32_t height() { return g_height.load(); }

}  // namespace stud::android_glue::x11
