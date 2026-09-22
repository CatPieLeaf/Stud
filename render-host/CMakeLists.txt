add_library(stud_render_host_protocol STATIC
    src/render_host_protocol.cpp
)
target_include_directories(stud_render_host_protocol PUBLIC include)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

# Tracy's client, fetched only when the profiler is switched on, so an
# ordinary build neither downloads nor compiles it. Pinned to a release
# rather than a branch: a profiler that changes under the build would
# make two measurements incomparable, which defeats the point of taking
# them.
if(STUD_TRACY)
    include(FetchContent)
    FetchContent_Declare(
        tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.14.1
        GIT_SHALLOW    TRUE
    )
    # Tracy's own option, which defaults OFF and would otherwise build
    # the client as a stub that collects nothing -- switched on here so
    # -DSTUD_TRACY=ON means what it says.
    set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
    # Collect only while a profiler is actually connected. Without this
    # the client records from the moment Stud starts and holds it, which
    # is wrong for a session someone leaves running and then attaches to
    # after the stall they are chasing.
    set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(tracy)
endif()
pkg_check_modules(WaylandEgl REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(Egl REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GlesV2 REQUIRED IMPORTED_TARGET glesv2)

# The upscale shader, compiled to SPIR-V at build time and embedded, so
# render-host carries no runtime file dependency for it. glslc comes with
# the NDK this project already vendors; no new tool.
set(SharpenShaderSrc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharpen.comp")
set(SharpenShaderHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/sharpen_spv.h")
# SGSR, the fallback upscaler, chosen at runtime by STUD_UPSCALER. Only
# its edge-direction variant is built; the base one was dropped after the
# comparison, and both come from this one source.
set(SgsrShaderSrc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sgsr.comp")
set(SgsrEdShaderHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/sgsr_ed_spv.h")
# RAVU-Zoom, which unlike SGSR carries trained weight tables as well as a
# shader. They are generated from the vendored hooks rather than checked
# in twice, so the two cannot drift. Only the anti-ringing shader is
# built; the plain hook stays because the weight table comes from it and
# is byte-identical to the anti-ringing hook's own copy.
set(RavuHook "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mpv-prescalers/ravu-zoom-r2-rgb.hook")
set(RavuLutHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/ravu_lut.h")
# The anti-ringing variant: the same filter weights, plus a second table
# used to build the soft minimum and maximum its output is clamped into.
# The weight table is byte-identical to the plain hook's, so only the
# second one is generated here and ravu_lut.h serves both shaders.
set(RavuArShaderSrc "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ravu_ar.comp")
set(RavuArShaderHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/ravu_ar_spv.h")
set(RavuArHook "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/mpv-prescalers/ravu-zoom-ar-r2-rgb.hook")
set(RavuLutArHeader "${CMAKE_CURRENT_BINARY_DIR}/generated/ravu_lut_ar.h")
# Optional, like glslc: a machine without a Python interpreter builds
# everything else and simply has no RAVU, rather than failing.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND)
    message(WARNING
        "Stud: no Python 3 interpreter, building WITHOUT the RAVU-Zoom weight "
        "tables. STUD_UPSCALER will fall back to SGSR.")
    add_custom_command(
        OUTPUT "${RavuLutHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${RavuLutHeader}"
        COMMENT "No Python 3, building without the RAVU-Zoom weight table"
    )
    add_custom_command(
        OUTPUT "${RavuLutArHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${RavuLutArHeader}"
        COMMENT "No Python 3, building without the RAVU-Zoom anti-ringing table"
    )
else()
add_custom_command(
    OUTPUT "${RavuLutHeader}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_ravu_lut.py"
            "${RavuHook}" "${RavuLutHeader}"
    DEPENDS "${RavuHook}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_ravu_lut.py"
    COMMENT "Generating the RAVU-Zoom weight table"
)
add_custom_command(
    OUTPUT "${RavuLutArHeader}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_ravu_lut.py"
            "${RavuArHook}" "${RavuLutArHeader}" 1
    DEPENDS "${RavuArHook}" "${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_ravu_lut.py"
    COMMENT "Generating the RAVU-Zoom anti-ringing table"
)
endif()
find_program(StudGlslc glslc
    HINTS "${STUD_ANDROID_NDK_ROOT}/shader-tools/linux-x86_64"
    NO_CMAKE_FIND_ROOT_PATH)
if(StudGlslc)
    add_custom_command(
        OUTPUT "${SharpenShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${StudGlslc}" --target-env=vulkan1.1 -O -mfmt=c
                -o "${SharpenShaderHeader}" "${SharpenShaderSrc}"
        DEPENDS "${SharpenShaderSrc}"
        COMMENT "Compiling the sharpening shader to SPIR-V"
    )
    add_custom_command(
        OUTPUT "${SgsrEdShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${StudGlslc}" --target-env=vulkan1.1 -O -mfmt=c
                -DSTUD_SGSR_EDGE_DIRECTION
                -o "${SgsrEdShaderHeader}" "${SgsrShaderSrc}"
        DEPENDS "${SgsrShaderSrc}"
        COMMENT "Compiling the SGSR edge-direction shader to SPIR-V"
    )
    add_custom_command(
        OUTPUT "${RavuArShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${StudGlslc}" --target-env=vulkan1.1 -O -mfmt=c
                -o "${RavuArShaderHeader}" "${RavuArShaderSrc}"
        DEPENDS "${RavuArShaderSrc}"
        COMMENT "Compiling the RAVU-Zoom anti-ringing shader to SPIR-V"
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
        OUTPUT "${SharpenShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${SharpenShaderHeader}"
        COMMENT "glslc not found, building without the sharpening shader"
    )
    add_custom_command(
        OUTPUT "${SgsrEdShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${SgsrEdShaderHeader}"
        COMMENT "glslc not found, building without the SGSR edge-direction shader"
    )
    add_custom_command(
        OUTPUT "${RavuArShaderHeader}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND "${CMAKE_COMMAND}" -E echo "{}" > "${RavuArShaderHeader}"
        COMMENT "glslc not found, building without the RAVU-Zoom anti-ringing shader"
    )
endif()

add_executable(stud-render-host "${SharpenShaderHeader}" "${SgsrEdShaderHeader}" "${RavuArShaderHeader}" "${RavuLutHeader}" "${RavuLutArHeader}" src/main.cpp src/audio_output.cpp src/vulkan_host.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/volk/volk.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio/miniaudio.c" src/vulkan_chain.cpp src/discord_rpc.cpp src/gamepad.cpp src/flight_recorder.cpp)
target_include_directories(stud-render-host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/volk" "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/miniaudio" "${CMAKE_CURRENT_BINARY_DIR}/generated")

# What miniaudio is allowed to be. These are set on the target rather than
# in a source file because they change its declarations as well as its
# definitions: defining them for miniaudio.c and not for audio_output.cpp
# would give the two translation units different struct layouts, which
# links cleanly and then misbehaves.
#
# Kept: the four backends this actually targets. Dropped: every backend
# for another operating system, and the null backend, which accepts any
# device and plays nothing (audio that pretends to work hides the fact
# that it did not open). Dropped too: decoding, encoding, the resource
# manager, the node graph and the high-level engine, none of which Stud
# uses -- it hands miniaudio interleaved float32 that the game engine has
# already mixed.
target_compile_definitions(stud-render-host PRIVATE
    MA_NO_NULL
    MA_NO_WASAPI MA_NO_DSOUND MA_NO_WINMM MA_NO_COREAUDIO
    MA_NO_SNDIO MA_NO_AUDIO4 MA_NO_AAUDIO MA_NO_OPENSL MA_NO_WEBAUDIO
    MA_NO_CUSTOM
    MA_NO_DECODING MA_NO_ENCODING MA_NO_WAV MA_NO_FLAC MA_NO_MP3
    MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE MA_NO_GENERATION
)
target_link_libraries(stud-render-host PRIVATE
    stud_render
    stud_config
    stud_android_glue
    stud_render_host_protocol
    PkgConfig::WaylandEgl
    PkgConfig::Egl
    PkgConfig::GlesV2
    # miniaudio's own requirements: it spawns the realtime audio thread
    # itself and uses libm. It opens libasound/libpulse/libjack by name at
    # runtime, so none of those are linked.
    Threads::Threads
    m
    ${CMAKE_DL_LIBS}
)
if(STUD_TRACY)
    target_link_libraries(stud-render-host PRIVATE TracyClient)
    # STUD_WITH_TRACY is what the markers in stud/profiling.h check;
    # TRACY_ENABLE is what Tracy's own headers check. Both, or the
    # markers compile away while the client is still linked in.
    target_compile_definitions(stud-render-host PRIVATE STUD_WITH_TRACY)
    message(STATUS "Stud: the render host is built with the Tracy profiler client")
endif()
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
