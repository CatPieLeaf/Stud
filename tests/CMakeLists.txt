# The framework's portable logic, built for the host. runtime/framework is
# otherwise bionic-only, so each test takes just the sources it needs.
set(StudFramework "${PROJECT_SOURCE_DIR}/runtime/framework")
function(stud_framework_test name)
    add_executable(stud_${name} ${name}.cpp ${ARGN})
    target_include_directories(stud_${name} PRIVATE
        "${StudFramework}/include"
        "${PROJECT_SOURCE_DIR}/bionic-symbol-lookup/include"
        "${PROJECT_SOURCE_DIR}/android-glue/include")
    target_link_libraries(stud_${name} PRIVATE fake-jni nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
endfunction()

stud_framework_test(jni_bridge_test "${StudFramework}/src/platform_params.cpp")
add_test(NAME jni_bridge_test COMMAND stud_jni_bridge_test)

stud_framework_test(device_params_test
    "${StudFramework}/src/device_params.cpp" "${StudFramework}/src/init_params.cpp"
    "${StudFramework}/src/platform_params.cpp")
add_test(NAME device_params_test COMMAND stud_device_params_test)

stud_framework_test(flag_overrides_test
    "${StudFramework}/src/flag_overrides.cpp" "${StudFramework}/src/trap_recovery.cpp")
add_test(NAME flag_overrides_test COMMAND stud_flag_overrides_test
         ${CMAKE_CURRENT_BINARY_DIR}/flag_overrides_scratch)

# Optimised whatever the build type: the bug it guards against only exists
# where the compiler keeps a local in a register across siglongjmp().
stud_framework_test(trap_recovery_test "${StudFramework}/src/trap_recovery.cpp")
target_compile_options(stud_trap_recovery_test PRIVATE -O2)
add_test(NAME trap_recovery_test COMMAND stud_trap_recovery_test)

add_executable(stud_atomic_file_test atomic_file_test.cpp)
target_include_directories(stud_atomic_file_test PRIVATE "${StudFramework}/include")
add_test(NAME atomic_file_test COMMAND stud_atomic_file_test
         ${CMAKE_CURRENT_BINARY_DIR}/atomic_file_scratch)

add_executable(stud_socket_reader_test socket_reader_test.cpp)
target_include_directories(stud_socket_reader_test PRIVATE ${PROJECT_SOURCE_DIR}/render-host/src)
add_test(NAME socket_reader_test COMMAND stud_socket_reader_test)

add_executable(stud_discord_rpc_test discord_rpc_test.cpp
               ${PROJECT_SOURCE_DIR}/render-host/src/discord_rpc.cpp)
target_include_directories(stud_discord_rpc_test PRIVATE ${PROJECT_SOURCE_DIR}/render-host/include)
find_package(Threads REQUIRED)
target_link_libraries(stud_discord_rpc_test PRIVATE Threads::Threads)
add_test(NAME discord_rpc_test COMMAND stud_discord_rpc_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

add_executable(stud_window_memory_test window_memory_test.cpp)
target_include_directories(stud_window_memory_test PRIVATE ${PROJECT_SOURCE_DIR}/android-glue/include)
add_test(NAME window_memory_test COMMAND stud_window_memory_test)

add_executable(stud_audio_limiter_test audio_limiter_test.cpp)
target_include_directories(stud_audio_limiter_test PRIVATE ${PROJECT_SOURCE_DIR}/render-host/include)
add_test(NAME audio_limiter_test COMMAND stud_audio_limiter_test)

add_executable(stud_range_inside_test range_inside_test.cpp)
target_include_directories(stud_range_inside_test PRIVATE ${PROJECT_SOURCE_DIR}/render-host/include)
add_test(NAME range_inside_test COMMAND stud_range_inside_test)

add_executable(stud_symbol_names_test symbol_names_test.cpp)
target_include_directories(stud_symbol_names_test PRIVATE
    ${PROJECT_SOURCE_DIR}/bionic-symbol-lookup/include ${PROJECT_SOURCE_DIR}/android-glue/include)
target_link_libraries(stud_symbol_names_test PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME symbol_names_test COMMAND stud_symbol_names_test)

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
           ${CMAKE_CURRENT_BINARY_DIR}/toy_bundle.apkm
           ${CMAKE_CURRENT_BINARY_DIR}/toy_bundle.xapk
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/make_toy_apk.py
            ${CMAKE_CURRENT_BINARY_DIR}/toy_apk.apk
    DEPENDS fixtures/make_toy_apk.py
    COMMENT "Building toy_apk.apk fixture for the apk-extract test"
)
add_custom_target(stud_toy_apk_fixture DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/toy_apk.apk)

add_executable(stud_apk_check_test apk_check_test.cpp)
target_link_libraries(stud_apk_check_test PRIVATE stud_android_glue)
add_test(NAME apk_check_test COMMAND stud_apk_check_test)

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

# Key mapping, checked against real compiled keymaps (see the test's own
# header comment). Header-only, so it needs no runtime target, only the
# same xkbcommon the compositor's keymap is read with.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XkbcommonTest REQUIRED IMPORTED_TARGET xkbcommon)
add_executable(stud_key_map_test key_map_test.cpp)
target_include_directories(stud_key_map_test PRIVATE
    "${CMAKE_SOURCE_DIR}/runtime/framework/include"
    "${CMAKE_SOURCE_DIR}/android-glue/include")
target_link_libraries(stud_key_map_test PRIVATE PkgConfig::XkbcommonTest)
add_test(NAME key_map_test COMMAND stud_key_map_test)
set_tests_properties(key_map_test PROPERTIES SKIP_RETURN_CODE 77)

# Who gets the session, what a MessageBus payload says, and where a caret is.
# Header-only, no dependencies.
add_executable(stud_web_text_test web_text_test.cpp)
target_include_directories(stud_web_text_test PRIVATE
    "${CMAKE_SOURCE_DIR}/android-glue/include"
    "${CMAKE_SOURCE_DIR}/runtime/framework/include")
add_test(NAME web_text_test COMMAND stud_web_text_test)

# Which language Stud reports to Roblox. Header-only, no dependencies,
# the table it checks is transcribed from the real app's own code.
add_executable(stud_locale_test locale_test.cpp)
target_include_directories(stud_locale_test PRIVATE "${CMAKE_SOURCE_DIR}/android-glue/include")
add_test(NAME locale_test COMMAND stud_locale_test)

# Render dispatch test: portable fixtures standing in for a real EGL/GLES
# driver (see render_test.cpp for why a real one isn't used here).
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
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    # These three use what the desktop provides, so each reports skipped on
    # a machine without it, a clean chroot or a CI container among them.
    add_executable(stud_gpu_enum_test gpu_enum_test.cpp)
    target_link_libraries(stud_gpu_enum_test PRIVATE stud_ui_gpu_enum)
    add_test(NAME gpu_enum_test COMMAND stud_gpu_enum_test)

    add_executable(stud_credential_store_test credential_store_test.cpp)
    target_link_libraries(stud_credential_store_test PRIVATE stud_ui_credential_store Qt6::DBus)
    add_test(NAME credential_store_test COMMAND stud_credential_store_test)

    add_executable(stud_notifications_test notifications_test.cpp)
    target_link_libraries(stud_notifications_test PRIVATE stud_ui_notifications Qt6::DBus)
    add_test(NAME notifications_test COMMAND stud_notifications_test)

    set_tests_properties(gpu_enum_test credential_store_test notifications_test
                         PROPERTIES SKIP_RETURN_CODE 77)

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


# The mapped-memory write barrier's one promise: a byte written into the
# mapping is reported by a flush. Runs the real fault handler by providing
# the hook the barrier resolves at runtime, so the mechanism under test is
# the real one. Built as its own target rather than against the runtime,
# which is bionic-only.
add_executable(stud_mapped_write_barrier_test
    mapped_write_barrier_test.cpp
    ../runtime/render-client/src/mapped_write_barrier.cpp
    ../runtime/render-client/src/uffd_scan.cpp)
target_include_directories(stud_mapped_write_barrier_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../runtime/render-client/src)
target_link_libraries(stud_mapped_write_barrier_test PRIVATE ${CMAKE_DL_LIBS})
# The barrier finds its hook with dlsym(RTLD_DEFAULT, ...), so the test's
# own definition has to be in the dynamic symbol table.
set_target_properties(stud_mapped_write_barrier_test PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME mapped_write_barrier_test COMMAND stud_mapped_write_barrier_test)
# The default above is uffd-scan wherever the kernel has it, so the fallback
# needs a run of its own or it is never tested.
add_test(NAME mapped_write_barrier_fault_test COMMAND stud_mapped_write_barrier_test)
set_tests_properties(mapped_write_barrier_fault_test PROPERTIES ENVIRONMENT STUD_VK_NO_UFFD_SCAN=1)
# Under ThreadSanitizer, which is what proves the barrier's own metadata is
# free of races rather than merely passing. Not a ctest target: it needs a
# separate toolchain and the suppression beside it, so it is run by hand.
#
#   clang++ -std=c++20 -fsanitize=thread -g -O1 \
#       -o /tmp/barrier_tsan tests/mapped_write_barrier_test.cpp \
#       runtime/render-client/src/mapped_write_barrier.cpp \
#       runtime/render-client/src/uffd_scan.cpp \
#       -Iruntime/render-client/src -ldl -rdynamic
#   TSAN_OPTIONS="handle_segv=0 suppressions=tests/tsan.supp" \
#       STUD_VK_NO_UFFD_SCAN=1 /tmp/barrier_tsan
#
# handle_segv=0 is required: the barrier's write faults are its mechanism,
# not crashes, so TSan must leave SIGSEGV to the test's own handler.

# Every STUD_* variable the source reads is in docs/environment.md.
#
# The source root is passed in rather than discovered, so the test scans
# the tree it was built from wherever ctest runs it.
add_executable(stud_env_documented_test env_documented_test.cpp)
add_test(NAME env_documented_test
         COMMAND stud_env_documented_test "${CMAKE_SOURCE_DIR}")

# What of the engine's log may reach the session log.
add_executable(stud_log_scrub_test log_scrub_test.cpp)
target_include_directories(stud_log_scrub_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src)
add_test(NAME log_scrub_test COMMAND stud_log_scrub_test)

# Windows-quality materials: the SPIR-V rewrites of the shader-pack builder.
add_executable(stud_shader_pack_test shader_pack_test.cpp)
target_link_libraries(stud_shader_pack_test PRIVATE stud_shader_pack)
add_test(NAME shader_pack_test COMMAND stud_shader_pack_test)

# render-host's GPU ETC2/EAC decoder against detex, the decoder the engine's
# process uses, byte for byte, on every Vulkan device present. detex comes
# out of the bionic runtime sub-build's own checkout, as in
# tools/CMakeLists.txt, so on a fresh configure this test is skipped until
# that sub-build has run once and cmake is re-run.
pkg_check_modules(StudEtcVulkan IMPORTED_TARGET vulkan)
set(StudEtcDetexDir "${CMAKE_BINARY_DIR}/runtime-prefix/src/runtime-build/texdec/detex")
if(StudEtcVulkan_FOUND AND TARGET stud_etc_decode_spv AND EXISTS "${StudEtcDetexDir}/decompress-etc.c")
    add_executable(stud_etc_decode_test etc_decode_test.cpp
        "${StudEtcDetexDir}/bits.c"
        "${StudEtcDetexDir}/clamp.c"
        "${StudEtcDetexDir}/division-tables.c"
        "${StudEtcDetexDir}/decompress-etc.c"
        "${StudEtcDetexDir}/decompress-eac.c")
    target_include_directories(stud_etc_decode_test PRIVATE
        "${StudEtcDetexDir}" "${CMAKE_BINARY_DIR}/render-host/generated")
    target_link_libraries(stud_etc_decode_test PRIVATE PkgConfig::StudEtcVulkan)
    add_dependencies(stud_etc_decode_test stud_etc_decode_spv)
    add_test(NAME etc_decode_test COMMAND stud_etc_decode_test)
    set_tests_properties(etc_decode_test PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The shared-memory ring the engine's Vulkan requests travel through.
add_executable(stud_transport_ring_test transport_ring_test.cpp)
target_link_libraries(stud_transport_ring_test PRIVATE stud_render_host_protocol)
add_test(NAME transport_ring_test COMMAND stud_transport_ring_test)

# Every test here finishes in seconds, so one that doesn't is hung. Several
# exist to prove something does not hang, and ctest's own default would take
# 25 minutes to say so.
get_directory_property(StudAllTests TESTS)
set_tests_properties(${StudAllTests} PROPERTIES TIMEOUT 120)
