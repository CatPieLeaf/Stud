#pragma once

// Real audio output, hosted in Process C for the same reason the GPU is:
// Process B is bionic and cannot talk to the host's audio server. See
// audio_output.cpp for why audio is load-bearing rather than optional
// (the engine's own FMOD fails System::init without a device, and the
// game-start path runs through it).

#include <cstddef>
#include <cstdint>

namespace stud::render_host {

// Opens the one process-wide output device and starts it playing (silence
// until something is fed to it). Called at startup so Stud appears in the
// desktop's volume mixer from launch, like any other application, rather
// than only while a sound happens to be playing. Safe to call repeatedly.
void audio_start_output_device();

// Tells the host the engine has opened its own audio stream.
// `bytes_per_frame` must be `channels * 4` -- 32-bit float interleaved,
// which is what the engine produces and what a real Android device gives
// an AAudio stream that does not request a format. Converting to 16-bit
// anywhere in this path costs the mix its headroom and clips loud content. Returns a stream handle, or 0 when the host has
// no usable audio output (no PulseAudio/PipeWire, or the server refused),
// which callers must treat as "no audio" rather than an error.
uint64_t audio_open_stream(int sample_rate, int channels, int bytes_per_frame);

// Writes interleaved PCM. Blocks until the device has taken it, which is
// what paces the caller. Returns bytes accepted, or 0 on failure.
uint64_t audio_write_frames(uint64_t stream, const void* data, size_t bytes);

void audio_close_stream(uint64_t stream);

}  // namespace stud::render_host
