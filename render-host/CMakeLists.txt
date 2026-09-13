add_library(stud_render_host_protocol STATIC
    src/render_host_protocol.cpp
)
target_include_directories(stud_render_host_protocol PUBLIC include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WaylandEgl REQUIRED IMPORTED_TARGET wayland-egl)
pkg_check_modules(Egl REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GlesV2 REQUIRED IMPORTED_TARGET glesv2)

add_executable(stud-render-host src/main.cpp src/audio_output.cpp src/vulkan_host.cpp src/vulkan_chain.cpp src/discord_rpc.cpp src/gamepad.cpp)
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

# MVP proof-of-mechanism client, independent of Process B/bionic --
# proves the IPC protocol itself renders a real frame before wiring the
# bionic-side stub library that will be Process B's real client.
add_executable(stud_render_host_test_client src/test_client.cpp)
target_link_libraries(stud_render_host_test_client PRIVATE stud_render_host_protocol PkgConfig::Egl PkgConfig::GlesV2)

install(TARGETS stud-render-host RUNTIME DESTINATION "${STUD_LIBEXEC_DIR}")

# Exports this binary's own symbols to the dynamic table, which is what
# lets the crash reporter print function names instead of bare addresses
# -- see install_crash_reporter in session_log.h.
set_target_properties(stud-render-host PROPERTIES ENABLE_EXPORTS ON)
