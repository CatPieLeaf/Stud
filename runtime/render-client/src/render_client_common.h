#pragma once

#include "stud/render_host_protocol.h"

// Shared connection singleton for both libEGL.so/libGLESv2.so stub
// bodies -- lazily connects to stud-render-host (Process C) on first
// real EGL/GLES call Roblox makes, once, for the process's whole life.
// One real glGetError per frame, called by the swap path. See
// glGetError() in gles_stub.cpp for why it is not polled per call. Lives
// in the GL stub, which is a separate shared library from the EGL one --
// so it is resolved the same way the engine resolves everything else
// here, by name at runtime, rather than linked.
extern "C" void stud_refresh_gl_error_cache();

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

// A third connection, for input, for the same reason audio has one and
// with a sharper deadline still.
//
// Input is the only path a human watches directly: every millisecond
// between the hand moving and the engine hearing about it is a
// millisecond the engine's own painted cursor is visibly behind, and
// there is no hardware cursor here to hide it the way there is on
// Windows. Sharing the render connection meant input could only be
// ASKED for between GL calls, and asking cost a round trip that every
// GL call then queued behind -- so it was polled on a timer instead,
// which is latency by construction.
//
// With its own connection the host can simply hold the request until an
// event actually arrives, and hand it over the instant it does.
stud::render_host::Client& input_connection();

}  // namespace stud::render_client
