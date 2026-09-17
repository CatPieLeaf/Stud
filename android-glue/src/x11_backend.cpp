#include "x11_backend.h"

#include "stud/android_glue.h"

#include <set>
#include <chrono>
#include <string>
#include <thread>

#include <X11/Xatom.h>
#include "stud_window_icon.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
// Types only; libXi itself is dlopen'd, like libX11 above.
#include <X11/extensions/XInput2.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <unistd.h>

namespace stud::android_glue::x11 {

namespace {

// Every Xlib entry point this backend uses, resolved from libX11 at
// runtime. Declared with the real header's own types, so the compiler
// checks each signature, what dlopen buys here is only that a machine
// without libX11 still runs Stud on Wayland, not a hand-written ABI.
// XInput2, for raw motion.
//
// Core X motion reports where the POINTER is, so it stops the moment the
// pointer stops: at the edge of the window a confined pointer has nowhere
// left to go and the events simply end, taking the camera with them. That
// is the reported "spin is limited by the display boundaries".
//
// Wayland does not have that problem because zwp_relative_pointer reports
// what the DEVICE did, independent of where the pointer is. XI2 raw events
// are the same thing on X: they come straight off the device, before the
// server has applied them to a pointer, so they keep arriving however hard
// the pointer is pinned against a wall.
//
// Loaded separately and optionally: a machine without libXi keeps the old
// behaviour rather than losing X11 support.
struct Xi2 {
    void* handle = nullptr;
    Status (*QueryVersion)(Display*, int*, int*) = nullptr;
    int (*SelectEvents)(Display*, Window, XIEventMask*, int) = nullptr;
    Bool (*QueryExtension)(Display*, const char*, int*, int*, int*) = nullptr;
    Bool (*GetEventData)(Display*, XGenericEventCookie*) = nullptr;
    void (*FreeEventData)(Display*, XGenericEventCookie*) = nullptr;
};

Xi2& xi2() {
    static Xi2 x;
    return x;
}

struct Xlib {
    void* handle = nullptr;
    // Xlib's own documented precondition for using one Display from more
    // than one thread, and Stud does exactly that on X11: the main loop
    // drains events while every client connection runs on its own thread
    // (serve_connection_thread), and the Vulkan driver's X11 WSI presents
    // on whichever of those threads the engine called from, through the
    // SAME Display this file opened -- vkCreateXlibSurfaceKHR is handed
    // it directly. Without this call Xlib's internal locks are no-ops and
    // its XCB connection has no agreed reader.
    //
    // Wayland has no equivalent exposure: the driver creates its own
    // wl_event_queue there, and Stud's own connection is only ever
    // touched under wayland_mutex().
    Status (*InitThreads)() = nullptr;
    // Error handling. Without these, Xlib's own defaults apply: a
    // protocol error prints to stderr and calls exit(), and an I/O error
    // (the server gone, the session ending) exits too -- both from
    // whatever thread happened to make the call, with static destructors
    // running underneath live engine threads. This project already
    // settled that question for its own shutdown, which is why both
    // processes _exit() rather than return from main.
    int (*SetErrorHandler)(int (*)(Display*, XErrorEvent*)) = nullptr;
    int (*SetIOErrorHandler)(int (*)(Display*)) = nullptr;
    int (*GetErrorText)(Display*, int, char*, int) = nullptr;
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
    int (*SetSelectionOwner)(Display*, Atom, Window, Time) = nullptr;
    Window (*GetSelectionOwner)(Display*, Atom) = nullptr;
    int (*ConvertSelection)(Display*, Atom, Atom, Atom, Window, Time) = nullptr;
    int (*GetWindowProperty)(Display*, Window, Atom, long, long, Bool, Atom, Atom*, int*,
                             unsigned long*, unsigned long*, unsigned char**) = nullptr;
    int (*DeleteProperty)(Display*, Window, Atom) = nullptr;
    Status (*SendEvent)(Display*, Window, Bool, long, XEvent*) = nullptr;
    int (*Free)(void*) = nullptr;
    Status (*MatchVisualInfo)(Display*, int, int, int, XVisualInfo*) = nullptr;
    Colormap (*CreateColormap)(Display*, Window, Visual*, int) = nullptr;
    Window (*CreateWindow)(Display*, Window, int, int, unsigned int, unsigned int, unsigned int,
                           int, unsigned int, Visual*, unsigned long,
                           XSetWindowAttributes*) = nullptr;
    GC (*CreateGC)(Display*, Drawable, unsigned long, XGCValues*) = nullptr;
    int (*FreeGC)(Display*, GC) = nullptr;
    XImage* (*CreateImage)(Display*, Visual*, unsigned int, int, int, char*, unsigned int,
                           unsigned int, int, int) = nullptr;
    int (*PutImage)(Display*, Drawable, GC, XImage*, int, int, int, int, unsigned int,
                    unsigned int) = nullptr;
    int (*MoveResizeWindow)(Display*, Window, int, int, unsigned int, unsigned int) = nullptr;
    int (*UnmapWindow)(Display*, Window) = nullptr;
    Bool (*TranslateCoordinates)(Display*, Window, Window, int, int, int*, int*,
                                 Window*) = nullptr;
    int (*RaiseWindow)(Display*, Window) = nullptr;
    char* (*ResourceManagerString)(Display*) = nullptr;
    int (*SetWindowBackgroundPixmap)(Display*, Window, Pixmap) = nullptr;
    int (*ChangeWindowAttributes)(Display*, Window, unsigned long,
                                  XSetWindowAttributes*) = nullptr;
    Bool (*QueryExtension)(Display*, const char*, int*, int*, int*) = nullptr;
    Bool (*GetEventData)(Display*, XGenericEventCookie*) = nullptr;
    void (*FreeEventData)(Display*, XGenericEventCookie*) = nullptr;
};

Xlib& xlib() {
    static Xlib x;
    return x;
}

Display* g_display = nullptr;
Window g_window = 0;
std::atomic<bool> g_pointer_locked{false};
// Confined, which is what a camera drag does: the pointer keeps its own
// position and its ordinary motion, it simply cannot leave the window.
// Kept because relative motion has to be reported while it lasts; see
// on_motion().
std::atomic<bool> g_pointer_confined{false};
// XInput2: the extension's opcode, and whether raw motion is actually
// being delivered. False means core motion is all there is.
int g_xi_opcode = -1;
bool g_raw_motion = false;
// Set while a warp of our own is in flight, so the MotionNotify it
// generates is not read as the user moving the mouse, without this the
// camera receives the warp back to centre as a second, opposite delta
// and mouse look cancels itself out.
bool g_ignore_next_motion = false;
// The empty cursor, shared by the game window and the text overlay.
Cursor g_blank_cursor = 0;
// Whether the window has been shown yet. It is held back until the first
// frame, so it never appears empty.
bool g_mapped = false;
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

// Opens libXi and asks the server for raw motion on every master device.
// Best-effort: returns false and leaves everything as it was if XI2 is
// not there, which costs only the unbounded spin.
bool enable_raw_motion(Display* display) {
    Xi2& xi = xi2();
    if (xi.handle == nullptr) {
        xi.handle = ::dlopen("libXi.so.6", RTLD_NOW | RTLD_LOCAL);
        if (xi.handle == nullptr) {
            std::fprintf(stderr,
                         "stud: android-glue: no libXi.so.6, so a camera spin stops at the edge "
                         "of the window (%s)\n",
                         ::dlerror());
            return false;
        }
        xi.QueryVersion = reinterpret_cast<decltype(xi.QueryVersion)>(
            ::dlsym(xi.handle, "XIQueryVersion"));
        xi.SelectEvents = reinterpret_cast<decltype(xi.SelectEvents)>(
            ::dlsym(xi.handle, "XISelectEvents"));
    }
    Xlib& x = xlib();
    if (xi.QueryVersion == nullptr || xi.SelectEvents == nullptr || x.QueryExtension == nullptr ||
        x.GetEventData == nullptr) {
        return false;
    }
    int event_base = 0;
    int error_base = 0;
    if (x.QueryExtension(display, "XInputExtension", &g_xi_opcode, &event_base, &error_base) !=
        True) {
        return false;
    }
    int major = 2;
    int minor = 2;
    if (xi.QueryVersion(display, &major, &minor) != Success) return false;

    // Raw events are only ever delivered to the root window: they are not
    // about any particular window, which is exactly why they survive the
    // pointer being pinned against an edge.
    unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XISetMask(mask_bits, XI_RawMotion);
    XIEventMask mask{};
    mask.deviceid = XIAllMasterDevices;
    mask.mask_len = sizeof(mask_bits);
    mask.mask = mask_bits;
    const Window root = DefaultRootWindow(display);
    if (xi.SelectEvents(display, root, &mask, 1) != Success) return false;
    if (x.Flush != nullptr) x.Flush(display);
    g_raw_motion = true;
    std::printf("stud: android-glue: XInput2 raw motion (a spin is not stopped by the window "
                "edge)\n");
    std::fflush(stdout);
    return true;
}

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
    LOAD(InitThreads, "XInitThreads");
    LOAD(SetErrorHandler, "XSetErrorHandler");
    LOAD(SetIOErrorHandler, "XSetIOErrorHandler");
    LOAD(GetErrorText, "XGetErrorText");
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
    LOAD(SetSelectionOwner, "XSetSelectionOwner");
    LOAD(GetSelectionOwner, "XGetSelectionOwner");
    LOAD(ConvertSelection, "XConvertSelection");
    LOAD(GetWindowProperty, "XGetWindowProperty");
    LOAD(DeleteProperty, "XDeleteProperty");
    LOAD(SendEvent, "XSendEvent");
    LOAD(Free, "XFree");
    LOAD(MatchVisualInfo, "XMatchVisualInfo");
    LOAD(CreateColormap, "XCreateColormap");
    LOAD(CreateWindow, "XCreateWindow");
    LOAD(CreateGC, "XCreateGC");
    LOAD(FreeGC, "XFreeGC");
    LOAD(CreateImage, "XCreateImage");
    LOAD(PutImage, "XPutImage");
    LOAD(MoveResizeWindow, "XMoveResizeWindow");
    LOAD(UnmapWindow, "XUnmapWindow");
    LOAD(TranslateCoordinates, "XTranslateCoordinates");
    LOAD(RaiseWindow, "XRaiseWindow");
    LOAD(ResourceManagerString, "XResourceManagerString");
    LOAD(SetWindowBackgroundPixmap, "XSetWindowBackgroundPixmap");
    LOAD(ChangeWindowAttributes, "XChangeWindowAttributes");
    LOAD(QueryExtension, "XQueryExtension");
    LOAD(GetEventData, "XGetEventData");
    LOAD(FreeEventData, "XFreeEventData");
#undef LOAD
    if (!ok) {
        ::dlclose(x.handle);
        x.handle = nullptr;
    }
    return ok;
}

}  // namespace

