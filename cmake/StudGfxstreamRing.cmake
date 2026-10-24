# gfxstream's single-producer, single-consumer ring (Apache-2.0): the one
# the Android emulator's guest and host share memory through, fetched at
# configure time with a pinned hash, same "vendor it, do not hand-roll it"
# reasoning as the decoders elsewhere in this tree. It carries the request
# direction of the engine's Vulkan connection; see transport_ring.h.
#
# Included by render-host/CMakeLists.txt and runtime/CMakeLists.txt, whose
# protocol libraries both build it. Sets StudRingSources and StudRingIncludes.
set(StudRingCommit "0884d2d224bfe2e46cbb13b71d68d7c66e2f7dc9")
set(StudRingDir "${CMAKE_CURRENT_BINARY_DIR}/gfxstream-ring")
foreach(pair
    "ring_buffer.c;1351c8d411d343566cca196a52f46f83cf5076f374eb33b122a30455b20c83fb"
    "ring_buffer.h;d2d9b27eb90cd20fd0df496846e306915220d93cff79317116c54f390677ac2a")
    list(GET pair 0 StudRingFile)
    list(GET pair 1 StudRingHash)
    file(DOWNLOAD
        "https://raw.githubusercontent.com/google/gfxstream/${StudRingCommit}/guest/android-emu/aemu/base/${StudRingFile}"
        "${StudRingDir}/aemu/base/${StudRingFile}"
        EXPECTED_HASH "SHA256=${StudRingHash}"
        TLS_VERIFY ON)
endforeach()
set(StudRingSources "${StudRingDir}/aemu/base/ring_buffer.c")
# The second directory stands in for the one emulator header ring_buffer.h
# includes, which only declares C linkage.
set(StudRingIncludes "${StudRingDir}" "${CMAKE_CURRENT_LIST_DIR}/gfxstream-shim")
