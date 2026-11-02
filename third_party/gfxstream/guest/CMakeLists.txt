# gfxstream's guest GLES libraries (libEGL, libGLESv2) for OpenGL mode,
# built for bionic with the NDK. The engine loads them in place of its
# libEGL/libGLESv2 (ui/src/main.cpp); they reach render-host's gfxstream
# (render-host/src/gfxstream_host.h) through Stud's own transports: a ring
# on memory render-host shares, else a Unix socket. Upstream builds the guest
# only with Android.bp/Bazel; this file lists the same sources.
#
# Needs a gfxstream checkout with ../stud.patch applied (STUD_GFXSTREAM_DIR)
# and a Mesa checkout (STUD_MESA_DIR), whose guest ring stream is used as is;
# ../stud.patch names the commits both were taken at. Configure with the NDK:
#   cmake -S third_party/gfxstream/guest -B build-gfxstream-guest -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
#     -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=26 -DCMAKE_BUILD_TYPE=Release \
#     -DSTUD_GFXSTREAM_DIR=<gfxstream> -DSTUD_MESA_DIR=<mesa>
# and point Stud's STUD_GFXSTREAM_GUEST_DIR at that build directory.
cmake_minimum_required(VERSION 3.22)
project(stud_gfxstream_guest C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(STUD_GFXSTREAM_DIR "" CACHE PATH "gfxstream source tree, with ../stud.patch applied")
set(STUD_MESA_DIR "" CACHE PATH "Mesa source tree")
if(NOT STUD_GFXSTREAM_DIR OR NOT STUD_MESA_DIR)
    message(FATAL_ERROR "set STUD_GFXSTREAM_DIR and STUD_MESA_DIR")
endif()
set(GFX "${STUD_GFXSTREAM_DIR}")
set(MESA "${STUD_MESA_DIR}/src/gfxstream")
set(G "${GFX}/guest")

add_compile_options(
    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable
    -Wno-unused-private-field -Wno-sign-compare -Wno-missing-field-initializers
    -Wno-deprecated-declarations -Wno-cast-function-type-mismatch -Wno-gnu-designator
    -Wno-unknown-warning-option -Wno-shadow -Wno-error)
# ANDROID is what Android's own build defines (the NDK defines __ANDROID__
# only); gfxstream gates its gralloc and ANativeWindow helpers on it.
add_compile_definitions(GL_GLEXT_PROTOTYPES EGL_EGLEXT_PROTOTYPES WITH_GLES2 ANDROID=1
                        LOG_TAG="gfxstream")

set(GUEST_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GFX}/third_party/opengl/include"
    "${GFX}/common/logging/include"
    "${GFX}/common/etc/include"
    "${G}/android-emu"
    "${G}/OpenglCodecCommon"
    "${G}/OpenglCodecCommon/include"
    "${G}/renderControl_enc"
    "${G}/GLESv1_enc"
    "${G}/GLESv2_enc"
    "${G}/OpenglSystemCommon"
    "${G}/egl"
    "${MESA}/guest/iostream/include"
    "${MESA}/guest/android/include"
    "${MESA}/guest/platform/include"
    "${MESA}/guest/GoldfishAddressSpace/include"
    "${MESA}/aemu/include"
    "${MESA}/..")

# Shared code: logging, ETC helpers, the emulator base library.
add_library(gfxstream_guest_common STATIC
    "${GFX}/common/logging/logging.cpp"
    "${GFX}/common/etc/etc.cpp"
    "${G}/android-emu/aemu/base/AlignedBuf.cpp"
    "${G}/android-emu/aemu/base/Path.cpp"
    "${G}/android-emu/aemu/base/Pool.cpp"
    "${G}/android-emu/aemu/base/Process.cpp"
    "${G}/android-emu/aemu/base/StringFormat.cpp"
    "${G}/android-emu/aemu/base/Tracing.cpp"
    "${G}/android-emu/aemu/base/synchronization/AndroidMessageChannel.cpp"
    "${G}/android-emu/aemu/base/threads/AndroidFunctorThread.cpp"
    "${G}/android-emu/aemu/base/threads/AndroidThread_pthread.cpp")
target_include_directories(gfxstream_guest_common PUBLIC ${GUEST_INCLUDES})
target_link_libraries(gfxstream_guest_common PUBLIC log)

# Encoders, client state and the host connection.
file(GLOB GUEST_CODEC_COMMON "${G}/OpenglCodecCommon/*.cpp")
add_library(gfxstream_guest_codec STATIC
    ${GUEST_CODEC_COMMON}
    "${G}/renderControl_enc/ExtendedRenderControl.cpp"
    "${G}/renderControl_enc/renderControl_client_context.cpp"
    "${G}/renderControl_enc/renderControl_enc.cpp"
    "${G}/renderControl_enc/renderControl_entry.cpp"
    "${G}/GLESv1_enc/gl_client_context.cpp"
    "${G}/GLESv1_enc/gl_enc.cpp"
    "${G}/GLESv1_enc/GLEncoder.cpp"
    "${G}/GLESv1_enc/GLEncoderUtils.cpp"
    "${G}/GLESv2_enc/gl2_client_context.cpp"
    "${G}/GLESv2_enc/gl2_enc.cpp"
    "${G}/GLESv2_enc/GL2Encoder.cpp"
    "${G}/GLESv2_enc/GL2EncoderUtils.cpp"
    "${G}/GLESv2_enc/GLESv2Validation.cpp"
    "${G}/GLESv2_enc/IOStream2.cpp"
    "${G}/OpenglSystemCommon/FormatConversions.cpp"
    "${G}/OpenglSystemCommon/ThreadInfo.cpp"
    # Stud's own: the transports, the connection that picks one, the process
    # handshake, and the helpers an emulator's platform would provide.
    src/StudStream.cpp
    src/StudAsgStream.cpp
    "${MESA}/guest/GoldfishAddressSpace/AddressSpaceStream.cpp"
    "${MESA}/aemu/ring_buffer.cpp"
    src/HostConnection.cpp
    src/ProcessPipe.cpp
    src/StudPlatform.cpp)
target_include_directories(gfxstream_guest_codec PUBLIC ${GUEST_INCLUDES})
target_link_libraries(gfxstream_guest_codec PUBLIC gfxstream_guest_common)

# One library holds EGL, GLES and the encoders they share. Android splits
# them across shared libraries that resolve each other's context accessors
# at run time; one library resolves them at link time instead. gl2.cpp
# includes GLESv2_enc's gl2_entry.cpp itself.
add_library(GLESv2 SHARED
    "${G}/GLESv2/gl2.cpp"
    "${G}/egl/ClientAPIExts.cpp"
    "${G}/egl/egl.cpp"
    "${G}/egl/eglDisplay.cpp")
target_link_libraries(GLESv2 PRIVATE gfxstream_guest_codec dl)

# EGL dlopens its client APIs by these names and asks each for
# init_emul_gles; both are this one library. GLES1 is loaded only because
# EGL requires it, and never used.
add_custom_command(TARGET GLESv2 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink libGLESv2.so libGLESv2_emulation.so
    COMMAND ${CMAKE_COMMAND} -E create_symlink libGLESv2.so libGLESv1_CM_emulation.so
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# libEGL.so is what the engine names for egl*; it defines nothing and depends
# on libGLESv2.so, where the dynamic linker finds them.
add_library(EGL SHARED src/egl_forward.cpp)
target_link_libraries(EGL PRIVATE GLESv2)
target_link_options(EGL PRIVATE -Wl,--no-as-needed)