// A protocol error is not fatal, and Xlib's default says otherwise.
//
// The default handler prints and calls exit(). Every request Stud makes
// is asynchronous, so the error arrives later, on whichever thread next
// talks to the server -- so the default turns a bad window id, or a
// property set on a window the manager has just withdrawn, into the whole
// app disappearing. None of those are worth dying for: the window is
// still there and the next frame still presents.
//
// Reported, though, and not silently swallowed: an X error means a real
// request Stud made was refused, and the first few say which.
int on_x_error(Display* dpy, XErrorEvent* event) {
    static int said = 0;
    static int total = 0;
    ++total;
    if (said < 8) {
        ++said;
        char text[128] = {0};
        Xlib& x = xlib();
        if (x.GetErrorText != nullptr) {
            x.GetErrorText(dpy, event->error_code, text, static_cast<int>(sizeof(text)));
        }
        std::fprintf(stderr,
                     "stud: android-glue: X error %u (%s) on request %u.%u, serial %lu\n",
                     event->error_code, text[0] != '\0' ? text : "unknown",
                     event->request_code, event->minor_code,
                     static_cast<unsigned long>(event->serial));
        std::fflush(stderr);
        if (said == 8) {
            std::fprintf(stderr, "stud: android-glue: further X errors will not be reported\n");
            std::fflush(stderr);
        }
    }
    return 0;
}

