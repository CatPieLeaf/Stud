# WebEngineWidgets and WebChannel show the client's web views.
find_package(Qt6 REQUIRED COMPONENTS Widgets DBus Network WebEngineWidgets WebChannel)
add_library(stud_ui_notifications STATIC
    src/notifications.cpp
)
target_include_directories(stud_ui_notifications PUBLIC src)
target_link_libraries(stud_ui_notifications PUBLIC Qt6::Core Qt6::DBus)
# Notifications name their desktop entry, which is the application id.
target_compile_definitions(stud_ui_notifications PRIVATE STUD_APP_ID="${STUD_APP_ID}")

# Writes a gzipped tar by hand; see log_archive.cpp.
add_library(stud_ui_log_archive STATIC
    src/log_archive.cpp
)
target_include_directories(stud_ui_log_archive PUBLIC src)
target_link_libraries(stud_ui_log_archive PUBLIC Qt6::Core miniz)

# What Stud 1.x left behind, removed at the first 2.0 start.
add_library(stud_ui_stud1_cleanup STATIC
    src/stud1_cleanup.cpp
)
target_include_directories(stud_ui_stud1_cleanup PUBLIC src)
target_link_libraries(stud_ui_stud1_cleanup PUBLIC Qt6::Core)

# Monochrome button icons that follow the colour scheme.
add_library(stud_ui_mono_icon STATIC
    src/mono_icon.cpp
)
target_include_directories(stud_ui_mono_icon PUBLIC src)
target_link_libraries(stud_ui_mono_icon PUBLIC Qt6::Widgets)

add_library(stud_ui_launch_uri STATIC
    src/launch_uri.cpp
)
target_include_directories(stud_ui_launch_uri PUBLIC src)

add_library(stud_ui_gpu_enum STATIC
    src/gpu_enum.cpp
)
target_include_directories(stud_ui_gpu_enum PUBLIC src)
target_link_libraries(stud_ui_gpu_enum PUBLIC stud_config)

# The login cookies kept in the system keyring (login_keyring.cpp).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)

# Apple's root certificate for checking Roblox's code signature (code_signature.cpp).
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/apple_root_ca.h"
    COMMAND "${CMAKE_COMMAND}" -DIN=${CMAKE_CURRENT_SOURCE_DIR}/resources/apple-root-ca.cer -DNAME=kAppleRootCa
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/apple_root_ca.h
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/apple-root-ca.cer" "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake"
    VERBATIM)

add_library(stud_ui_mac_client STATIC
    src/mac_client.cpp
    src/code_signature.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/apple_root_ca.h"
    src/customization.cpp
    src/fflags.cpp
    src/cpu_cores.cpp
    src/flatpak_permissions.cpp
    src/roblox_settings.cpp
)
target_include_directories(stud_ui_mac_client PUBLIC src)
target_include_directories(stud_ui_mac_client PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(stud_ui_mac_client PUBLIC Qt6::Core Qt6::Gui Qt6::Network stud_config miniz
                                         PRIVATE OpenSSL::Crypto)
target_compile_definitions(stud_ui_mac_client PRIVATE STUD_VERSION="${STUD_VERSION}" STUD_APP_ID="${STUD_APP_ID}")

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
    src/cube.cpp
    src/update_check.cpp
    src/tray.cpp
    src/loading_screen.cpp
    src/webview_host.cpp
    src/log_export.cpp
    src/launcher_portal.cpp
    src/login_keyring.cpp
    resources/resources.qrc
)
target_link_libraries(stud-ui PRIVATE Qt6::Widgets Qt6::Network Qt6::DBus
                                       Qt6::WebEngineWidgets Qt6::WebChannel
                                       stud_ui_gpu_enum stud_config stud_ui_notifications stud_ui_mono_icon
                                       stud_ui_launch_uri stud_ui_log_archive stud_ui_mac_client
                                       stud_ui_stud1_cleanup miniz PkgConfig::LIBSECRET)


set_target_properties(stud-ui PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    # Function names in crash reports.
    ENABLE_EXPORTS ON
)

target_compile_definitions(stud-ui PRIVATE STUD_APP_ID="${STUD_APP_ID}" STUD_VERSION="${STUD_VERSION}")

# Shown in the session log's header. Builds from an archive without .git can
# pass -DSTUD_COMMIT=<commit>.
set(STUD_COMMIT "" CACHE STRING "Commit the source tree is, when it has no .git to ask")
add_custom_target(stud_commit
    COMMAND "${CMAKE_COMMAND}" -DSOURCE=${CMAKE_SOURCE_DIR} -DCOMMIT=${STUD_COMMIT}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/stud_commit.h
            -P "${CMAKE_SOURCE_DIR}/cmake/stud_commit.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/stud_commit.h"
    VERBATIM)
add_dependencies(stud-ui stud_commit)
target_include_directories(stud-ui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# Only the launcher goes on PATH; Rosemary is found relative to it in libexec.
install(PROGRAMS "$<TARGET_FILE:stud-ui>"
        DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME stud)
