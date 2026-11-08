// The limiter that keeps the engine's mix from clipping the whole system's.

#include <cmath>
#include <vector>

#include "check.h"
#include "stud/audio_limiter.h"

using stud::render_host::AudioLimiter;

int main() {
    // Under full scale nothing changes, bit for bit.
    {
        AudioLimiter limiter;
        limiter.set_release_frames(3840);
        std::vector<float> s = {0.5f, -0.99f, 1.0f, -1.0f, 0.0f, 0.25f};
        const std::vector<float> before = s;
        limiter.process(s.data(), 3, 2);
        check(s == before && limiter.gain == 1.0f, "audio under full scale passes unchanged");
    }
    // A peak over full scale comes out at full scale, both channels turned
    // down together so the stereo image stays.
    {
        AudioLimiter limiter;
        limiter.set_release_frames(3840);
        std::vector<float> s = {2.0f, 1.0f};
        limiter.process(s.data(), 1, 2);
        check(std::fabs(s[0]) <= 1.0f && std::fabs(s[0] - 1.0f) < 1e-6f, "a 2.0 peak comes out at 1.0");
        check(std::fabs(s[1] - 0.5f) < 1e-6f, "the other channel is turned down by the same gain");
    }
    // A long loud stretch never goes over.
    {
        AudioLimiter limiter;
        limiter.set_release_frames(3840);
        std::vector<float> s(48000 * 2);
        for (size_t i = 0; i < s.size(); ++i) s[i] = 3.0f * std::sin(static_cast<float>(i) * 0.01f);
        limiter.process(s.data(), s.size() / 2, 2);
        float peak = 0.0f;
        for (float v : s) peak = std::fmax(peak, std::fabs(v));
        check(peak <= 1.0f + 1e-6f, "a mix three times too hot never passes full scale");
    }
    // After the loud part the gain comes all the way back, and quiet audio
    // is unchanged again.
    {
        AudioLimiter limiter;
        limiter.set_release_frames(480);
        std::vector<float> loud = {4.0f, 4.0f};
        limiter.process(loud.data(), 1, 2);
        std::vector<float> quiet(48000 * 2, 0.1f);
        limiter.process(quiet.data(), quiet.size() / 2, 2);
        check(limiter.gain == 1.0f, "the gain returns to exactly 1");
        check(quiet.back() == 0.1f, "quiet audio afterwards is untouched");
    }
    return 0;
}
