#include "fmod_audio_output.h"

#include "render_client_common.h"
#include "stud/app_java_classes.h"

#include <cstdio>
#include <mutex>
#include <vector>

namespace stud::runtime {

namespace {

using stud::render_host::CallId;

// The host plays everything through one float32 device and accepts
// streams only in its own rate and channel count; libaaudio asks for the
// same (runtime/render-client/src/aaudio_client.cpp).
constexpr int kDeviceRate = 48000;
constexpr int kDeviceChannels = 2;

// FMOD's 16-bit PCM, in whatever layout it opened with, into the
// device's: mono duplicated to both sides, more than two channels down to
// the front pair, and the rate converted by linear interpolation, which
// is enough for a fallback that exists so that there is sound at all.
class Output {
public:
    bool open(int channels, int rate) {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
        if (channels <= 0 || rate <= 0) return false;
        uint64_t args[8] = {static_cast<uint64_t>(kDeviceRate),
                            static_cast<uint64_t>(kDeviceChannels),
                            static_cast<uint64_t>(kDeviceChannels * sizeof(float))};
        handle_ = stud::render_client::audio_connection().call(CallId::AudioOpenStream, args,
                                                               nullptr, 0, nullptr, 0, nullptr);
        if (handle_ == 0) return false;
        channels_ = channels;
        step_ = static_cast<double>(rate) / kDeviceRate;
        position_ = 0.0;
        previous_[0] = previous_[1] = 0.0f;
        return true;
    }

    void write(const int16_t* samples, size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (handle_ == 0 || channels_ <= 0) return;
        const size_t frames = count / static_cast<size_t>(channels_);
        if (frames == 0) return;
        out_.clear();
        // position_ is where the next output frame falls, in input frames,
        // counted from one frame BEFORE this buffer (the last frame of the
        // previous one, kept in previous_), so interpolation runs across
        // the boundary without a click.
        auto frame_at = [&](size_t i, int side) -> float {
            if (i == 0) return previous_[side];
            const int16_t* f = samples + (i - 1) * static_cast<size_t>(channels_);
            const int16_t v = channels_ == 1 ? f[0] : f[side];
            return static_cast<float>(v) / 32768.0f;
        };
        while (position_ < static_cast<double>(frames)) {
            const size_t i = static_cast<size_t>(position_);
            const float t = static_cast<float>(position_ - static_cast<double>(i));
            for (int side = 0; side < kDeviceChannels; ++side) {
                const float a = frame_at(i, side);
                const float b = frame_at(i + 1, side);
                out_.push_back(a + (b - a) * t);
            }
            position_ += step_;
        }
        position_ -= static_cast<double>(frames);
        previous_[0] = frame_at(frames, 0);
        previous_[1] = frame_at(frames, 1);
        if (out_.empty()) return;
        // Blocks until the device has taken it, as AudioTrack.write does;
        // that is what paces FMOD's feeder thread.
        uint64_t args[8] = {handle_};
        stud::render_client::audio_connection().call(
            CallId::AudioWriteFrames, args, out_.data(),
            static_cast<uint32_t>(out_.size() * sizeof(float)), nullptr, 0, nullptr);
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        close_locked();
    }

private:
    void close_locked() {
        if (handle_ == 0) return;
        uint64_t args[8] = {handle_};
        stud::render_client::audio_connection().call(CallId::AudioCloseStream, args, nullptr, 0,
                                                     nullptr, 0, nullptr);
        handle_ = 0;
    }

    std::mutex mutex_;
    uint64_t handle_ = 0;
    int channels_ = 0;
    double step_ = 1.0;
    double position_ = 0.0;
    float previous_[2] = {0.0f, 0.0f};
    std::vector<float> out_;
};

Output& output() {
    static Output o;
    return o;
}

}  // namespace

void install_fmod_audio_output() {
    auto& hooks = stud::jni_bridge::AudioDeviceJava::output;
    hooks.open = [](int channels, int rate) { return output().open(channels, rate); };
    hooks.write = [](const int16_t* samples, size_t count) { output().write(samples, count); };
    hooks.close = [] { output().close(); };
}

}  // namespace stud::runtime
