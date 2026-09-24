#pragma once

// FMOD's Java AudioTrack fallback (org.fmod.AudioDevice), played through
// the render host's audio output. See AudioDeviceStub.
namespace stud::runtime {

void install_fmod_audio_output();

}  // namespace stud::runtime
