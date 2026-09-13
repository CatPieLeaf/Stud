# jni_bridge_test/device_params_test/flag_overrides_test: real, still-
# valid tests for the framework's portable logic (JSON serialization,
# param-building) -- moved with the rest of jni-bridge into
# runtime/framework/ (now bionic-only, no host-side build target).
# Not wired back into the host-side test suite yet; revisit once the
# new runtime's own bring-up is proven, not blocking it.

add_executable(stud_media_format_test media_format_test.cpp)
target_link_libraries(stud_media_format_test PRIVATE stud_android_glue)
add_test(NAME media_format_test COMMAND stud_media_format_test)

# Phase 7: real regression test that Process B's bwrap sandbox never
# ends up binding a real host GPU driver path (see bionic_runtime.h's
# own doc comment on build_process_b_argv() for why this matters).
add_executable(stud_bionic_runtime_sandbox_test bionic_runtime_sandbox_test.cpp)
target_link_libraries(stud_bionic_runtime_sandbox_test PRIVATE stud_bionic_runtime)
add_test(NAME bionic_runtime_sandbox_test COMMAND stud_bionic_runtime_sandbox_test)

# M7: real APK asset extraction, end-to-end through AAssetManager.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/toy_apk.apk
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_toy_apk.py
            ${CMAKE_CURRENT_BINARY_DIR}/toy_apk.apk
    DEPENDS fixtures/make_toy_apk.py
    COMMENT "Building toy_apk.apk fixture for the apk-extract test"
)
add_custom_target(stud_toy_apk_fixture DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/toy_apk.apk)

add_executable(stud_apk_extract_test apk_extract_test.cpp)
target_link_libraries(stud_apk_extract_test PRIVATE stud_android_glue)
add_dependencies(stud_apk_extract_test stud_toy_apk_fixture)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/apk_extract_scratch)
add_test(NAME apk_extract_test COMMAND stud_apk_extract_test
         ${CMAKE_CURRENT_BINARY_DIR}/toy_apk.apk
         ${CMAKE_CURRENT_BINARY_DIR}/apk_extract_scratch)

# M6: ANativeWindow's real Wayland backing.
add_executable(stud_native_window_test native_window_test.cpp)
target_link_libraries(stud_native_window_test PRIVATE stud_android_glue)
add_test(NAME native_window_test COMMAND stud_native_window_test)

# M5 CPU compat test.
add_executable(stud_cpu_compat_test cpu_compat_test.cpp)
target_link_libraries(stud_cpu_compat_test PRIVATE stud_cpu_compat)
add_test(NAME cpu_compat_test COMMAND stud_cpu_compat_test)

# M6 render dispatch test: portable fixtures standing in for a real ANGLE
# build (see render_test.cpp for why real ANGLE isn't used here).
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/toy_egl_lib.so
    COMMAND ${CMAKE_C_COMPILER} -fPIC -shared -Wl,--hash-style=sysv
            -o ${CMAKE_CURRENT_BINARY_DIR}/toy_egl_lib.so
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/toy_egl_lib.c
    DEPENDS fixtures/toy_egl_lib.c
    COMMENT "Building toy_egl_lib.so fixture for the render test"
)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/toy_gles_lib.so
    COMMAND ${CMAKE_C_COMPILER} -fPIC -shared -Wl,--hash-style=sysv
            -o ${CMAKE_CURRENT_BINARY_DIR}/toy_gles_lib.so
            ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/toy_gles_lib.c
    DEPENDS fixtures/toy_gles_lib.c
    COMMENT "Building toy_gles_lib.so fixture for the render test"
)
add_custom_target(stud_toy_render_fixtures DEPENDS
    ${CMAKE_CURRENT_BINARY_DIR}/toy_egl_lib.so
    ${CMAKE_CURRENT_BINARY_DIR}/toy_gles_lib.so
)

add_executable(stud_render_test render_test.cpp)
target_link_libraries(stud_render_test PRIVATE stud_render)
add_dependencies(stud_render_test stud_toy_render_fixtures)

add_test(NAME render_test COMMAND stud_render_test
         ${CMAKE_CURRENT_BINARY_DIR}/toy_egl_lib.so
         ${CMAKE_CURRENT_BINARY_DIR}/toy_gles_lib.so)

if(STUD_BUILD_UI)
    # M8: real GPU enumeration against this machine's actual Vulkan driver.
    add_executable(stud_gpu_enum_test gpu_enum_test.cpp)
    target_link_libraries(stud_gpu_enum_test PRIVATE stud_ui_gpu_enum)
    add_test(NAME gpu_enum_test COMMAND stud_gpu_enum_test)

    # M8: real QtKeychain-backed credential storage against this
    # machine's real keychain backend (KWallet, under this live Plasma
    # session).
    add_executable(stud_credential_store_test credential_store_test.cpp)
    target_link_libraries(stud_credential_store_test PRIVATE stud_ui_credential_store)
    add_test(NAME credential_store_test COMMAND stud_credential_store_test)


    # M8: real freedesktop Notifications against this machine's live
    # D-Bus session bus / notification daemon.
    add_executable(stud_notifications_test notifications_test.cpp)
    target_link_libraries(stud_notifications_test PRIVATE stud_ui_notifications)
    add_test(NAME notifications_test COMMAND stud_notifications_test)

    # M8: real roblox-player:/roblox: launch-URI parsing.
    add_executable(stud_launch_uri_test launch_uri_test.cpp)
    target_link_libraries(stud_launch_uri_test PRIVATE stud_ui_launch_uri)
    add_test(NAME launch_uri_test COMMAND stud_launch_uri_test)

    # The log export writes a gzipped tar by hand; this checks the result
    # against the format, and against a real tar where there is one.
    add_executable(stud_log_archive_test log_archive_test.cpp)
    target_link_libraries(stud_log_archive_test PRIVATE stud_ui_log_archive)
    add_test(NAME log_archive_test COMMAND stud_log_archive_test)
endif()

# M8: real UI-runtime IPC (Unix domain socket launch handoff).
add_executable(stud_ipc_test ipc_test.cpp)
target_link_libraries(stud_ipc_test PRIVATE stud_ipc)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ipc_scratch)
add_test(NAME ipc_test COMMAND stud_ipc_test ${CMAKE_CURRENT_BINARY_DIR}/ipc_scratch)

# M8: general settings storage.
add_executable(stud_settings_test settings_test.cpp)
target_link_libraries(stud_settings_test PRIVATE stud_config)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/settings_scratch)
add_test(NAME settings_test COMMAND stud_settings_test
         ${CMAKE_CURRENT_BINARY_DIR}/settings_scratch)

if(STUD_ENABLE_DEV_RENDER_TOGGLE)
    add_executable(stud_dev_backend_config_test dev_backend_config_test.cpp)
    target_link_libraries(stud_dev_backend_config_test PRIVATE stud_render)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/dev_backend_config_scratch)
    add_test(NAME dev_backend_config_test COMMAND stud_dev_backend_config_test
             ${CMAKE_CURRENT_BINARY_DIR}/dev_backend_config_scratch)
endif()
