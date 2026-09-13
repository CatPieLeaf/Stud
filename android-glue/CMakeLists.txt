include(FetchContent)
# M7: real APK zip extraction for extract_apk_assets() (apk_extract.cpp)
# -- MIT-licensed, single-amalgamated-header, no system package needed
# (matches the project's existing nlohmann_json/libjnivm vendoring
# pattern rather than requiring a sudo-installed system zip library).
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.1.2
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# Amalgamated output at this tag has a real double-declaration bug
# (miniz.h ends up concatenated with itself) -- the plain multi-header
# form (miniz.h + miniz_zip.h, both real, working headers) avoids it.
set(AMALGAMATE_SOURCES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(miniz)

# Real xdg-shell protocol bindings, generated at configure time from the
# system's installed wayland-protocols package -- gives ANativeWindow's
# Wayland backing (native_window.cpp) an xdg_toplevel role, the real
# mechanism that makes a wl_surface an actual visible, mapped window
# (Vulkan surface *creation* already worked without this; *presenting
# frames onto a visible window* needs it -- see the engineering notes' M6
# entry). Generated, not vendored/committed -- wayland-scanner's output
# is deterministic from the standard, stable xdg-shell protocol XML, no
# reason to check in ~90KB of generated code when any dev machine with
# wayland-protocols installed can regenerate it identically.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandProtocols REQUIRED wayland-protocols)
pkg_get_variable(WaylandProtocolsDataDir wayland-protocols pkgdatadir)
find_program(WaylandScanner NAMES wayland-scanner REQUIRED)

set(XdgShellXml "${WaylandProtocolsDataDir}/stable/xdg-shell/xdg-shell.xml")
set(XdgShellGeneratedDir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(XdgShellHeader "${XdgShellGeneratedDir}/xdg-shell-client-protocol.h")
set(XdgShellSource "${XdgShellGeneratedDir}/xdg-shell-protocol.c")
file(MAKE_DIRECTORY "${XdgShellGeneratedDir}")
add_custom_command(
    OUTPUT "${XdgShellHeader}"
    COMMAND "${WaylandScanner}" client-header "${XdgShellXml}" "${XdgShellHeader}"
    DEPENDS "${XdgShellXml}"
)
add_custom_command(
    OUTPUT "${XdgShellSource}"
    COMMAND "${WaylandScanner}" private-code "${XdgShellXml}" "${XdgShellSource}"
    DEPENDS "${XdgShellXml}"
)

# Real xdg-decoration protocol bindings, same generation mechanism as
# xdg-shell above -- explicitly requests SERVER_SIDE decoration mode
# for Stud's own toplevel (the engineering notes, real user report: "must have
# titlebar"). Without this, whether a titlebar appears is purely up to
# the compositor's own default for undecorated clients (observed: none)
# -- most compositors implement CSD-by-default unless a client opts
# into SSD via this real, standard protocol. Stud draws no decorations
# of its own, so SSD is the only real option, not a preference.
set(XdgDecorationXml "${WaylandProtocolsDataDir}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
set(XdgDecorationHeader "${XdgShellGeneratedDir}/xdg-decoration-unstable-v1-client-protocol.h")
set(XdgDecorationSource "${XdgShellGeneratedDir}/xdg-decoration-unstable-v1-protocol.c")
add_custom_command(
    OUTPUT "${XdgDecorationHeader}"
    COMMAND "${WaylandScanner}" client-header "${XdgDecorationXml}" "${XdgDecorationHeader}"
    DEPENDS "${XdgDecorationXml}"
)
add_custom_command(
    OUTPUT "${XdgDecorationSource}"
    COMMAND "${WaylandScanner}" private-code "${XdgDecorationXml}" "${XdgDecorationSource}"
    DEPENDS "${XdgDecorationXml}"
)

# Real fractional-scale + viewporter bindings, same generation mechanism.
# wl_output.scale is only ever an INTEGER, so on a fractionally-scaled
# desktop (a real, common KDE/GNOME setting) it rounds up -- a 1.25x
# desktop advertises 2, and a client that believes it renders 60% more
# pixels than needed and still has to guess the window's real size. These
# two protocols are how a client learns the exact factor
# (wp_fractional_scale_v1.preferred_scale, in 120ths) and tells the
# compositor what logical size its buffer represents (wp_viewporter).
set(FractionalScaleXml "${WaylandProtocolsDataDir}/staging/fractional-scale/fractional-scale-v1.xml")
set(FractionalScaleHeader "${XdgShellGeneratedDir}/fractional-scale-v1-client-protocol.h")
set(FractionalScaleSource "${XdgShellGeneratedDir}/fractional-scale-v1-protocol.c")
add_custom_command(
    OUTPUT "${FractionalScaleHeader}"
    COMMAND "${WaylandScanner}" client-header "${FractionalScaleXml}" "${FractionalScaleHeader}"
    DEPENDS "${FractionalScaleXml}"
)
add_custom_command(
    OUTPUT "${FractionalScaleSource}"
    COMMAND "${WaylandScanner}" private-code "${FractionalScaleXml}" "${FractionalScaleSource}"
    DEPENDS "${FractionalScaleXml}"
)

# Real xdg-output bindings, same generation mechanism. This is the one
# protocol that reports a display's LOGICAL size directly, with no
# surface involved: wl_output only gives the physical mode and an integer
# scale, and a fractional scale is reported per-surface and only once a
# surface is MAPPED. So before Stud's window exists there is no other way
# to know that a 1920x1080 panel is a 1536x864 desktop -- which is what
# sizing the default window, and every number derived from the scale,
# actually needs.
# Real xdg-activation bindings. A Wayland compositor will not raise a
# window a client launches unless the launcher hands over an activation
# TOKEN -- without one the browser opens behind Stud, which is exactly
# what the user reported. This is the protocol that mints those tokens.
set(XdgActivationXml "${WaylandProtocolsDataDir}/staging/xdg-activation/xdg-activation-v1.xml")
set(XdgActivationHeader "${XdgShellGeneratedDir}/xdg-activation-v1-client-protocol.h")
set(XdgActivationSource "${XdgShellGeneratedDir}/xdg-activation-v1-protocol.c")
add_custom_command(
    OUTPUT "${XdgActivationHeader}"
    COMMAND "${WaylandScanner}" client-header "${XdgActivationXml}" "${XdgActivationHeader}"
    DEPENDS "${XdgActivationXml}"
)
add_custom_command(
    OUTPUT "${XdgActivationSource}"
    COMMAND "${WaylandScanner}" private-code "${XdgActivationXml}" "${XdgActivationSource}"
    DEPENDS "${XdgActivationXml}"
)

set(XdgOutputXml "${WaylandProtocolsDataDir}/unstable/xdg-output/xdg-output-unstable-v1.xml")
set(XdgOutputHeader "${XdgShellGeneratedDir}/xdg-output-unstable-v1-client-protocol.h")
set(XdgOutputSource "${XdgShellGeneratedDir}/xdg-output-unstable-v1-protocol.c")
add_custom_command(
    OUTPUT "${XdgOutputHeader}"
    COMMAND "${WaylandScanner}" client-header "${XdgOutputXml}" "${XdgOutputHeader}"
    DEPENDS "${XdgOutputXml}"
)
add_custom_command(
    OUTPUT "${XdgOutputSource}"
    COMMAND "${WaylandScanner}" private-code "${XdgOutputXml}" "${XdgOutputSource}"
    DEPENDS "${XdgOutputXml}"
)

# Real pointer-constraints + relative-pointer bindings, same generation
# mechanism. These are how a Wayland client implements mouse look: the
# compositor stops moving the physical pointer (lock_pointer) and delivers
# raw motion deltas instead (zwp_relative_pointer_v1). Roblox drives camera
# rotation exactly this way on a real device, via Android's own pointer
# capture -- without them the real pointer keeps travelling across the
# desktop while the camera turns, and the engine's cursor lands wherever it
# ended up the moment the button is released.
set(PointerConstraintsXml "${WaylandProtocolsDataDir}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml")
set(PointerConstraintsHeader "${XdgShellGeneratedDir}/pointer-constraints-unstable-v1-client-protocol.h")
set(PointerConstraintsSource "${XdgShellGeneratedDir}/pointer-constraints-unstable-v1-protocol.c")
add_custom_command(
    OUTPUT "${PointerConstraintsHeader}"
    COMMAND "${WaylandScanner}" client-header "${PointerConstraintsXml}" "${PointerConstraintsHeader}"
    DEPENDS "${PointerConstraintsXml}"
)
add_custom_command(
    OUTPUT "${PointerConstraintsSource}"
    COMMAND "${WaylandScanner}" private-code "${PointerConstraintsXml}" "${PointerConstraintsSource}"
    DEPENDS "${PointerConstraintsXml}"
)

set(RelativePointerXml "${WaylandProtocolsDataDir}/unstable/relative-pointer/relative-pointer-unstable-v1.xml")
set(RelativePointerHeader "${XdgShellGeneratedDir}/relative-pointer-unstable-v1-client-protocol.h")
set(RelativePointerSource "${XdgShellGeneratedDir}/relative-pointer-unstable-v1-protocol.c")
add_custom_command(
    OUTPUT "${RelativePointerHeader}"
    COMMAND "${WaylandScanner}" client-header "${RelativePointerXml}" "${RelativePointerHeader}"
    DEPENDS "${RelativePointerXml}"
)
add_custom_command(
    OUTPUT "${RelativePointerSource}"
    COMMAND "${WaylandScanner}" private-code "${RelativePointerXml}" "${RelativePointerSource}"
    DEPENDS "${RelativePointerXml}"
)

set(ViewporterXml "${WaylandProtocolsDataDir}/stable/viewporter/viewporter.xml")
set(ViewporterHeader "${XdgShellGeneratedDir}/viewporter-client-protocol.h")
set(ViewporterSource "${XdgShellGeneratedDir}/viewporter-protocol.c")
add_custom_command(
    OUTPUT "${ViewporterHeader}"
    COMMAND "${WaylandScanner}" client-header "${ViewporterXml}" "${ViewporterHeader}"
    DEPENDS "${ViewporterXml}"
)
add_custom_command(
    OUTPUT "${ViewporterSource}"
    COMMAND "${WaylandScanner}" private-code "${ViewporterXml}" "${ViewporterSource}"
    DEPENDS "${ViewporterXml}"
)

# Real pointer-gestures bindings, same generation mechanism. A touchpad
# pinch is a first-class Wayland event, not two synthesised scrolls, and
# it is the only way a client learns the real scale factor. The app's own
# handler has a mouse branch for exactly this shape
# (`nativePassMousePinch(x, y, delta)`), which Stud has never had
# anything to feed.
set(PointerGesturesXml "${WaylandProtocolsDataDir}/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml")
set(PointerGesturesHeader "${XdgShellGeneratedDir}/pointer-gestures-unstable-v1-client-protocol.h")
set(PointerGesturesSource "${XdgShellGeneratedDir}/pointer-gestures-unstable-v1-protocol.c")
add_custom_command(
    OUTPUT "${PointerGesturesHeader}"
    COMMAND "${WaylandScanner}" client-header "${PointerGesturesXml}" "${PointerGesturesHeader}"
    DEPENDS "${PointerGesturesXml}"
)
add_custom_command(
    OUTPUT "${PointerGesturesSource}"
    COMMAND "${WaylandScanner}" private-code "${PointerGesturesXml}" "${PointerGesturesSource}"
    DEPENDS "${PointerGesturesXml}"
)

# The window icon, as _NET_WM_ICON's own raw ARGB array. X11 has no way
# to point a window at an icon file, so it has to be in the binary --
# generated at configure time from the same PNGs the packages install, so
# there is no second copy to keep in step and no image decoder in a
# process with no other use for one.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WindowIconHeader "${XdgShellGeneratedDir}/stud_window_icon.h")
set(WindowIconSources
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icons/32.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icons/48.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icons/128.png")
add_custom_command(
    OUTPUT "${WindowIconHeader}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/png_to_argb_header.py"
            "${WindowIconHeader}" ${WindowIconSources}
    DEPENDS ${WindowIconSources}
            "${CMAKE_CURRENT_SOURCE_DIR}/../tools/png_to_argb_header.py"
)

add_library(stud_android_glue STATIC
    src/asset_manager.cpp
    src/apk_extract.cpp
    src/apk_manifest.cpp
    src/configuration.cpp
    src/looper.cpp
    src/native_window.cpp
    src/x11_backend.cpp
    src/text_overlay.cpp
    src/clipboard.cpp
    src/media_codec.cpp
    src/android_glue.cpp
    "${XdgShellSource}"
    "${XdgShellHeader}"
    "${XdgDecorationSource}"
    "${XdgDecorationHeader}"
    "${FractionalScaleSource}"
    "${XdgOutputSource}"
    "${XdgActivationSource}"
    "${FractionalScaleHeader}"
    "${XdgOutputHeader}"
    "${XdgActivationHeader}"
    "${ViewporterSource}"
    "${ViewporterHeader}"
    "${PointerConstraintsSource}"
    "${PointerConstraintsHeader}"
    "${RelativePointerSource}"
    "${RelativePointerHeader}"
    "${PointerGesturesSource}"
    "${PointerGesturesHeader}"
    "${WindowIconHeader}"
)
target_include_directories(stud_android_glue PUBLIC "${XdgShellGeneratedDir}")
target_include_directories(stud_android_glue PUBLIC include)

# X11 headers, for types only.
#
# The X11 backend (src/x11_backend.cpp) resolves every Xlib entry point
# with dlopen/dlsym at runtime, exactly as the Wayland side treats
# optional protocols -- so nothing here links libX11 and a machine
# without it still runs Stud on Wayland. The headers are a build
# dependency (libX11-devel / libx11-dev) and nothing more: XEvent is a
# large union and hand-declaring it would be a real ABI risk for no gain.
find_path(StudX11IncludeDir NAMES X11/Xlib.h)
if(NOT StudX11IncludeDir)
    message(FATAL_ERROR
            "Stud: X11 headers not found (X11/Xlib.h). Install libX11-devel "
            "(Fedora) or libx11-dev (Debian/Ubuntu) -- they are needed to "
            "build the X11 window backend, which is loaded at runtime only.")
endif()
target_include_directories(stud_android_glue PRIVATE "${StudX11IncludeDir}")
# For jni.h (JNIEnv/jobject types used by AAssetManager_fromJava/
# ANativeWindow_fromSurface's signatures) -- jnivm is already fetched by
# jni-bridge's CMakeLists.txt, which must run before this one (see the
# add_subdirectory order in the top-level CMakeLists.txt).
target_link_libraries(stud_android_glue PUBLIC jnivm miniz)

# Real wl_display_connect()/wl_compositor_create_surface() for
# ANativeWindow's Wayland backing (see native_window.cpp) -- Wayland-only
# per the locked windowing decision (the engineering notes), standard system
# dev package (already present on this host, wayland-devel).
find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandClient REQUIRED IMPORTED_TARGET wayland-client)
target_link_libraries(stud_android_glue PUBLIC PkgConfig::WaylandClient)

# Real wl_egl_window_create()/wl_egl_window_resize() -- now owned here
# (native_window_get_or_create_egl_window(), the real fix for
# the engineering notes' "can't be resized" report), shared between whoever
# creates the render context and this module's own xdg_toplevel_configure
# resize handling.
pkg_check_modules(WaylandEgl REQUIRED IMPORTED_TARGET wayland-egl)
target_link_libraries(stud_android_glue PUBLIC PkgConfig::WaylandEgl)

# Real glyph rasterisation for the native text input the engine asks Stud
# to draw over a focused TextBox (src/text_overlay.cpp). Roblox ships its
# own fonts in the APK and tells Stud which one to use, so the overlay
# renders in the engine's own typeface rather than a desktop substitute.
pkg_check_modules(Freetype REQUIRED IMPORTED_TARGET freetype2)
target_link_libraries(stud_android_glue PUBLIC PkgConfig::Freetype)

target_compile_definitions(stud_android_glue PRIVATE STUD_APP_ID="${STUD_APP_ID}")
