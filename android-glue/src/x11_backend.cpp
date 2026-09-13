#include "x11_backend.h"

#include "stud/android_glue.h"
#include "stud_window_icon.h"

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
    Pixmap (*CreateBitmapFromData)(Display*, Drawable, const char*, unsigned int,
                                   unsigned int) = nullptr;
    Cursor (*CreatePixmapCursor)(Display*, Pixmap, Pixmap, XColor*, XColor*, unsigned int,
                                 unsigned int) = nullptr;
    int (*DefineCursor)(Display*, Window, Cursor) = nullptr;
    int (*FreePixmap)(Display*, Pixmap) = nullptr;
    int (*FreeCursor)(Display*, Cursor) = nullptr;
    int (*GrabPointer)(Display*, Window, Bool, unsigned int, int, int, Window, Cursor,
                       Time) = nullptr;
    int (*UngrabPointer)(Display*, Time) = nullptr;
    int (*WarpPointer)(Display*, Window, Window, int, int, unsigned int, unsigned int, int,
                       int) = nullptr;
};

Xlib& xlib() {
    static Xlib x;
    return x;
}

Display* g_display = nullptr;
Window g_window = 0;
std::atomic<bool> g_pointer_locked{false};
// Set while a warp of our own is in flight, so the MotionNotify it
// generates is not read as the user moving the mouse -- without this the
// camera receives the warp back to centre as a second, opposite delta
// and mouse look cancels itself out.
bool g_ignore_next_motion = false;
// The last unlocked pointer position, in window coordinates: where a
// drag began, what every event reports while the drag lasts, and where
// the pointer is put back when it ends.
float g_pointer_x = 0.0f;
float g_pointer_y = 0.0f;
// The previous raw position while locked, which is what the deltas are
// measured against. Not the window centre: warping on every motion is
// what put the cursor in the middle of the screen.
int g_locked_last_x = 0;
int g_locked_last_y = 0;
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
    LOAD(CreateBitmapFromData, "XCreateBitmapFromData");
    LOAD(CreatePixmapCursor, "XCreatePixmapCursor");
    LOAD(DefineCursor, "XDefineCursor");
    LOAD(FreePixmap, "XFreePixmap");
    LOAD(FreeCursor, "XFreeCursor");
    LOAD(GrabPointer, "XGrabPointer");
    LOAD(UngrabPointer, "XUngrabPointer");
    LOAD(WarpPointer, "XWarpPointer");
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

    // StructureNotify carries ConfigureNotify (resizes) and the
    // map/unmap pair; the rest is real input, which this backend now
    // delivers into the same queue the Wayland listeners feed.
    x.SelectInput(g_display, g_window,
                  StructureNotifyMask | VisibilityChangeMask | FocusChangeMask |
                      KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                      PointerMotionMask | EnterWindowMask | LeaveWindowMask);

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

    // The engine draws its own cursor, in-frame, on the app shell and
    // in-game alike -- the same reason the Wayland path passes a null
    // cursor surface. Without this X11 shows the desktop's arrow on top
    // of Roblox's own, which is two cursors.
    //
    // X11 has no "no cursor": the way to have none is a cursor made
    // from an empty 1x1 bitmap, which is what every application that
    // hides the pointer does.
    if (x.CreateBitmapFromData != nullptr && x.CreatePixmapCursor != nullptr &&
        x.DefineCursor != nullptr) {
        const char empty[8] = {0};
        Pixmap bitmap = x.CreateBitmapFromData(g_display, g_window, empty, 1, 1);
        if (bitmap != 0) {
            XColor black{};
            Cursor blank = x.CreatePixmapCursor(g_display, bitmap, bitmap, &black, &black, 0, 0);
            if (blank != 0) {
                x.DefineCursor(g_display, g_window, blank);
                if (x.FreeCursor != nullptr) x.FreeCursor(g_display, blank);
            }
            if (x.FreePixmap != nullptr) x.FreePixmap(g_display, bitmap);
        }
    }

    // The taskbar/titlebar icon. WM_CLASS above is enough for a desktop
    // that can find Stud's .desktop file, but nothing guarantees one is
    // installed -- an AppImage run straight from a download has none --
    // and then the window has no icon at all. _NET_WM_ICON carries the
    // pixels themselves, so it works either way.
    {
        const Atom net_wm_icon = x.InternAtom(g_display, "_NET_WM_ICON", False);
        const Atom cardinal = x.InternAtom(g_display, "CARDINAL", False);
        if (net_wm_icon != 0 && cardinal != 0) {
            x.ChangeProperty(g_display, g_window, net_wm_icon, cardinal, 32, PropModeReplace,
                             reinterpret_cast<const unsigned char*>(kStudWindowIcon),
                             static_cast<int>(kStudWindowIconLength));
        }
    }

    x.MapWindow(g_display, g_window);
    x.Flush(g_display);

    std::printf("stud: android-glue: X11 window %lux%lu, WM_CLASS=\"%s\", title=\"Stud\"\n",
                static_cast<unsigned long>(width), static_cast<unsigned long>(height),
                STUD_APP_ID);
    std::fflush(stdout);
    return true;
}

