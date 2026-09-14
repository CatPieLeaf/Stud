cmake_minimum_required(VERSION 3.20)

# Standalone bionic-toolchain project -- built as a nested configure/build
# via ExternalProject_Add from the top-level (host/glibc) CMakeLists.txt.
# Real bionic ELF executable + real bionic .so's, same reasoning as the
# pre-reset process-b (the engineering notes' "nuke stud, recreate sober 1:1"
# entry): CMake can't mix two compilers/ABIs in one configure, and
# libroblox.so's own real bionic-ABI JNI calls need same-ABI, same-thread
# calls with zero %fs-swapping trampoline.
project(StudRuntime LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT ANDROID)
    message(FATAL_ERROR
        "stud/runtime must be configured with the NDK's android.toolchain.cmake "
        "(see the ExternalProject_Add(runtime ...) call in the top-level "
        "CMakeLists.txt) -- it produces real bionic binaries, not host glibc ones.")
endif()

if(NOT MINIZ_SRC_DIR OR NOT MINIZ_BIN_DIR)
    message(FATAL_ERROR "runtime requires -DMINIZ_SRC_DIR and -DMINIZ_BIN_DIR "
                         "(the host build's already-fetched miniz checkout/generated headers)")
endif()

set(ANDROID_GLUE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../android-glue")

set(RENDER_HOST_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../render-host")
add_library(stud_runtime_render_host_protocol STATIC
    "${RENDER_HOST_SRC}/src/render_host_protocol.cpp"
)
target_include_directories(stud_runtime_render_host_protocol PUBLIC "${RENDER_HOST_SRC}/include")

add_compile_options(-static-libstdc++)
add_link_options(-static-libstdc++)

add_library(stud_runtime_libandroid SHARED
    "${ANDROID_GLUE_SRC}/src/asset_manager.cpp"
    "${ANDROID_GLUE_SRC}/src/configuration.cpp"
    "${ANDROID_GLUE_SRC}/src/looper.cpp"
    "${ANDROID_GLUE_SRC}/src/media_codec.cpp"
    "${ANDROID_GLUE_SRC}/src/apk_extract.cpp"
    "${ANDROID_GLUE_SRC}/src/apk_manifest.cpp"
    render-client/src/native_window_forward.cpp
    render-client/src/render_client_common.cpp
    "${MINIZ_SRC_DIR}/miniz.c"
    "${MINIZ_SRC_DIR}/miniz_tdef.c"
    "${MINIZ_SRC_DIR}/miniz_tinfl.c"
    "${MINIZ_SRC_DIR}/miniz_zip.c"
)
target_include_directories(stud_runtime_libandroid PUBLIC "${ANDROID_GLUE_SRC}/include" "${MINIZ_SRC_DIR}" "${MINIZ_BIN_DIR}" render-client/src)
target_link_libraries(stud_runtime_libandroid PRIVATE stud_runtime_render_host_protocol)
set_target_properties(stud_runtime_libandroid PROPERTIES
    OUTPUT_NAME "android"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
)

    # Real, live-caught bug (the engineering notes, "GameActivity_
    # initializeNativeCode returns NULL" / "ALooper_forThread() genuinely
    # returns NULL inside libroblox.so's own call, despite Stud's own
    # ALooper_prepare(0) succeeding moments earlier on the same thread"
    # entry): this target used to also compile looper.cpp/asset_manager.cpp/
    # configuration.cpp/apk_extract.cpp -- duplicating them here as well as
    # in stud_runtime_libandroid meant BOTH .so files exported the exact
    # same real symbol names (ALooper_prepare, ALooper_forThread, ...),
    # each with its own independent thread_local t_looper. libroblox.so's
    # real DT_NEEDED order lists libmediandk.so BEFORE libandroid.so
    # (confirmed via the library's own dependency list) -- the real bionic dynamic linker binds
    # libroblox.so's own ALooper_* imports against THIS library's copy,
    # while Stud's own main.cpp links directly against stud_runtime_
    # libandroid and calls ALooper_prepare() on THAT copy -- two
    # completely separate, never-synchronized loopers. A real device's
    # actual libmediandk.so does not export ALooper_*/AAssetManager_*/
    # AConfiguration_* at all (those are libandroid.so's exclusive real
    # surface) -- trimmed to media_codec.cpp only, its own real, unique
    # purpose, confirmed (via grep) to call none of the removed functions.
add_library(stud_runtime_libmediandk SHARED
    "${ANDROID_GLUE_SRC}/src/media_codec.cpp"
)
target_include_directories(stud_runtime_libmediandk PUBLIC "${ANDROID_GLUE_SRC}/include")
set_target_properties(stud_runtime_libmediandk PROPERTIES
    OUTPUT_NAME "mediandk"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
)

# Real libaaudio.so -- the engine's own audio output path. Not an empty
# stub like the ones below: it forwards PCM to Process C, which owns the
# host audio server connection (see render-client/src/aaudio_stub.cpp for
# why audio is load-bearing rather than optional).
add_library(stud_runtime_libaaudio SHARED
    render-client/src/aaudio_stub.cpp
    render-client/src/render_client_common.cpp
)
target_include_directories(stud_runtime_libaaudio PRIVATE render-client/src)
target_link_libraries(stud_runtime_libaaudio PRIVATE stud_runtime_render_host_protocol)
set_target_properties(stud_runtime_libaaudio PROPERTIES
    OUTPUT_NAME "aaudio"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
)

foreach(stub_name IN ITEMS OpenSLES OpenMAXAL nativewindow)
    add_library(stud_runtime_lib${stub_name} SHARED src/empty_stub_lib.cpp)
    set_target_properties(stud_runtime_lib${stub_name} PROPERTIES
        OUTPUT_NAME "${stub_name}"
        SUFFIX ".so"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
    )
endforeach()

# framework/: real Android JNIEnv/JavaVM implementation (FakeJni/jnivm)
# plus Stud's own real, app- and AOSP-grounded framework substrate (Handler/
# Looper/ClassLoader/Activity/Context, Djinni protocol registrations,
# AGDK GameActivity/game-text-input surfaces) -- real, evidence-based,
# proven-compiling code kept across the reset. What's NOT reused is the
# old scripted, hand-sequenced call-chasing orchestration in main.cpp --
# see src/main.cpp's own doc comment for the real replacement.
include(FetchContent)
FetchContent_Declare(
    libjnivm
    GIT_REPOSITORY https://github.com/ChristopherHX/libjnivm.git
    GIT_TAG        main
    PATCH_COMMAND  ${CMAKE_COMMAND} -Djni_h_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/include/jni.h
                   -Dvm_cpp_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/src/jnivm/vm.cpp
                   -Dvm_h_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/include/jnivm/vm.h
                   -Dmethod_cpp_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/src/jnivm/internal/method.cpp
                   -Dfield_cpp_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/src/jnivm/internal/field.cpp
                   -Dfake_jni_cpp_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/src/fake-jni/fake-jni.cpp
                   -Djnitypes_h_path=${CMAKE_BINARY_DIR}/_deps/libjnivm-src/include/jnivm/jnitypes.h
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/framework/patches/patch_libjnivm.cmake
)
set(JNIVM_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(JNIVM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(JNIVM_ENABLE_TRACE OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(libjnivm)
unset(CMAKE_POLICY_VERSION_MINIMUM)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

set(FRAMEWORK_SRC "${CMAKE_CURRENT_SOURCE_DIR}/framework")
add_library(stud_runtime_framework STATIC
    "${FRAMEWORK_SRC}/src/platform_params.cpp"
    "${FRAMEWORK_SRC}/src/device_params.cpp"
    "${FRAMEWORK_SRC}/src/init_params.cpp"
    "${FRAMEWORK_SRC}/src/start_app_params.cpp"
    "${FRAMEWORK_SRC}/src/start_game_params.cpp"
    "${FRAMEWORK_SRC}/src/flag_overrides.cpp"
    "${FRAMEWORK_SRC}/src/trap_recovery.cpp"
    "${FRAMEWORK_SRC}/src/bootstrap.cpp"
    "${FRAMEWORK_SRC}/src/native_settings.cpp"
    "${FRAMEWORK_SRC}/src/app_bridge.cpp"
    "${FRAMEWORK_SRC}/src/client_settings_bridge.cpp"
    "${FRAMEWORK_SRC}/src/native_flags_bridge.cpp"
    "${FRAMEWORK_SRC}/src/activity_lifecycle_bridge.cpp"
    "${FRAMEWORK_SRC}/src/engine_thread.cpp"
    "${FRAMEWORK_SRC}/src/engine_v2_bridge.cpp"
    "${FRAMEWORK_SRC}/src/haptics_bridge.cpp"
    "${FRAMEWORK_SRC}/src/webview_bridge.cpp"
    "${FRAMEWORK_SRC}/src/permissions_bridge.cpp"
    "${FRAMEWORK_SRC}/src/system_theme_bridge.cpp"
    "${FRAMEWORK_SRC}/src/linking_bridge.cpp"
    "${FRAMEWORK_SRC}/src/webview_cookies.cpp"
    "${FRAMEWORK_SRC}/src/server_address.cpp"
    "${FRAMEWORK_SRC}/src/game_instance.cpp"
    "${FRAMEWORK_SRC}/src/content_sharing_bridge.cpp"
    "${FRAMEWORK_SRC}/src/game_activity_stubs.cpp"
    "${FRAMEWORK_SRC}/src/text_editor.cpp"
    "${FRAMEWORK_SRC}/src/android_framework_stubs.cpp"
    "${FRAMEWORK_SRC}/src/protocol_platform_stubs.cpp"
    "${FRAMEWORK_SRC}/src/game_engine_boot.cpp"
    "${FRAMEWORK_SRC}/src/canonical_vm_registry.cpp"
)
target_include_directories(stud_runtime_framework PUBLIC
    "${FRAMEWORK_SRC}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../bionic-symbol-lookup/include"
    # stud/stud_paths.h -- one answer to "where does this file belong",
    # shared by all three processes.
    "${ANDROID_GLUE_SRC}/include"
)
target_link_libraries(stud_runtime_framework PUBLIC fake-jni nlohmann_json::nlohmann_json)

add_library(stud_runtime_libEGL SHARED
    render-client/src/egl_stub.cpp
    render-client/src/render_client_common.cpp
)
target_include_directories(stud_runtime_libEGL PRIVATE render-client/src)
target_link_libraries(stud_runtime_libEGL PRIVATE stud_runtime_render_host_protocol)
set_target_properties(stud_runtime_libEGL PROPERTIES
    OUTPUT_NAME "EGL"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
)

add_library(stud_runtime_libGLESv2 SHARED
    render-client/src/gles_stub.cpp
    render-client/src/render_client_common.cpp
)
target_include_directories(stud_runtime_libGLESv2 PRIVATE render-client/src)
target_link_libraries(stud_runtime_libGLESv2 PRIVATE stud_runtime_render_host_protocol)
set_target_properties(stud_runtime_libGLESv2 PROPERTIES
    OUTPUT_NAME "GLESv2"
    SUFFIX ".so"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
)

# Real ETC1/ETC2/EAC and PVRTC decoders, fetched at configure time with a
# pinned hash -- same "vendor it, do not hand-roll it" reasoning as miniz
# and libjnivm elsewhere in this tree. A block decoder that is subtly wrong
# produces textures that look plausible and are quietly corrupt, which is
# far worse than not supporting the format at all, so these are vetted
# implementations rather than something written from the spec here.
#   etcdec.h        -- public domain (Unlicense), Sergii "iOrange" Kudlai
#   PVRTDecompress  -- MIT, Imagination Technologies' own PowerVR SDK
set(TexDecodeDir "${CMAKE_CURRENT_BINARY_DIR}/texdec")
file(MAKE_DIRECTORY "${TexDecodeDir}")
file(DOWNLOAD
    "https://raw.githubusercontent.com/iOrange/etcdec/main/etcdec.h"
    "${TexDecodeDir}/etcdec.h"
    EXPECTED_HASH SHA256=57317112d44a1fc89d153fa4141587963a2ceeb22c597f23c594b301e1de6838
    TLS_VERIFY ON
)
file(DOWNLOAD
    "https://raw.githubusercontent.com/powervr-graphics/Native_SDK/master/framework/PVRCore/texture/PVRTDecompress.h"
    "${TexDecodeDir}/PVRTDecompress.h"
    EXPECTED_HASH SHA256=67123a36b99df380af76a65f3dfa3a7f368a153f5401b36a9ff1ed233cee6f5f
    TLS_VERIFY ON
)
file(DOWNLOAD
    "https://raw.githubusercontent.com/powervr-graphics/Native_SDK/master/framework/PVRCore/texture/PVRTDecompress.cpp"
    "${TexDecodeDir}/PVRTDecompress.cpp"
    EXPECTED_HASH SHA256=74559c5a4b8161aafce1ebe984bf8060896feaa81276b614491dbee755d6e4c7
    TLS_VERIFY ON
)

# detex -- MIT, Hans-Kristian "hglm" Lemmens' texture decompression
# library, and the ETC decoder Sober uses for this same job. Measured
# against etcdec.h on this machine, byte-identical output on every format
# Stud emulates (ETC2 RGB8/RGBA8/RGB8A1, EAC R11/RG11) and 2.3x the
# throughput on the two colour formats that dominate a real texture
# burst -- 510 -> 1181 MB/s and 420 -> 979 MB/s. Pinned to one commit
# rather than a branch: a decoder that changes under the build is exactly
# the "quietly corrupt textures" risk this whole block exists to avoid.
set(DetexCommit "cab11584d9be140602a66fd9a88ef0b99f08a97a")
file(MAKE_DIRECTORY "${TexDecodeDir}/detex")
foreach(pair
    "detex.h;4621b899b0efcdfa53b8776110016e6b21e43c15936f3446f46b6889d735a48b"
    "bits.h;8078fde1a3d62e6d43e238083e0f32c17c350de6b65361a6a3a34049015a33ea"
    "bits.c;185185b21c6999acfb72aca5542143de9c177ad409eae01643571cb21b64281d"
    "clamp.c;0bd2f31ad599bb05637d7481985986818fa71448005b730b34fb17767e8a62d4"
    "division-tables.c;e0e52fd4865e09e5f2f43923ea828f30850121c901031c93d78f506712703022"
    "decompress-etc.c;99c7f62ec5ca5b4dab54dbb984a5649f552827160332dbbf5a60ff7259fd7988"
    "decompress-eac.c;3ce53076e7eb85d7eecfa83a5971f19eefe60d64fd76032a1c8e123793aa13b8")
    list(GET pair 0 DetexFile)
    list(GET pair 1 DetexHash)
    file(DOWNLOAD
        "https://raw.githubusercontent.com/hglm/detex/${DetexCommit}/${DetexFile}"
        "${TexDecodeDir}/detex/${DetexFile}"
        EXPECTED_HASH "SHA256=${DetexHash}"
        TLS_VERIFY ON
    )
endforeach()
set(DetexSources
    "${TexDecodeDir}/detex/bits.c"
    "${TexDecodeDir}/detex/clamp.c"
    "${TexDecodeDir}/detex/division-tables.c"
    "${TexDecodeDir}/detex/decompress-etc.c"
    "${TexDecodeDir}/detex/decompress-eac.c"
)

add_library(stud_runtime_libvulkan SHARED
    render-client/src/vulkan_stub.cpp
    render-client/src/texture_decode.cpp
    render-client/src/texture_cache.cpp
    render-client/src/texture_encode.cpp
    render-client/src/mapped_write_barrier.cpp
    render-client/src/render_client_common.cpp
    "${TexDecodeDir}/PVRTDecompress.cpp"
    ${DetexSources}
    "${CMAKE_CURRENT_SOURCE_DIR}/../render-host/src/vulkan_chain.cpp"
)
target_include_directories(stud_runtime_libvulkan PRIVATE render-client/src "${TexDecodeDir}"
    "${TexDecodeDir}/detex" "${CMAKE_CURRENT_SOURCE_DIR}/../android-glue/include")
target_link_libraries(stud_runtime_libvulkan PRIVATE stud_runtime_render_host_protocol)
set_target_properties(stud_runtime_libvulkan PROPERTIES
    OUTPUT_NAME "vulkan"
    SUFFIX ".so.1"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib64"
)

set(STUD_IPC_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../stud-ipc")

# Which library each copy of render_client_common.cpp ends up in.
#
# That file is listed in all five of these targets, so each gets its own
# copy of the connection statics -- up to fifteen sockets to render-host,
# not the three the header describes. STUD_IPC_OWNERS=1 makes each one
# say which library opened it.
target_compile_definitions(stud_runtime_libandroid PRIVATE STUD_CLIENT_OWNER="libandroid.so")
target_compile_definitions(stud_runtime_libaaudio PRIVATE STUD_CLIENT_OWNER="libaaudio.so")
target_compile_definitions(stud_runtime_libEGL PRIVATE STUD_CLIENT_OWNER="libEGL.so")
target_compile_definitions(stud_runtime_libGLESv2 PRIVATE STUD_CLIENT_OWNER="libGLESv2.so")
target_compile_definitions(stud_runtime_libvulkan PRIVATE STUD_CLIENT_OWNER="libvulkan.so.1")

add_library(stud_runtime_ipc STATIC
    "${STUD_IPC_SRC}/src/ipc.cpp"
)
target_include_directories(stud_runtime_ipc PUBLIC "${STUD_IPC_SRC}/include")
target_link_libraries(stud_runtime_ipc PUBLIC nlohmann_json::nlohmann_json)

add_executable(stud-runtime-bionic src/main.cpp src/input_bridge.cpp src/mouse_behavior.cpp
                                    src/pthread_create_interpose.cpp
                                    src/android_log_interpose.cpp src/pthread_tls_interpose.cpp)
# The render client lives in five separate .so files and finds this
# executable's stud_note_shutting_down() by name at runtime; without
# --export-dynamic the symbol is not in the dynamic table and dlsym
# returns null, silently.
target_link_options(stud-runtime-bionic PRIVATE -Wl,--export-dynamic)
target_include_directories(stud-runtime-bionic PRIVATE
    "${ANDROID_GLUE_SRC}/include"
    "${FRAMEWORK_SRC}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../bionic-symbol-lookup/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/render-client/src"
    "${RENDER_HOST_SRC}/include"
)
target_link_libraries(stud-runtime-bionic PRIVATE
    stud_runtime_framework
    stud_runtime_libandroid
    stud_runtime_libEGL
    stud_runtime_libGLESv2
    stud_runtime_ipc
    fake-jni
)
# Export this executable's own symbols into the dynamic table.
#
# libvulkan.so is a separate object that does not link the framework, so
# it resolves stud_set_write_fault_handler (trap_recovery.h) at runtime
# with dlsym(RTLD_DEFAULT, ...) -- the same pattern Stud already uses for
# every other cross-object entry point. Without this the executable
# exported zero dynamic symbols and that lookup simply failed, silently
# leaving the mapped-memory write barrier off.
set_target_properties(stud-runtime-bionic PROPERTIES ENABLE_EXPORTS ON)
add_dependencies(stud-runtime-bionic
    stud_runtime_libmediandk
    stud_runtime_libOpenSLES
    stud_runtime_libOpenMAXAL
    stud_runtime_libnativewindow
    stud_runtime_libvulkan
)
