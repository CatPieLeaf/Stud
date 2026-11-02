# gfxstream (Apache-2.0), the Android emulator's GPU layer, for OpenGL mode:
# render-host runs its host side (EGL-on-EGL over the system driver), and the
# engine loads its guest libEGL/libGLESv2 instead of the system's. Fetched at
# configure time at a pinned commit and hash, patched with Stud's changes
# (third_party/gfxstream/stud.patch), and built as two sub-builds: the host
# with gfxstream's own meson build, the guest with the NDK
# (third_party/gfxstream/guest). The guest also takes Mesa's guest ring
# stream, the same file Mesa ships, file by file with their hashes.
#
# Included by the top-level CMakeLists.txt before render-host. Sets:
#   StudGfxstreamSource       the patched gfxstream tree
#   StudGfxstreamHostBuild    its meson build directory
#   StudGfxstreamGuestBuild   the guest libraries' build directory
# and the targets gfxstream_host, gfxstream_guest (the guest only where the
# NDK is).
set(StudGfxstreamCommit "0ab696da6e393009c2ebeeac8f33d1c9531d940f")
set(StudGfxstreamArchiveHash "bffebd370521f1db7231a926ee6280acb62cbb186532d27eff6f70d7617195ce")
set(StudMesaCommit "daa925c3bc78697cc214317ee330a39d1acda19f")

set(StudGfxstreamRoot "${CMAKE_BINARY_DIR}/gfxstream")
set(StudGfxstreamSource "${StudGfxstreamRoot}/src")
set(StudGfxstreamHostBuild "${StudGfxstreamRoot}/host-build")
set(StudGfxstreamGuestBuild "${StudGfxstreamRoot}/guest-build")
set(StudMesaSource "${StudGfxstreamRoot}/mesa")
set(StudGfxstreamPatch "${CMAKE_SOURCE_DIR}/third_party/gfxstream/stud.patch")

# The patched tree, made once per (commit, patch): a changed patch makes a
# fresh tree rather than patching a patched one.
file(SHA256 "${StudGfxstreamPatch}" StudGfxstreamPatchHash)
set(StudGfxstreamStamp "${StudGfxstreamRoot}/prepared-${StudGfxstreamPatchHash}")
if(NOT EXISTS "${StudGfxstreamStamp}")
    find_package(Git REQUIRED)
    set(archive "${StudGfxstreamRoot}/gfxstream-${StudGfxstreamCommit}.tar.gz")
    file(DOWNLOAD
        "https://github.com/google/gfxstream/archive/${StudGfxstreamCommit}.tar.gz"
        "${archive}"
        EXPECTED_HASH "SHA256=${StudGfxstreamArchiveHash}"
        TLS_VERIFY ON)
    file(REMOVE_RECURSE "${StudGfxstreamSource}" "${StudGfxstreamRoot}/extract"
         "${StudGfxstreamHostBuild}" "${StudGfxstreamRoot}/host-stamp")
    file(GLOB old_stamps "${StudGfxstreamRoot}/prepared-*")
    if(old_stamps)
        file(REMOVE ${old_stamps})
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${StudGfxstreamRoot}/extract")
    file(RENAME "${StudGfxstreamRoot}/extract/gfxstream-${StudGfxstreamCommit}"
         "${StudGfxstreamSource}")
    file(REMOVE_RECURSE "${StudGfxstreamRoot}/extract")
    # The tree sits inside Stud's own checkout when the build directory
    # does, and there git apply would take the patch's paths as relative to
    # that checkout and skip every one of them without a word. Discovery
    # stops above the tree, so it applies as it does outside any repository.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${StudGfxstreamRoot}"
                "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${StudGfxstreamPatch}"
        WORKING_DIRECTORY "${StudGfxstreamSource}"
        RESULT_VARIABLE patched)
    file(STRINGS "${StudGfxstreamSource}/host/render_api.cpp" applied REGEX "gfxstream_asg_create")
    if(NOT patched EQUAL 0 OR NOT applied)
        message(FATAL_ERROR "third_party/gfxstream/stud.patch does not apply to gfxstream "
                            "${StudGfxstreamCommit}")
    endif()
    file(WRITE "${StudGfxstreamStamp}" "")
endif()