void set_pointer_locked(bool locked) {
    if (g_display == nullptr || g_window == 0) return;
    if (locked == g_pointer_locked.load()) return;
    Xlib& x = xlib();
    if (locked) {
        if (x.GrabPointer == nullptr || x.WarpPointer == nullptr) return;
        // Owner-events so the window keeps receiving its own events
        // normally; confined to the window so nothing outside sees the
        // drag. Async modes: a synchronous grab would require replaying
        // every event by hand.
        const int result =
            x.GrabPointer(g_display, g_window, True,
                          ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                          GrabModeAsync, g_window, None, CurrentTime);
        if (result != GrabSuccess) return;
        g_pointer_locked.store(true);
        // Measure the first delta from where the drag actually began --
        // no warp here. Moving the pointer at the moment of the lock is
        // visible as the cursor jumping, and there is no reason for it.
        g_locked_last_x = static_cast<int>(g_pointer_x);
        g_locked_last_y = static_cast<int>(g_pointer_y);
        g_ignore_next_motion = false;
        if (x.Flush != nullptr) x.Flush(g_display);
        return;
    }
    g_pointer_locked.store(false);
    g_ignore_next_motion = false;
    if (x.UngrabPointer != nullptr) x.UngrabPointer(g_display, CurrentTime);
    // Put the pointer back where the drag started, which is where the
    // engine's own cursor has stayed -- the Wayland path gets this from
    // the compositor, which does not move a locked pointer at all.
    if (x.WarpPointer != nullptr) {
        x.WarpPointer(g_display, 0, g_window, 0, 0, 0, 0, static_cast<int>(g_pointer_x),
                      static_cast<int>(g_pointer_y));
    }
    if (x.Flush != nullptr) x.Flush(g_display);
}

void* display() { return g_display; }

unsigned long window() { return g_window; }


