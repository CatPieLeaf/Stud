# Real render smoke test: proves the real ANGLE-to-Vulkan (or dev-only
# Zink) -> real Wayland window pipeline actually renders visible frames,
# standalone from libroblox.so/JNI. Still real, still useful under the
# bionic-native/render-host architecture (see render-host/src/main.cpp)
# exercises the exact same render.cpp/android-glue code render-host
# itself reuses unchanged.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandEgl REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(Egl REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GlesV2 REQUIRED IMPORTED_TARGET glesv2)
add_executable(stud_try_vulkan_window try_vulkan_window.cpp)
target_link_libraries(stud_try_vulkan_window PRIVATE stud_android_glue vulkan)

add_executable(stud_try_render_window try_render_window.cpp)
target_link_libraries(stud_try_render_window PRIVATE stud_render stud_android_glue
                       PkgConfig::WaylandEgl PkgConfig::Egl PkgConfig::GlesV2)
if(STUD_ENABLE_DEV_RENDER_TOGGLE)
    target_compile_definitions(stud_try_render_window PRIVATE STUD_ENABLE_DEV_RENDER_TOGGLE)
endif()

# Real, temporary diagnostic driver; see diag_launch_direct.cpp's own
# doc comment. Not meant to survive long-term; remove once the
# trap_recovery.cpp live-verification it exists for is done.
add_executable(stud_diag_launch_direct diag_launch_direct.cpp)
target_link_libraries(stud_diag_launch_direct PRIVATE stud_bionic_runtime stud_ipc)

# Ground truth for the software texture decoder: decodes the same blocks on
# a GPU that implements ETC2 in hardware and compares. See the file's own
# header for what it settled and how to run it.
#
# It compiles detex/PVRTDecompress straight out of the bionic runtime
# sub-build's own FetchContent checkout, which only exists once that
# sub-build has run at least once, so on a fresh configure this target
# is skipped rather than failing the whole generate step. Re-run cmake
# after a first successful build to get it.
# PkgConfig is already found at the top of this file.
pkg_check_modules(VulkanLoader IMPORTED_TARGET vulkan)
set(STUD_TEXDEC_DIR "${CMAKE_CURRENT_BINARY_DIR}/../runtime-prefix/src/runtime-build/texdec")
if (VulkanLoader_FOUND AND EXISTS "${STUD_TEXDEC_DIR}/PVRTDecompress.cpp")
    add_executable(stud_tex_decode_compare
        tex_decode_compare.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/render-client/src/texture_decode.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/render-client/src/texture_cache.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/render-client/src/texture_encode.cpp"
        "${STUD_TEXDEC_DIR}/PVRTDecompress.cpp"
        "${STUD_TEXDEC_DIR}/detex/bits.c"
        "${STUD_TEXDEC_DIR}/detex/clamp.c"
        "${STUD_TEXDEC_DIR}/detex/division-tables.c"
        "${STUD_TEXDEC_DIR}/detex/decompress-etc.c"
        "${STUD_TEXDEC_DIR}/detex/decompress-eac.c"
    )
    target_include_directories(stud_tex_decode_compare PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/render-client/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/../android-glue/include"
        "${STUD_TEXDEC_DIR}"
        "${STUD_TEXDEC_DIR}/detex"
    )
    target_link_libraries(stud_tex_decode_compare PRIVATE PkgConfig::VulkanLoader)
endif()
