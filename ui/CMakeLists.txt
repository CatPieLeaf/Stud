# The launcher: plain Qt6 Widgets, which runs on any desktop. Network
# downloads Roblox's macOS client from Roblox's CDN.
# WebEngineWidgets and WebChannel show the client's web views (see
# src/webview_host.h).
find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets DBus Network WebEngineWidgets WebChannel)
# Headers only (see stud_ui_gpu_enum below). The headers' own package is
# enough; FindVulkan, the fallback where that is not installed, also
# insists on the loader's development library.
find_package(VulkanHeaders CONFIG QUIET)
if(NOT TARGET Vulkan::Headers)
    find_package(Vulkan REQUIRED)
endif()

# Real freedesktop.org Notifications via QtDBus; its own small
# library, no other Qt module needed beyond Core+DBus, independently
# testable (see tests/notifications_test.cpp).
add_library(stud_ui_notifications STATIC
    src/notifications.cpp
)
target_include_directories(stud_ui_notifications PUBLIC src)
target_link_libraries(stud_ui_notifications PUBLIC Qt6::Core Qt6::DBus)
# A notification names the desktop entry it belongs to, and that name is
# the application id; see the hint's own comment in notifications.cpp.
target_compile_definitions(stud_ui_notifications PRIVATE STUD_APP_ID="${STUD_APP_ID}")

# The tray's log export, writing a gzipped tar by hand (see
# log_archive.cpp for why by hand). Its own small library so the format
# can be tested without a display or a tray; see
# tests/log_archive_test.cpp.
add_library(stud_ui_log_archive STATIC
    src/log_archive.cpp
)
target_include_directories(stud_ui_log_archive PUBLIC src)
target_link_libraries(stud_ui_log_archive PUBLIC Qt6::Core miniz)

# Real roblox-player:/roblox: launch-URI parsing ("magnet link" style
# launch, see launch_uri.h). Its own small library, Qt6::Core only
# (for QUrl's real percent-decoding), independently testable (see
# tests/launch_uri_test.cpp).
# What Stud 1.x left behind, removed at the first 2.0 start.
add_library(stud_ui_stud1_cleanup STATIC
    src/stud1_cleanup.cpp
)
target_include_directories(stud_ui_stud1_cleanup PUBLIC src)
target_link_libraries(stud_ui_stud1_cleanup PUBLIC Qt6::Core)

# Every button icon: one colour, the palette's, following the scheme.
add_library(stud_ui_mono_icon STATIC
    src/mono_icon.cpp
)
target_include_directories(stud_ui_mono_icon PUBLIC src)
target_link_libraries(stud_ui_mono_icon PUBLIC Qt6::Widgets)

add_library(stud_ui_launch_uri STATIC
    src/launch_uri.cpp
)
target_include_directories(stud_ui_launch_uri PUBLIC src)
target_link_libraries(stud_ui_launch_uri PUBLIC Qt6::Core)

# Real GPU enumeration (vkEnumeratePhysicalDevices); no Qt dependency,
# kept as its own small library so it's independently testable (see
# tests/gpu_enum_test.cpp) rather than only reachable through the full
# Qt window.
add_library(stud_ui_gpu_enum STATIC
    src/gpu_enum.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/volk/volk.c"
)
target_include_directories(stud_ui_gpu_enum PUBLIC src)
target_include_directories(stud_ui_gpu_enum PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/volk")
# Headers only: the loader is opened at run time through volk, so the
# launcher starts on a machine with no Vulkan and Settings lists no GPUs
# there instead of failing to start with "libvulkan.so.1: cannot open".
target_link_libraries(stud_ui_gpu_enum PUBLIC Vulkan::Headers stud_config ${CMAKE_DL_LIBS})

# The client download: version check, download and unpack. Its own small
# library so the unpacking can be tested (tests/zip_extract_test.cpp).
add_library(stud_ui_mac_client STATIC
    src/mac_client.cpp
    src/customization.cpp
    src/fflags.cpp
    src/cpu_cores.cpp
    src/flatpak_permissions.cpp
    src/roblox_settings.cpp
)
target_include_directories(stud_ui_mac_client PUBLIC src)
target_link_libraries(stud_ui_mac_client PUBLIC Qt6::Core Qt6::Gui Qt6::Network stud_config miniz)
target_compile_definitions(stud_ui_mac_client PRIVATE STUD_VERSION="${STUD_VERSION}")

add_executable(stud-ui
    src/main.cpp
    src/desktop_entry.cpp
    src/diagnose.cpp
    src/discord_rpc.cpp
    src/game_activity.cpp
    src/history.cpp
    src/chooser.cpp
    src/settings_window.cpp
    src/project_links.cpp
    src/cube_view.cpp
    src/cube_gl.cpp
    src/update_check.cpp
    src/tray.cpp
    src/loading_screen.cpp
    src/webview_host.cpp
    resources/resources.qrc
)
target_link_libraries(stud-ui PRIVATE Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Network Qt6::DBus
                                       Qt6::WebEngineWidgets Qt6::WebChannel
                                       stud_ui_gpu_enum stud_config stud_ui_notifications stud_ui_mono_icon
                                       stud_ui_launch_uri stud_ui_log_archive stud_ui_mac_client
                                       stud_ui_stud1_cleanup miniz)

# The About tab's cube is the GL smoke test's own (tools/spinning_cube.h).
target_include_directories(stud-ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../tools")

set_target_properties(stud-ui PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    # So a crash report carries function names, not bare addresses.
    ENABLE_EXPORTS ON
)

target_compile_definitions(stud-ui PRIVATE STUD_APP_ID="${STUD_APP_ID}" STUD_VERSION="${STUD_VERSION}")

# The launcher is the only binary a user runs, so it is the only one on
# PATH. Rosemary lives in libexec and is found relative to it.
install(PROGRAMS "$<TARGET_FILE:stud-ui>"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME stud)