// X11 input, translated into the same HostInputEvent queue the Wayland
// listeners push onto -- so everything downstream (Process B's bridge,
// the engine's own entry points) is identical on both backends and
// nothing had to learn about X11.
//
// Two conversions matter and both are exact rather than approximate:
//
//  - An X keycode is the evdev scancode plus 8, fixed by the X protocol.
//    The Wayland path reports raw evdev codes, and so does a real
//    Android device's own KeyEvent.getScanCode(), so subtracting 8 puts
//    X11 on precisely the same footing with no second mapping table.
//  - X button numbers are 1-based with the wheel occupying 4-7. The
//    first three map to Android's own button indices the same way the
//    Wayland path does (PRIMARY-1=0, SECONDARY-1=1, TERTIARY-1=3), and
//    4/5 are a wheel notch each rather than buttons.
namespace {

void push(stud::android_glue::HostInputEvent ev) {
    ev.surface_width = static_cast<uint32_t>(g_width.load());
    ev.surface_height = static_cast<uint32_t>(g_height.load());
    stud::android_glue::push_host_input_event(ev);
}

void on_motion(int x_pos, int y_pos) {
    if (g_pointer_locked.load()) {
        // A warp of our own is not the user moving the mouse. It only
        // re-establishes where the next delta is measured from.
        if (g_ignore_next_motion) {
            g_ignore_next_motion = false;
            g_locked_last_x = x_pos;
            g_locked_last_y = y_pos;
            return;
        }
        const int dx = x_pos - g_locked_last_x;
        const int dy = y_pos - g_locked_last_y;
        g_locked_last_x = x_pos;
        g_locked_last_y = y_pos;
        if (dx != 0 || dy != 0) {
            stud::android_glue::HostInputEvent ev;
            ev.type = stud::android_glue::HostInputEvent::kPointerRelative;
            ev.x = static_cast<float>(dx);
            ev.y = static_cast<float>(dy);
            // The position reported alongside a delta stays at the
            // anchor, because that is what a locked pointer means
            // downstream: Wayland's compositor genuinely does not move
            // the pointer, and the whole unlock path depends on the
            // first absolute position afterwards still being the anchor.
            push(ev);
        }
        // Only warp when the pointer is about to run out of window --
        // the grab confines it, so it would stop dead at the edge and
        // the camera with it. Recentring here rather than on every
        // motion is what keeps the pointer where the user left it.
        const int margin = 64;
        const int w = g_width.load();
        const int h = g_height.load();
        if (x_pos < margin || y_pos < margin || x_pos > w - margin || y_pos > h - margin) {
            Xlib& x = xlib();
            if (x.WarpPointer != nullptr) {
                g_ignore_next_motion = true;
                x.WarpPointer(g_display, 0, g_window, 0, 0, 0, 0, w / 2, h / 2);
                if (x.Flush != nullptr) x.Flush(g_display);
            }
        }
        return;
    }
    g_pointer_x = static_cast<float>(x_pos);
    g_pointer_y = static_cast<float>(y_pos);
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerMotion;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    push(ev);
}

void on_button(unsigned int button, bool pressed, int x_pos, int y_pos) {
    // While locked, every event reports the anchor. The pointer really
    // has moved (X11 has no way to hold it still), but saying so would
    // hand the engine the warped position and move its cursor there --
    // which is exactly the jump to the middle of the screen this had.
    if (!g_pointer_locked.load()) {
        g_pointer_x = static_cast<float>(x_pos);
        g_pointer_y = static_cast<float>(y_pos);
    }
    if (button == 4 || button == 5) {
        // A wheel notch is a press followed by a release; only one of
        // them is a scroll, or every notch would count twice.
        if (!pressed) return;
        stud::android_glue::HostInputEvent ev;
        ev.type = stud::android_glue::HostInputEvent::kPointerAxis;
        ev.x = g_pointer_x;
        ev.y = g_pointer_y;
        // Button 4 is up. The Wayland path reports a positive value for
        // scrolling up after negating Wayland's own downward axis, so
        // this matches without a second convention.
        ev.a = button == 4 ? 1.0f : -1.0f;
        push(ev);
        return;
    }
    uint32_t android_button;
    switch (button) {
        case 1: android_button = 0; break;  // primary
        case 3: android_button = 1; break;  // secondary
        case 2: android_button = 3; break;  // tertiary
        default: return;                    // no real Android equivalent
    }
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerButton;
    ev.code = android_button;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    ev.a = pressed ? 1.0f : 0.0f;
    push(ev);
}

void on_key(unsigned int keycode, bool pressed) {
    if (keycode < 8) return;  // no evdev code below this exists
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kKey;
    ev.code = keycode - 8;
    ev.a = pressed ? 1.0f : 0.0f;
    // X11 delivers its own auto-repeat as ordinary press events with no
    // marker, and telling a real repeat from a fast typist needs the
    // XKB detectable-autorepeat extension. Reported as first presses
    // until that lands, which is the honest side to err on: a held key
    // still reaches the engine.
    ev.b = 0.0f;
    push(ev);
}

}  // namespace

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
            case MotionNotify:
                on_motion(event.xmotion.x, event.xmotion.y);
                break;
            case EnterNotify:
                on_motion(event.xcrossing.x, event.xcrossing.y);
                break;
            case LeaveNotify: {
                stud::android_glue::HostInputEvent ev;
                ev.type = stud::android_glue::HostInputEvent::kPointerLeave;
                push(ev);
                break;
            }
            case ButtonPress:
                on_button(event.xbutton.button, true, event.xbutton.x, event.xbutton.y);
                break;
            case ButtonRelease:
                on_button(event.xbutton.button, false, event.xbutton.x, event.xbutton.y);
                break;
            case KeyPress:
                on_key(event.xkey.keycode, true);
                break;
            case KeyRelease:
                on_key(event.xkey.keycode, false);
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
