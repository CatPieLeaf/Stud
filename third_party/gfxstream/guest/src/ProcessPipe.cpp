// Stud's replacement for gfxstream's ProcessPipe.cpp: the process id comes
// from a 'P' connection to Stud's host (see StudStream) instead of the
// emulator's GLProcessPipe service. Same interface, same once-per-process
// rule.
#include "ProcessPipe.h"

#include <mutex>

#include "StudStream.h"

namespace {
std::once_flag sInit;
uint64_t sProcUID = 0;
}  // namespace

bool processPipeInit(int /*streamHandle*/, HostConnectionType /*connType*/,
                     uint32_t noRenderControlEnc) {
    if (noRenderControlEnc) return true;
    std::call_once(sInit, [] {
        StudStream stream(4096);
        sProcUID = stream.processPipeInit();
    });
    return sProcUID != 0;
}

uint64_t getPuid() { return sProcUID; }