// An I/O error is the connection itself being gone -- the server exited,
// the session ended, the socket was cut. Nothing can be recovered and no
// further Xlib call can succeed, so the only question is how to leave.
//
// Xlib calls exit() if this returns, which would run static destructors
// underneath whatever threads are still live -- the exact teardown crash
// this project already fixed by making both processes _exit(). So it
// leaves the same way, deliberately, rather than by falling through to
// Xlib's answer.
int on_x_io_error(Display*) {
    std::fprintf(stderr, "stud: android-glue: the X server connection is gone; exiting\n");
    std::fflush(stderr);
    std::fflush(stdout);
    ::_exit(0);
    return 0;
}

void install_x_error_handlers() {
    Xlib& x = xlib();
    if (x.SetErrorHandler != nullptr) x.SetErrorHandler(on_x_error);
    if (x.SetIOErrorHandler != nullptr) x.SetIOErrorHandler(on_x_io_error);
}

bool available() {
    static const bool answer = [] {
        const char* display_name = std::getenv("DISPLAY");
        if (display_name == nullptr || *display_name == '\0') return false;
        if (!load_xlib()) return false;
        // Opened once and kept: this is the connection the window, EGL and
        // Vulkan all use. Closing and reopening would hand out a stale
        // Display* to whichever of them asked first.
        install_x_error_handlers();
        // Before any other Xlib call on this connection, per Xlib's own
        // contract -- see the declaration of InitThreads above.
        if (xlib().InitThreads() == 0) {
            std::fprintf(stderr, "stud: android-glue: XInitThreads() failed; X11 is not safe "
                                 "to use from more than one thread\n");
            return false;
        }
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

    // Device-level motion, so a camera spin is not stopped by the window
    // edge. Optional: without it the core motion above is all there is.
    enable_raw_motion(g_display);

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

    // What the server does with this window's pixels when it is resized.
    //
    // Both halves matter, and having only one of them is what made a
    // drag-resize show a transparent strip of desktop through the window.
    //
    // The default bit gravity is ForgetGravity: on every configure the
    // server throws the window's whole contents away and clears it to its
    // background. With the background also set to None, "clears it" is a
    // no-op -- so the window holds nothing at all wherever the engine has
    // not painted since, and a compositor draws nothing where a window
    // holds nothing. That is the strip: not an alpha channel (this window
    // is depth 24, opaque) but an area with no content in it.
    //
    // The two together fix it, and neither alone would:
    //
    //   NorthWestGravity  keeps the pixels already there, anchored at the
    //                     top-left, instead of discarding them. This is
    //                     what avoids the whole window going black for as
    //                     long as the mouse button is held -- the original
    //                     reason the background was removed, which was the
    //                     right symptom and the wrong half of the fix.
    //
    //   a real background only the NEWLY exposed area is then cleared to
    //                     it, so the strip is black for the frame or two
    //                     before the engine catches up, rather than a
    //                     hole.
    //
    // The engine still owns the window's contents; this only says what the
    // server does in the instant between the resize and the next present.
    if (x.ChangeWindowAttributes != nullptr) {
        XSetWindowAttributes attrs{};
        attrs.bit_gravity = NorthWestGravity;
        attrs.background_pixel = BlackPixel(g_display, screen);
        x.ChangeWindowAttributes(g_display, g_window, CWBitGravity | CWBackPixel, &attrs);
    }

    g_width.store(width);
    g_height.store(height);

    // The engine draws its own cursor, in-frame, on the app shell and
    // in-game alike, the same reason the Wayland path passes a null
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
            // Kept, not freed: the text overlay is its own top-level
            // window and needs the same blank cursor, or the desktop's
            // arrow reappears the moment the pointer crosses onto it.
            g_blank_cursor = x.CreatePixmapCursor(g_display, bitmap, bitmap, &black, &black, 0, 0);
            if (g_blank_cursor != 0) x.DefineCursor(g_display, g_window, g_blank_cursor);
            if (x.FreePixmap != nullptr) x.FreePixmap(g_display, bitmap);
        }
    }

    // The taskbar/titlebar icon. WM_CLASS above is enough for a desktop
    // that can find Stud's .desktop file, but nothing guarantees one is
    // installed, an AppImage run straight from a download has none,
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

    // Deliberately NOT mapped here; see ensure_mapped(). The window is
    // shown when there is something in it.
    x.Flush(g_display);

    std::printf("stud: android-glue: X11 window %lux%lu, WM_CLASS=\"%s\", title=\"Stud\"\n",
                static_cast<unsigned long>(width), static_cast<unsigned long>(height),
                STUD_APP_ID);
    std::fflush(stdout);
    return true;
}

