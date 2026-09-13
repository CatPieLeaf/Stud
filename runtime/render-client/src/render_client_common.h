#pragma once

#include "stud/render_host_protocol.h"

// Shared connection singleton for both libEGL.so/libGLESv2.so stub
// bodies -- lazily connects to stud-render-host (Process C) on first
// real EGL/GLES call Roblox makes, once, for the process's whole life.
namespace stud::render_client {

stud::render_host::Client& connection();

// A second, independent connection to the same render host, for audio.
//
// Audio has a hard deadline that GL does not: a burst that arrives late is
// a gap in the waveform, and the engine issues hundreds of GL calls per
// frame through the one connection, every one of which holds its mutex for
// a round trip. Sharing meant the feeder was regularly starved -- measured
// at 16312 host-side underruns in a single session, which is what the
// "bad audio quality" actually was. The host already serves more than one
// client connection.
stud::render_host::Client& audio_connection();

}  // namespace stud::render_client
