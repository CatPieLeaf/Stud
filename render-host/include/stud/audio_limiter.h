#pragma once

// Keeps the engine's mix at or under full scale before it leaves Stud.
//
// The engine's own mix goes past full scale on loud passages, and nothing
// on the way out stops it: the sound server adds Stud's samples to every
// other application's and only clamps the sum, at the speakers. So a hot
// Roblox mix clipped everything else playing at the same moment with it.
// Clamping here instead flattens the peaks into the "deepfried" sound that
// float output was chosen to avoid. This turns them down instead.
//
// Instant attack: a frame that would go past full scale gets exactly the
// gain that puts it at full scale, so nothing is ever over. The gain then
// recovers smoothly. While nothing is over, the gain is exactly 1 and the
// samples pass through unchanged.

#include <cmath>
#include <cstddef>

namespace stud::render_host {

struct AudioLimiter {
    float gain = 1.0f;
    // The fraction of the way back to unity gain made each frame.
    float release = 1.0f;

    // Recovers by about two thirds of the way back every `frames` frames.
    void set_release_frames(double frames) {
        release = frames > 0.0 ? static_cast<float>(1.0 - std::exp(-1.0 / frames)) : 1.0f;
    }

    void process(float* samples, size_t frames, int channels) {
        for (size_t f = 0; f < frames; ++f) {
            float* frame = samples + f * static_cast<size_t>(channels);
            if (gain < 1.0f) {
                const float next = gain + (1.0f - gain) * release;
                // Rounding stops it just short of 1 forever; that last step
                // is taken outright so the gain really does reach 1.
                gain = next == gain ? 1.0f : next;
            }
            float peak = 0.0f;
            for (int c = 0; c < channels; ++c) peak = std::fmax(peak, std::fabs(frame[c]));
            if (peak * gain > 1.0f) gain = 1.0f / peak;
            if (gain < 1.0f) {
                for (int c = 0; c < channels; ++c) frame[c] *= gain;
            }
        }
    }
};

}  // namespace stud::render_host