void warp_pointer(int x, int y) {
    if (g_display == nullptr || g_window == 0) return;
    Xlib& x11 = xlib();
    if (x11.WarpPointer == nullptr) return;
    // Given in the engine's pixels; the server wants its own.
    const int device_x = static_cast<int>(
        stud::android_glue::native_window_device_px_from_pointer(static_cast<float>(x)) + 0.5f);
    const int device_y = static_cast<int>(
        stud::android_glue::native_window_device_px_from_pointer(static_cast<float>(y)) + 0.5f);
    x11.WarpPointer(g_display, 0, g_window, 0, 0, 0, 0, device_x, device_y);
    if (x11.Flush != nullptr) x11.Flush(g_display);
    g_pointer_x = static_cast<float>(device_x);
    g_pointer_y = static_cast<float>(device_y);
}

void set_pointer_confined(bool confined) {
    if (g_display == nullptr || g_window == 0) return;
    Xlib& x11 = xlib();
    if (confined) {
        if (x11.GrabPointer == nullptr) return;
        // confine_to = this window, and nothing else: the pointer keeps
        // its own position and its ordinary motion, it simply cannot
        // cross the frame. No warping, unlike the mouse-look grab.
        x11.GrabPointer(g_display, g_window, True,
                        ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                        GrabModeAsync, g_window, 0, 0);
        g_pointer_confined.store(true);
    } else if (x11.UngrabPointer != nullptr) {
        x11.UngrabPointer(g_display, 0);
        g_pointer_confined.store(false);
    }
    if (x11.Flush != nullptr) x11.Flush(g_display);
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
        // Measure the first delta from where the drag actually began.
        // No warp here. Moving the pointer at the moment of the lock is
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
    // engine's own cursor has stayed, the Wayland path gets this from
    // the compositor, which does not move a locked pointer at all.
    if (x.WarpPointer != nullptr) {
        x.WarpPointer(g_display, 0, g_window, 0, 0, 0, 0, static_cast<int>(g_pointer_x),
                      static_cast<int>(g_pointer_y));
    }
    if (x.Flush != nullptr) x.Flush(g_display);
}

void* display() { return g_display; }

// A second connection to the same X server, handed to the Vulkan driver
// and to nothing else.
//
// The driver's X11 WSI is not a passive user of the connection it is
// given: it issues requests and waits for Present events on it, from
// whichever thread the engine called on, while Stud's own main loop is
// draining events on the same connection. Sharing one Display makes those
// two contend for one request stream and one reader -- live-caught as
// vkCreateSwapchainKHR entering the driver during a resize and never
// coming back, with the main loop stuck behind it.
//
// On Wayland this problem does not exist because the driver creates its
// own wl_event_queue, so its traffic and Stud's never meet. A second
// Display is the X equivalent: one connection per role, so issuing a
// request never has to wait on the thread that is reading events.
//
// The window is a server-side resource named by an XID, so a surface
// created on this connection addresses the same window the other one
// made.
void* vk_display() {
    static Display* vk = [] () -> Display* {
        if (g_display == nullptr) return nullptr;
        Display* d = xlib().OpenDisplay(nullptr);
        if (d == nullptr) {
            std::fprintf(stderr, "stud: android-glue: could not open a second X connection for "
                                 "Vulkan; sharing the event connection instead\n");
            return nullptr;
        }
        std::printf("stud: android-glue: Vulkan has its own X connection\n");
        std::fflush(stdout);
        return d;
    }();
    return vk != nullptr ? static_cast<void*>(vk) : static_cast<void*>(g_display);
}

unsigned long window() { return g_window; }


// X11 input, translated into the same HostInputEvent queue the Wayland
// listeners push onto, so everything downstream (Process B's bridge,
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

