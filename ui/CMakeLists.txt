# M8. Plain Qt6 Widgets (revised from the original "Qt6 + libplasma"
# decision -- libplasma is the Plasma shell/plasmoid framework, not a
# general KDE app toolkit; see the engineering notes' locked-decisions table
# for the full reasoning). No GTK, no WebKitGTK, no libplasma dependency
# -- runs on any DE, matching the AppImage run-anywhere goal.
# QtWebEngine is deliberately NOT required any more: Stud logs in the
# way Sober does, inside the real Roblox app's own login screen, so
# there is no web view to host and no session cookie to scrape.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Network)
find_package(Vulkan REQUIRED)
find_package(Qt6Keychain REQUIRED)

# Real freedesktop.org Notifications via QtDBus -- its own small
# library, no other Qt module needed beyond Core+DBus, independently
# testable (see tests/notifications_test.cpp).
add_library(stud_ui_notifications STATIC
    src/notifications.cpp
)
target_include_directories(stud_ui_notifications PUBLIC src)
target_link_libraries(stud_ui_notifications PUBLIC Qt6::Core Qt6::DBus)
# A notification names the desktop entry it belongs to, and that name is
# the application id -- see the hint's own comment in notifications.cpp.
target_compile_definitions(stud_ui_notifications PRIVATE STUD_APP_ID="${STUD_APP_ID}")

# The tray's log export, writing a gzipped tar by hand (see
# log_archive.cpp for why by hand). Its own small library so the format
# can be tested without a display or a tray -- see
# tests/log_archive_test.cpp.
add_library(stud_ui_log_archive STATIC
    src/log_archive.cpp
)
target_include_directories(stud_ui_log_archive PUBLIC src)
target_link_libraries(stud_ui_log_archive PUBLIC Qt6::Core miniz)

# Real roblox-player:/roblox: launch-URI parsing ("magnet link" style
# launch, see launch_uri.h) -- its own small library, Qt6::Core only
# (for QUrl's real percent-decoding), independently testable (see
# tests/launch_uri_test.cpp).
add_library(stud_ui_launch_uri STATIC
    src/launch_uri.cpp
)
target_include_directories(stud_ui_launch_uri PUBLIC src)
target_link_libraries(stud_ui_launch_uri PUBLIC Qt6::Core)


# Real GPU enumeration (vkEnumeratePhysicalDevices) -- no Qt dependency,
# kept as its own small library so it's independently testable (see
# tests/gpu_enum_test.cpp) rather than only reachable through the full
# Qt window.
add_library(stud_ui_gpu_enum STATIC
    src/gpu_enum.cpp
)
target_include_directories(stud_ui_gpu_enum PUBLIC src)
target_link_libraries(stud_ui_gpu_enum PUBLIC Vulkan::Headers ${CMAKE_DL_LIBS})
# Stud's OWN direct Vulkan usage (this process only enumerates GPUs for
# the Settings UI -- the render-host/process-b Vulkan proxy, Phase 4, is
# a separate, IPC-based design, not an in-process dlopen interposition
# like the old, now-deleted vulkan-wsi/ module) -- linking the real
# loader directly is correct and intentional here.
target_link_libraries(stud_ui_gpu_enum PRIVATE Vulkan::Vulkan)

# Real QtKeychain-backed credential storage -- its own small library,
# same reasoning as stud_ui_gpu_enum (independently testable, see
# tests/credential_store_test.cpp).
add_library(stud_ui_credential_store STATIC
    src/credential_store.cpp
    src/safe_storage.cpp
)
# src for its own headers, android-glue's for stud/stud_paths.h -- the
# secrets directory has to agree with the other two processes.
target_include_directories(stud_ui_credential_store PUBLIC src
                            "${CMAKE_CURRENT_SOURCE_DIR}/../android-glue/include")
find_package(OpenSSL REQUIRED)
target_link_libraries(stud_ui_credential_store PUBLIC Qt6::Core Qt6Keychain::Qt6Keychain
                                                     OpenSSL::Crypto)

add_executable(stud-ui
    src/main.cpp
    src/desktop_entry.cpp
    src/settings_window.cpp
    src/tray.cpp
    resources/resources.qrc
)
target_link_libraries(stud-ui PRIVATE Qt6::Widgets Qt6::Network stud_ui_gpu_enum stud_config stud_ui_credential_store
                                       stud_ui_notifications stud_ui_launch_uri stud_ui_log_archive
                                       stud_ipc stud_android_glue stud_render stud_bionic_runtime)

set_target_properties(stud-ui PROPERTIES
    AUTOMOC ON
    AUTORCC ON
)

target_compile_definitions(stud-ui PRIVATE STUD_APP_ID="${STUD_APP_ID}")

# The launcher is the only binary a user runs, so it is the only one on
# PATH. The others live in libexec and are found relative to it.
install(PROGRAMS "$<TARGET_FILE:stud-ui>"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME stud)