foreach(pair
    "src/gfxstream/aemu/include/ring_buffer.h;5b85d94d2fcf8a48a9327452e96c3d0a190c7a1d231f62d0b7582439c0403e6d"
    "src/gfxstream/aemu/ring_buffer.cpp;5d6241fbc29ca16a33ca4e54bfb8eaa540b9b9ae303a3618206f0f32ea5a5ec4"
    "src/gfxstream/guest/android/include/gfxstream/guest/ANativeWindow.h;29472622a1a262c4f7cb585ad21742ee81069ebc0a7e579a296dd75a4910fc24"
    "src/gfxstream/guest/android/include/gfxstream/guest/GfxStreamGralloc.h;198f0f73907406d331c19d9244e06142dd1401bd0d767f299f677fe801542199"
    "src/gfxstream/guest/android/include/gfxstream/guest/goldfish_sync.h;2706520bae780928f262a8e55688af63572616d130648a53725096d93f0d46f2"
    "src/gfxstream/guest/GoldfishAddressSpace/AddressSpaceStream.cpp;f20732e799de2fa82a111d9d5e67e0db3d0efc6f3c722c2e8e48ea52b859326a"
    "src/gfxstream/guest/GoldfishAddressSpace/include/address_space_graphics_types.h;baccc955a2427cde961fc2a5c7fd814c2fae12e582a1fdfded38213aadf1d3f1"
    "src/gfxstream/guest/GoldfishAddressSpace/include/address_space.h;4676efe94022beceaf9019721d6c4e17872ed288e2a63ab9ca7d8b9abb4e2c81"
    "src/gfxstream/guest/GoldfishAddressSpace/include/AddressSpaceStream.h;58ef93d26863872ac425d8f98fd1f5d7e27a6a32d3e30c16f5bade4678508887"
    "src/gfxstream/guest/iostream/include/gfxstream/guest/IOStream.h;6479d23f9773f1781897e6e4f22b24ddd884ef5d040754b810faa88d2cd75bc8"
    "src/gfxstream/guest/platform/include/Sync.h;0eb6dd2ac0e9c288f2014735c8000290bce5fe9b795b96823aaf79cb78afd55d"
    "src/gfxstream/guest/platform/include/VirtGpu.h;9d7460ae7c76ffd0732b94038fde0341965382dc9448a45aa0be3972c83b6cea"
    "src/virtio/virtio-gpu/virgl_hw.h;576a3c6b1343a82f977b4a00edcbc13ae2821b898a39fe37a9a3a59431e4503d"
    "src/virtio/virtio-gpu/virtgpu_gfxstream_protocol.h;f7fae835eb5816dbb6dbd288330ec253f3d56a458fb905ac851abce1f7442416")
    list(GET pair 0 file)
    list(GET pair 1 hash)
    file(DOWNLOAD
        "https://gitlab.freedesktop.org/mesa/mesa/-/raw/${StudMesaCommit}/${file}"
        "${StudMesaSource}/${file}"
        EXPECTED_HASH "SHA256=${hash}"
        TLS_VERIFY ON)
endforeach()
file(DOWNLOAD "https://gitlab.freedesktop.org/mesa/mesa/-/raw/${StudMesaCommit}/docs/license.rst"
     "${StudGfxstreamRoot}/LICENSE-mesa.rst"
     EXPECTED_HASH "SHA256=0d1a0472ecc81830e75c20d59b0ea02841e3db21255e0ebad97ab682c54d6615"
     TLS_VERIFY ON)

include(ExternalProject)

# The host: libgfxstream_backend.so, which render-host links.
find_program(STUD_MESON meson REQUIRED)
ExternalProject_Add(gfxstream_host
    SOURCE_DIR "${StudGfxstreamSource}"
    BINARY_DIR "${StudGfxstreamHostBuild}"
    STAMP_DIR "${StudGfxstreamRoot}/host-stamp"
    CONFIGURE_COMMAND "${STUD_MESON}" setup "${StudGfxstreamHostBuild}" "${StudGfxstreamSource}"
                      -Dlog-level=error -Dbuildtype=release -Db_ndebug=true
    BUILD_COMMAND "${STUD_MESON}" compile -C "${StudGfxstreamHostBuild}"
    BUILD_ALWAYS ON
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${StudGfxstreamHostBuild}/host/libgfxstream_backend.so")
# Under the name render-host asks for (its soname): the build's is a link to
# a file named after gfxstream's own version, which a link would not carry.
# file(INSTALL) puts DESTDIR in front of the destination itself.
install(CODE "
    file(REAL_PATH \"${StudGfxstreamHostBuild}/host/libgfxstream_backend.so.0\" library)
    file(INSTALL \"\${library}\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${STUD_PRIVATE_LIB_DIR}\"
         RENAME libgfxstream_backend.so.0)")
install(FILES "${StudGfxstreamRoot}/LICENSE-mesa.rst"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/stud/mesa")

# The guest: libEGL.so and libGLESv2.so for the engine, built for bionic.
if(EXISTS "${STUD_ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
    ExternalProject_Add(gfxstream_guest
        SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/gfxstream/guest"
        BINARY_DIR "${StudGfxstreamGuestBuild}"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${STUD_ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake"
            "-DANDROID_ABI=x86_64"
            "-DANDROID_PLATFORM=26"
            "-DCMAKE_BUILD_TYPE=Release"
            "-DSTUD_GFXSTREAM_DIR=${StudGfxstreamSource}"
            "-DSTUD_MESA_DIR=${StudMesaSource}"
        BUILD_ALWAYS ON
        INSTALL_COMMAND "")
    install(FILES "${StudGfxstreamGuestBuild}/libEGL.so" "${StudGfxstreamGuestBuild}/libGLESv2.so"
            DESTINATION "${STUD_PRIVATE_LIB_DIR}/gfxstream-guest")
endif()