// The clipboard, X11-style: a selection belongs to a WINDOW, and its
// owner is asked for the bytes every time somebody pastes. So "copy"
// means claiming ownership and keeping the text, and answering
// SelectionRequest for as long as Stud holds it; "paste" means asking
// the current owner and waiting for the reply to land on a property of
// our own window.
std::string g_clipboard_owned;      // what Stud last copied
std::string g_clipboard_received;   // the most recent paste, once it arrives
bool g_clipboard_reply_pending = false;
Atom g_atom_clipboard = 0;
Atom g_atom_utf8 = 0;
Atom g_atom_targets = 0;
Atom g_atom_stud_selection = 0;

void intern_clipboard_atoms() {
    Xlib& x = xlib();
    if (g_atom_clipboard != 0 || x.InternAtom == nullptr || g_display == nullptr) return;
    g_atom_clipboard = x.InternAtom(g_display, "CLIPBOARD", False);
    g_atom_utf8 = x.InternAtom(g_display, "UTF8_STRING", False);
    g_atom_targets = x.InternAtom(g_display, "TARGETS", False);
    // Our own property, which a paste reply is written to. Named rather
    // than reusing a standard one so nothing else can collide with it.
    g_atom_stud_selection = x.InternAtom(g_display, "STUD_SELECTION", False);
}

// Somebody is pasting from Stud. Hand over the text, or the list of
// formats it is available in.
void answer_selection_request(const XSelectionRequestEvent& request) {
    Xlib& x = xlib();
    XEvent reply{};
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = request.display;
    reply.xselection.requestor = request.requestor;
    reply.xselection.selection = request.selection;
    reply.xselection.target = request.target;
    reply.xselection.time = request.time;
    reply.xselection.property = None;  // refused, unless the target is one we have

    // A requestor that sets no property is using the obsolete protocol;
    // ICCCM says to answer on the target atom instead.
    const Atom property = request.property != None ? request.property : request.target;

    if (request.target == g_atom_targets) {
        const Atom offered[] = {g_atom_targets, g_atom_utf8, XA_STRING};
        x.ChangeProperty(g_display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                         reinterpret_cast<const unsigned char*>(offered),
                         static_cast<int>(sizeof(offered) / sizeof(offered[0])));
        reply.xselection.property = property;
    } else if (request.target == g_atom_utf8 || request.target == XA_STRING) {
        x.ChangeProperty(g_display, request.requestor, property, request.target, 8,
                         PropModeReplace,
                         reinterpret_cast<const unsigned char*>(g_clipboard_owned.data()),
                         static_cast<int>(g_clipboard_owned.size()));
        reply.xselection.property = property;
    }
    if (x.SendEvent != nullptr) {
        x.SendEvent(g_display, request.requestor, False, 0, &reply);
    }
    if (x.Flush != nullptr) x.Flush(g_display);
}

// The answer to a paste Stud asked for has landed on our own window.
void read_selection_reply(const XSelectionEvent& notify) {
    g_clipboard_reply_pending = false;
    g_clipboard_received.clear();
    Xlib& x = xlib();
    if (notify.property == None || x.GetWindowProperty == nullptr) return;
    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    // Read it whole: a clipboard large enough to need INCR is a
    // clipboard no text box here is going to receive.
    if (x.GetWindowProperty(g_display, g_window, notify.property, 0, 1 << 20, True, AnyPropertyType,
                            &actual_type, &actual_format, &count, &remaining, &data) == Success &&
        data != nullptr) {
        if (actual_format == 8) {
            g_clipboard_received.assign(reinterpret_cast<const char*>(data), count);
        }
        if (x.Free != nullptr) x.Free(data);
    }
}

// The engine's pixels, not the server's.
//
// X reports device pixels and sizes; the engine renders into a buffer that
// is smaller whenever HiDPI is off or the upscaler is running. Converting
// at this one door keeps every event consistent -- positions, deltas and
// the surface size that rides along with them.
float to_engine_px(float device_px) {
    return stud::android_glue::native_window_pointer_px_from_device(device_px);
}

void push(stud::android_glue::HostInputEvent ev) {
    ev.surface_width =
        static_cast<uint32_t>(to_engine_px(static_cast<float>(g_width.load())) + 0.5f);
    ev.surface_height =
        static_cast<uint32_t>(to_engine_px(static_cast<float>(g_height.load())) + 0.5f);
    if (ev.type == stud::android_glue::HostInputEvent::kPointerMotion ||
        ev.type == stud::android_glue::HostInputEvent::kPointerButton ||
        ev.type == stud::android_glue::HostInputEvent::kPointerAxis ||
        ev.type == stud::android_glue::HostInputEvent::kPointerRelative ||
        ev.type == stud::android_glue::HostInputEvent::kPointerEnter) {
        ev.x = to_engine_px(ev.x);
        ev.y = to_engine_px(ev.y);
    }
    stud::android_glue::push_host_input_event(ev);
}

