add_library(stud_render_host_protocol STATIC
    src/render_host_protocol.cpp
)
target_include_directories(stud_render_host_protocol PUBLIC include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandEgl REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(Egl REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GlesV2 REQUIRED IMPORTED_TARGET glesv2)

# The upscale shader, compiled to SPIR-V at build time and embedded, so
# render-host carries no runtime file dependency for it. glslc comes with
# the NDK this project already vendors; no new tool.
set(UpscaleShaderSrc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/upscale.comp")
set(UpscaleShaderHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/upscale_spv.h")
set(SharpenShaderSrc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharpen.comp")
set(SharpenShaderHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/sharpen_spv.h")
# The other upscaler, chosen at runtime by STUD_UPSCALER. One pass to
# FSR1's two, and one texture fetch on a flat pixel to EASU's twelve.
set(SgsrShaderSrc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sgsr.comp")
set(SgsrShaderHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/sgsr_spv.h")
find_program(StudGlslc glslc
    HINTS "${STUD_ANDROID_NDK_ROOT}/shader-tools/linux-x86_64"
    NO_CMAKE_FIND_ROOT_PATH)
if(StudGlslc)
    add_custom_command(
        OUTPUT "${UpscaleShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${StudGlslc}" --target-env=vulkan1.1 -O -mfmt=c
                -o "${UpscaleShaderHeader}.inc" "${UpscaleShaderSrc}"
        COMMAND "${CMAKE_COMMAND}" -E copy "${UpscaleShaderHeader}.inc" "${UpscaleShaderHeader}"
        DEPENDS "${UpscaleShaderSrc}"
        COMMENT "Compiling the upscale shader to SPIR-V"
    )
    add_custom_command(
        OUTPUT "${SharpenShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${StudGlslc}" --target-env=vulkan1.1 -O -mfmt=c
                -o "${SharpenShaderHeader}" "${SharpenShaderSrc}"
        DEPENDS "${SharpenShaderSrc}"
        COMMENT "Compiling the sharpening shader to SPIR-V"
    )
    add_custom_command(
        OUTPUT "${SgsrShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${StudGlslc}" --target-env=vulkan1.1 -O -mfmt=c
                -o "${SgsrShaderHeader}" "${SgsrShaderSrc}"
        DEPENDS "${SgsrShaderSrc}"
        COMMENT "Compiling the SGSR upscale shader to SPIR-V"
    )
else()
    # Without glslc the upscaler cannot exist; an empty array makes that a
    # clean runtime degrade (the blit path) rather than a build failure on
    # a machine that only wants the rest of Stud.
    #
    # Said out loud, because silence is how a release shipped with FSR
    # quietly doing nothing: the setting was there, the option was in
    # Settings, and the shader behind it was an empty array. A package
    # build should never take this branch; STUD_REQUIRE_SHADERS makes
    # it fatal, and the release workflow sets it.
    if(STUD_REQUIRE_SHADERS)
        message(FATAL_ERROR
            "glslc not found, so the upscaler would have no shader. It ships with "
            "the Android NDK at <ndk>/shader-tools/linux-x86_64/glslc, or comes "
            "from the glslc/shaderc package. Pass -DSTUD_REQUIRE_SHADERS=OFF to "
            "build without the upscaler on purpose.")
    endif()
    message(WARNING
        "Stud: glslc not found, building WITHOUT the upscale and sharpening "
        "shaders. The FSR setting will exist and do nothing.")
    add_custom_command(
        OUTPUT "${UpscaleShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${UpscaleShaderHeader}"
        COMMENT "glslc not found, building without the upscale shader"
    )
    add_custom_command(
        OUTPUT "${SharpenShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${SharpenShaderHeader}"
        COMMENT "glslc not found, building without the sharpening shader"
    )
    add_custom_command(
        OUTPUT "${SgsrShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${SgsrShaderHeader}"
        COMMENT "glslc not found, building without the SGSR shader"
    )
endif()

add_executable(stud-render-host "${UpscaleShaderHeader}" "${SharpenShaderHeader}" "${SgsrShaderHeader}" src/main.cpp src/audio_output.cpp src/vulkan_host.cpp src/vulkan_chain.cpp src/discord_rpc.cpp src/gamepad.cpp src/flight_recorder.cpp)
target_include_directories(stud-render-host PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(stud-render-host PRIVATE
    stud_render
    stud_config
    stud_android_glue
    stud_render_host_protocol
    PkgConfig::WaylandEgl
    PkgConfig::Egl
    PkgConfig::GlesV2
    ${CMAKE_DL_LIBS}
)
if(STUD_ENABLE_DEV_RENDER_TOGGLE)
    target_compile_definitions(stud-render-host PRIVATE STUD_ENABLE_DEV_RENDER_TOGGLE)
endif()

# The window's own app id, for the KWin fallback that raises Stud when a
# game link is clicked. android-glue already compiles this in for
# xdg_toplevel_set_app_id; matching on it means the two cannot disagree
# about which window is Stud's.
target_compile_definitions(stud-render-host PRIVATE STUD_APP_ID="${STUD_APP_ID}")

# MVP proof-of-mechanism client, independent of Process B/bionic,
# proves the IPC protocol itself renders a real frame before wiring the
# bionic-side stub library that will be Process B's real client.
add_executable(stud_render_host_test_client src/test_client.cpp)
target_link_libraries(stud_render_host_test_client PRIVATE stud_render_host_protocol PkgConfig::Egl PkgConfig::GlesV2)

install(TARGETS stud-render-host RUNTIME DESTINATION "${STUD_LIBEXEC_DIR}")

# Exports this binary's own symbols to the dynamic table, which is what
# lets the crash reporter print function names instead of bare addresses
# see install_crash_reporter in session_log.h.
set_target_properties(stud-render-host PROPERTIES ENABLE_EXPORTS ON)