// Raw motion, straight off the device: one event per physical movement,
// with the deltas the device reported, whatever the pointer is doing.
void on_raw_motion(double dx, double dy) {
    if (!g_pointer_locked.load() && !g_pointer_confined.load()) return;
    if (dx == 0.0 && dy == 0.0) return;
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerRelative;
    ev.x = static_cast<float>(dx);
    ev.y = static_cast<float>(dy);
    push(ev);
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
        // Raw motion already reported this movement, from the device.
        if (!g_raw_motion && (dx != 0 || dy != 0)) {
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
        // Only warp when the pointer is about to run out of window,
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
    // Relative motion while CONFINED, not only while locked, because that
    // is what the Wayland side does (relative_pointer_motion() reports
    // for locked OR confined) and everything downstream is written to it:
    // during a confined drag the client deliberately withholds the delta
    // it could compute from these absolute positions, on the grounds that
    // the relative event for the same motion carries it. On X11 that
    // event did not exist, so the movement was withheld and nothing
    // replaced it -- a camera drag moved the view by exactly nothing,
    // while the engine pinned its own cursor for the gesture, so the
    // cursor stopped too. User-reported as "you click, it moves a tiny
    // bit, you click again, repeat".
    //
    // A camera drag confines rather than locks (drag-lock is opt-in,
    // STUD_DRAG_LOCK), which is why the locked branch above never came
    // into it.
    const float dx = static_cast<float>(x_pos) - g_pointer_x;
    const float dy = static_cast<float>(y_pos) - g_pointer_y;
    g_pointer_x = static_cast<float>(x_pos);
    g_pointer_y = static_cast<float>(y_pos);
    if (!g_raw_motion && g_pointer_confined.load() && (dx != 0.0f || dy != 0.0f)) {
        stud::android_glue::HostInputEvent rel;
        rel.type = stud::android_glue::HostInputEvent::kPointerRelative;
        rel.x = dx;
        rel.y = dy;
        push(rel);
    }
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kPointerMotion;
    ev.x = g_pointer_x;
    ev.y = g_pointer_y;
    push(ev);
}

// What is currently held, so it can be let go of when this window stops
// being the one receiving input.
//
// X11 stops delivering key and button events the moment focus moves, so a
// key released after clicking another window is one this client is never
// told about, and the engine goes on holding it, which in an experience
// means walking forever. The Wayland backend has done this since it was
// written; this side never did.
//
// Only ever touched from the thread that pumps X11, which is one thread.
std::set<unsigned int>& keys_down() {
    static std::set<unsigned int> k;
    return k;
}
std::set<uint32_t>& buttons_down() {
    static std::set<uint32_t> b;
    return b;
}

void release_all_held_input() {
    for (unsigned int code : keys_down()) {
        stud::android_glue::HostInputEvent ev;
        ev.type = stud::android_glue::HostInputEvent::kKey;
        ev.code = code;
        ev.a = 0.0f;
        push(ev);
    }
    keys_down().clear();
    for (uint32_t button : buttons_down()) {
        stud::android_glue::HostInputEvent ev;
        ev.type = stud::android_glue::HostInputEvent::kPointerButton;
        ev.code = button;
        ev.x = g_pointer_x;
        ev.y = g_pointer_y;
        ev.a = 0.0f;
        push(ev);
    }
    buttons_down().clear();
}

void push_window_focus(bool focused) {
    stud::android_glue::HostInputEvent ev;
    ev.type = stud::android_glue::HostInputEvent::kWindowFocus;
    ev.a = focused ? 1.0f : 0.0f;
    push(ev);
}

void on_button(unsigned int button, bool pressed, int x_pos, int y_pos) {
    // While locked, every event reports the anchor. The pointer really
    // has moved (X11 has no way to hold it still), but saying so would
    // hand the engine the warped position and move its cursor there,
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
    if (pressed) {
        buttons_down().insert(android_button);
    } else {
        buttons_down().erase(android_button);
    }
    push(ev);
}

void on_key(unsigned int keycode, bool pressed) {
    if (keycode < 8) return;  // no evdev code below this exists
    if (pressed) {
        keys_down().insert(keycode - 8);
    } else {
        keys_down().erase(keycode - 8);
    }
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
        if (g_raw_motion && event.type == GenericEvent &&
            event.xcookie.extension == g_xi_opcode && x.GetEventData != nullptr &&
            x.GetEventData(g_display, &event.xcookie) == True) {
            if (event.xcookie.evtype == XI_RawMotion && event.xcookie.data != nullptr) {
                const auto* raw = static_cast<const XIRawEvent*>(event.xcookie.data);
                // valuator 0 is X, 1 is Y, and only the axes that moved
                // are present, so the values are walked in order.
                double dx = 0.0;
                double dy = 0.0;
                const double* value = raw->raw_values;
                for (int axis = 0; axis < raw->valuators.mask_len * 8; ++axis) {
                    if (!XIMaskIsSet(raw->valuators.mask, axis)) continue;
                    if (axis == 0) dx = *value;
                    if (axis == 1) dy = *value;
                    ++value;
                }
                on_raw_motion(dx, dy);
            }
            if (x.FreeEventData != nullptr) x.FreeEventData(g_display, &event.xcookie);
            continue;
        }
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
            case SelectionRequest:
                answer_selection_request(event.xselectionrequest);
                break;
            case SelectionNotify:
                read_selection_reply(event.xselection);
                break;
            case SelectionClear:
                // Another application took the clipboard; Stud is no
                // longer the owner and must stop answering for it.
                g_clipboard_owned.clear();
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
                push_window_focus(true);
                break;
            case FocusOut:
                if (g_focused.exchange(false)) {
                    std::printf("stud: android-glue: window in the background\n");
                    std::fflush(stdout);
                }
                // Let go of everything held. Whatever is still down will
                // be released somewhere this window cannot hear.
                release_all_held_input();
                push_window_focus(false);
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

namespace {
// The text overlay's own window.
//
// The drawing is shared with the Wayland path; what differs is where the
// pixels go. Two things about X11 decide the shape of this:
//
//  - Alpha needs a 32-bit TrueColor visual with its own colormap. That
//    part is standard.
//  - A compositing manager only blends TOP-LEVEL windows. A child window
//    with an ARGB visual is simply drawn into its parent with the alpha
//    ignored, which is a black box behind the text, exactly what this
//    first did. So the overlay is an override-redirect top-level,
//    positioned in root coordinates over the game window, which is what
//    every tooltip and IME candidate window on X11 already is.
//
// Without a compositor running the server ignores the alpha and the box
// is opaque. That is a degradation rather than a failure, and is why
// this does not refuse to run without one.
Window g_overlay_window = 0;
GC g_overlay_gc = nullptr;
XImage* g_overlay_image = nullptr;
int g_overlay_w = 0;
int g_overlay_h = 0;
Visual* g_overlay_visual = nullptr;
int g_overlay_depth = 0;

bool ensure_overlay_window() {
    if (g_overlay_window != 0) return true;
    Xlib& x = xlib();
    if (x.CreateWindow == nullptr || x.CreateGC == nullptr || x.MatchVisualInfo == nullptr ||
        x.CreateColormap == nullptr) {
        return false;
    }
    const int screen = DefaultScreen(g_display);
    XVisualInfo vi{};
    if (x.MatchVisualInfo(g_display, screen, 32, TrueColor, &vi) == 0) {
        // No ARGB visual: fall back to the parent's, which means an
        // opaque text box rather than none at all.
        vi.visual = DefaultVisual(g_display, screen);
        vi.depth = DefaultDepth(g_display, screen);
    }
    g_overlay_visual = vi.visual;
    g_overlay_depth = vi.depth;

    const Window root = RootWindow(g_display, screen);
    XSetWindowAttributes attributes{};
    attributes.colormap = x.CreateColormap(g_display, root, vi.visual, AllocNone);
    attributes.border_pixel = 0;
    attributes.background_pixel = 0;
    // Override-redirect: no window manager decoration, no focus stealing,
    // no placement of its own. It goes exactly where it is put, which
    // is over the text box.
    attributes.override_redirect = True;
    // No input: a click inside the text box belongs to the engine
    // underneath, which is what decides where the caret goes.
    attributes.event_mask = 0;
    g_overlay_window = x.CreateWindow(g_display, root, 0, 0, 1, 1, 0, vi.depth, InputOutput,
                                      vi.visual,
                                      CWColormap | CWBorderPixel | CWBackPixel | CWEventMask |
                                          CWOverrideRedirect,
                                      &attributes);
    if (g_overlay_window == 0) return false;
    if (g_blank_cursor != 0 && x.DefineCursor != nullptr) {
        x.DefineCursor(g_display, g_overlay_window, g_blank_cursor);
    }
    // Input-transparent: an override-redirect window would otherwise
    // swallow every click landing on the text box, and clicking a text
    // box is how the caret is placed. An empty INPUT shape makes the
    // server treat the window as not being there for pointer purposes
    // while still drawing it. XShape lives in libXext, loaded the same
    // optional way as everything else here, without it the overlay
    // still draws, it just eats clicks, so this is not fatal.
    if (void* xext = ::dlopen("libXext.so.6", RTLD_NOW | RTLD_LOCAL); xext != nullptr) {
        using CombineRectanglesFn = void (*)(Display*, Window, int, int, int, XRectangle*, int,
                                             int, int);
        auto combine =
            reinterpret_cast<CombineRectanglesFn>(::dlsym(xext, "XShapeCombineRectangles"));
        if (combine != nullptr) {
            // ShapeInput is 2 and ShapeSet is 0; named here rather than
            // pulling in <X11/extensions/shape.h> for two constants.
            constexpr int kShapeInput = 2;
            constexpr int kShapeSet = 0;
            combine(g_display, g_overlay_window, kShapeInput, 0, 0, nullptr, 0, kShapeSet,
                    Unsorted);
        }
    }
    XGCValues values{};
    g_overlay_gc = x.CreateGC(g_display, g_overlay_window, 0, &values);
    return g_overlay_gc != nullptr;
}

}  // namespace

void present_text_overlay(const void* argb, int width, int height, int x_pos, int y_pos) {
    if (g_display == nullptr || g_window == 0 || argb == nullptr || width <= 0 || height <= 0) {
        return;
    }
    Xlib& x = xlib();
    if (!ensure_overlay_window()) return;
    if (x.CreateImage == nullptr || x.PutImage == nullptr) return;

    if (g_overlay_image == nullptr || g_overlay_w != width || g_overlay_h != height) {
        if (g_overlay_image != nullptr) {
            // Created with XCreateImage over a buffer this code does not
            // own, so only the header is freed, XDestroyImage would
            // free the caller's pixels too.
            if (x.Free != nullptr) x.Free(g_overlay_image);
            g_overlay_image = nullptr;
        }
        g_overlay_image =
            x.CreateImage(g_display, g_overlay_visual, static_cast<unsigned int>(g_overlay_depth),
                          ZPixmap, 0, const_cast<char*>(static_cast<const char*>(argb)),
                          static_cast<unsigned int>(width), static_cast<unsigned int>(height), 32,
                          0);
        if (g_overlay_image == nullptr) return;
        g_overlay_w = width;
        g_overlay_h = height;
    }
    g_overlay_image->data = const_cast<char*>(static_cast<const char*>(argb));

    // A top-level window is placed in ROOT coordinates, so where the box
    // is inside the game window has to be translated first, and again
    // on every update, because the game window can be moved or resized
    // under it.
    int root_x = x_pos;
    int root_y = y_pos;
    if (x.TranslateCoordinates != nullptr) {
        Window child = 0;
        x.TranslateCoordinates(g_display, g_window, RootWindow(g_display, DefaultScreen(g_display)),
                               x_pos, y_pos, &root_x, &root_y, &child);
    }
    if (x.MoveResizeWindow != nullptr) {
        x.MoveResizeWindow(g_display, g_overlay_window, root_x, root_y,
                           static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    }
    x.MapWindow(g_display, g_overlay_window);
    if (x.RaiseWindow != nullptr) x.RaiseWindow(g_display, g_overlay_window);
    x.PutImage(g_display, g_overlay_window, g_overlay_gc, g_overlay_image, 0, 0, 0, 0,
               static_cast<unsigned int>(width), static_cast<unsigned int>(height));
    if (x.Flush != nullptr) x.Flush(g_display);
}

void hide_text_overlay() {
    if (g_display == nullptr || g_overlay_window == 0) return;
    Xlib& x = xlib();
    if (x.UnmapWindow != nullptr) x.UnmapWindow(g_display, g_overlay_window);
    if (x.Flush != nullptr) x.Flush(g_display);
}


int32_t display_scale_120() {
    if (g_display == nullptr) return 120;
    Xlib& x = xlib();
    if (x.ResourceManagerString == nullptr) return 120;
    const char* resources = x.ResourceManagerString(g_display);
    if (resources == nullptr) return 120;
    // The resource database is plain text, one "Name:\tvalue" per line.
    // Only one entry matters here and parsing the whole database with
    // Xrm would pull in more of Xlib for no gain.
    const std::string text(resources);
    const std::string key = "Xft.dpi:";
    auto at = text.find(key);
    if (at == std::string::npos) return 120;
    at += key.size();
    double dpi = 0.0;
    try {
        dpi = std::stod(text.substr(at));
    } catch (const std::exception&) {
        return 120;
    }
    if (dpi <= 0.0) return 120;
    // 96 dpi is the unscaled baseline every toolkit uses.
    const int32_t scale = static_cast<int32_t>(dpi * 120.0 / 96.0 + 0.5);
    return scale > 0 ? scale : 120;
}

bool output_geometry(int32_t& px_w, int32_t& px_h, int32_t& mm_w, int32_t& mm_h) {
    if (g_display == nullptr) return false;
    const int screen = DefaultScreen(g_display);
    px_w = DisplayWidth(g_display, screen);
    px_h = DisplayHeight(g_display, screen);
    mm_w = DisplayWidthMM(g_display, screen);
    mm_h = DisplayHeightMM(g_display, screen);
    return px_w > 0 && px_h > 0;
}

void ensure_mapped() {
    if (g_display == nullptr || g_window == 0 || g_mapped) return;
    Xlib& x = xlib();
    if (x.MapWindow == nullptr) return;
    g_mapped = true;
    x.MapWindow(g_display, g_window);
    if (x.Flush != nullptr) x.Flush(g_display);
}

void clipboard_set(const std::string& text) {
    if (g_display == nullptr || g_window == 0) return;
    Xlib& x = xlib();
    if (x.SetSelectionOwner == nullptr) return;
    intern_clipboard_atoms();
    g_clipboard_owned = text;
    x.SetSelectionOwner(g_display, g_atom_clipboard, g_window, CurrentTime);
    if (x.Flush != nullptr) x.Flush(g_display);
}

std::string clipboard_get() {
    if (g_display == nullptr || g_window == 0) return {};
    Xlib& x = xlib();
    if (x.ConvertSelection == nullptr || x.GetSelectionOwner == nullptr) return {};
    intern_clipboard_atoms();
    // Stud's own copy needs no round trip, and asking oneself through
    // the server would deadlock this thread against its own pump.
    if (x.GetSelectionOwner(g_display, g_atom_clipboard) == g_window) return g_clipboard_owned;

    g_clipboard_received.clear();
    g_clipboard_reply_pending = true;
    x.ConvertSelection(g_display, g_atom_clipboard, g_atom_utf8, g_atom_stud_selection, g_window,
                       CurrentTime);
    if (x.Flush != nullptr) x.Flush(g_display);
    // The reply arrives as an event, so this has to pump for it. Bounded
    // rather than blocking: an owner that never answers is a real case
    // (a dead application still holding the selection), and an empty
    // paste is better than a frozen window.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (g_clipboard_reply_pending && std::chrono::steady_clock::now() < deadline) {
        pump();
        if (g_clipboard_reply_pending) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    g_clipboard_reply_pending = false;
    return g_clipboard_received;
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
