// Real libaaudio.so, bionic-compiled, placed where the real bionic linker
// resolves the engine's own dlopen("libaaudio.so"), exactly the same
// shape as this project's libEGL/libGLESv2 stubs, and for the same
// reason: Process B cannot talk to the host's audio server, so the real
// device work happens in Process C and this forwards to it.
//
// Why audio is not optional: the engine's own FMOD initialises an Android
// audio device when a game starts, and with no device at all it fails,
// live-caught in the engine's own log, immediately before a join stalls:
//   Error [FLog::FMOD] FMOD API error, FMOD_RESULT:51, functionname:System::init
//   Error [FLog::Audio] FMOD initialization failed with error code 51!
// (51 is FMOD_ERR_OUTPUT_INIT.)
//
// Only the 25 entry points the engine actually imports are implemented,
// confirmed by name against the real libroblox.so rather than guessed at:
// builder create/delete/open plus its setters, and the stream's own
// start/pause/stop/close/read and getters. AAudio's real model here is
// callback-driven, the app installs a data callback and AAudio pulls
// from it on its own thread, which is what the feeder thread below
// does, with the host's blocking write as the only clock.

#include "render_client_common.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using stud::render_host::CallId;
using stud::render_client::connection;
using stud::render_client::audio_connection;

namespace {

// Real AAudio constants (aaudio/AAudio.h). Only the values that actually
// travel across this boundary are declared.
constexpr int32_t AAUDIO_OK = 0;
constexpr int32_t AAUDIO_ERROR_NULL = -898;
constexpr int32_t AAUDIO_ERROR_INVALID_STATE = -895;
constexpr int32_t AAUDIO_FORMAT_PCM_I16 = 1;
constexpr int32_t AAUDIO_FORMAT_PCM_FLOAT = 2;
constexpr int32_t AAUDIO_DIRECTION_OUTPUT = 0;
constexpr int32_t AAUDIO_STREAM_STATE_OPEN = 1;
constexpr int32_t AAUDIO_STREAM_STATE_STARTED = 4;
constexpr int32_t AAUDIO_STREAM_STATE_PAUSED = 6;
constexpr int32_t AAUDIO_STREAM_STATE_STOPPED = 8;
constexpr int32_t AAUDIO_CALLBACK_RESULT_CONTINUE = 0;

// Stud always runs the device at 48 kHz stereo: it is what every host
// audio server this targets uses natively, so nothing has to resample.
// The engine asks for the stream's real rate/channel count after opening
// and adapts, which is exactly what a real device relies on too.
constexpr int32_t kSampleRate = 48000;
constexpr int32_t kChannels = 2;
// ~10ms per burst. Small enough to keep latency sane, large enough that
// one IPC round trip per burst is not a meaningful cost next to the audio
// device's own buffering.
constexpr int32_t kFramesPerBurst = 480;

using AAudioStreamCallback = int32_t (*)(void* stream, void* userData, void* audioData,
                                          int32_t numFrames);
using AAudioErrorCallback = void (*)(void* stream, void* userData, int32_t error);

struct Builder {
    // Float unless the engine asks otherwise. A real Android device picks
    // the device's own format when none is requested, and on anything
    // modern that is float, and this build's engine never calls
    // setFormat at all (confirmed live: three streams opened, no format
    // request). Defaulting to 16-bit therefore imposed a ceiling nothing
    // asked for, and a hot mix clipped against it: loud, bassy passages
    // came out distorted while everything quieter was fine.
    int32_t format = AAUDIO_FORMAT_PCM_FLOAT;
    int32_t direction = AAUDIO_DIRECTION_OUTPUT;
    int32_t buffer_capacity = kFramesPerBurst * 4;
    AAudioStreamCallback data_callback = nullptr;
    void* data_callback_user = nullptr;
    AAudioErrorCallback error_callback = nullptr;
    void* error_callback_user = nullptr;
};

struct Stream {
    uint64_t host_handle = 0;
    int32_t format = AAUDIO_FORMAT_PCM_I16;
    int32_t buffer_size = kFramesPerBurst * 2;
    int32_t buffer_capacity = kFramesPerBurst * 4;
    AAudioStreamCallback data_callback = nullptr;
    void* data_callback_user = nullptr;
    std::atomic<int32_t> state{AAUDIO_STREAM_STATE_OPEN};
    std::atomic<bool> running{false};
    std::thread feeder;
    // Capture streams have no feeder: the engine reads from them.
    bool is_input = false;
    int32_t channels = kChannels;
};

// The engine converts to whatever the stream reports, so 16-bit is the
// one format that has to be produced here, float is converted on this
// side rather than asking the host to handle two layouts.
void feed(Stream* s) {
    const size_t frames = static_cast<size_t>(kFramesPerBurst);
    std::vector<float> float_scratch(frames * kChannels);
    std::vector<int16_t> pcm(frames * kChannels);

    // Real-time pacing lives here, on this side. The host used to pace the
    // feeder by blocking in its device write, but that write happens on
    // the shared render dispatch thread, so it stalled every GL call
    // behind the audio clock and froze the app outright (live-caught).
    // The host now queues and returns immediately, so this thread keeps
    // its own deadline: one burst per burst-duration, which is exactly
    // the cadence a real AAudio callback runs at.
    const auto burst_duration =
        std::chrono::microseconds(1000000LL * kFramesPerBurst / kSampleRate);
    auto next_deadline = std::chrono::steady_clock::now();

    while (s->running.load(std::memory_order_relaxed)) {
        if (s->state.load(std::memory_order_relaxed) != AAUDIO_STREAM_STATE_STARTED) {
            // Paused: nothing to pull, and nothing to write. The host's
            // own buffer drains on its own.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        void* buffer = s->format == AAUDIO_FORMAT_PCM_FLOAT
                           ? static_cast<void*>(float_scratch.data())
                           : static_cast<void*>(pcm.data());
        int32_t result = AAUDIO_CALLBACK_RESULT_CONTINUE;
        if (s->data_callback != nullptr) {
            result = s->data_callback(s, s->data_callback_user, buffer,
                                      static_cast<int32_t>(frames));
        } else {
            // No callback installed: a real AAudio stream in this state is
            // written to with AAudioStream_write, which this build's engine
            // never imports. Silence is the honest output.
            std::memset(pcm.data(), 0, pcm.size() * sizeof(int16_t));
        }
        // The host takes float. When the engine asked for 16-bit, widen,
        // never the other way round, which is what used to clip the mix.
        if (s->format != AAUDIO_FORMAT_PCM_FLOAT) {
            for (size_t i = 0; i < pcm.size(); ++i) {
                float_scratch[i] = static_cast<float>(pcm[i]) / 32768.0f;
            }
        }
        // STUD_AUDIO_STATS=1: is the signal actually clipping, and was it
        // already clipping before this stub touched it? "Distorted only
        // when loud" is clipping by definition, and it matters a great
        // deal whether the engine handed over samples beyond full scale
        // (its own mix is too hot for the format it was told to produce)
        // or whether something here scaled them past it.
        static const bool audio_stats = std::getenv("STUD_AUDIO_STATS") != nullptr;
        if (audio_stats) {
            static uint64_t bursts = 0;
            static uint64_t clipped = 0;
            static uint64_t over_range = 0;
            static float peak_float = 0.0f;
            static int32_t peak_pcm = 0;
            for (float v : float_scratch) {
                const float a = v < 0.0f ? -v : v;
                if (a > 1.0f) ++over_range;
            }
            for (float v : float_scratch) {
                const float a = v < 0.0f ? -v : v;
                if (a > peak_float) peak_float = a;
                if (a >= 1.0f) ++clipped;
                const int32_t q = static_cast<int32_t>(a * 32768.0f);
                if (q > peak_pcm) peak_pcm = q;
            }
            if (++bursts % 200 == 0) {
                std::fprintf(stderr,
                             "stud: audio: format=%s peak_float=%.3f over_1.0=%llu "
                             "peak_pcm=%d at_full_scale=%llu\n",
                             s->format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "i16",
                             static_cast<double>(peak_float),
                             static_cast<unsigned long long>(over_range), peak_pcm,
                             static_cast<unsigned long long>(clipped));
                peak_float = 0.0f;
                peak_pcm = 0;
                over_range = 0;
                clipped = 0;
            }
        }
        uint64_t args[8] = {s->host_handle};
        const uint64_t fill = audio_connection().call(
            CallId::AudioWriteFrames, args, reinterpret_cast<const uint8_t*>(float_scratch.data()),
            static_cast<uint32_t>(float_scratch.size() * sizeof(float)), nullptr, 0, nullptr);

        // Track the device's clock instead of free-running against it.
        //
        // This thread paces itself by wall clock at exactly one burst per
        // burst-duration, but the audio device consumes at its own real
        // sample rate. Those two clocks are never identical, so an
        // open-loop feeder slowly separates from the device and the
        // host's ring alternately starves and backs up, small, regular
        // gaps in the waveform, which is what "compressed"-sounding audio
        // actually is here rather than any codec or bit depth.
        //
        // The host now returns how much it has buffered, so the deadline
        // is nudged to hold a steady target: a little early when the ring
        // is draining, a little late when it is filling. The correction
        // is a fraction of a burst, far too small to hear, and bounded so
        // a single late burst cannot make it lurch.
        // ~80ms of slack. The feeder is an ordinary thread competing with
        // an engine that saturates a core, so it will occasionally be
        // scheduled late however cheap its work is; the buffer has to
        // cover that, and 80ms is still well under what anyone notices in
        // a game. 40ms did not: it left 1162 underruns in 45 seconds even
        // after audio stopped sharing the render connection.
        constexpr uint64_t kTargetFill =
            static_cast<uint64_t>(kFramesPerBurst) * kChannels * 2 * 8;
        auto correction = std::chrono::microseconds(0);
        if (fill > 0) {
            const int64_t error = static_cast<int64_t>(fill) - static_cast<int64_t>(kTargetFill);
            const int64_t bound = burst_duration.count() / 4;
            int64_t adjust = error / 16;  // gentle: a fraction of the error
            if (adjust > bound) adjust = bound;
            if (adjust < -bound) adjust = -bound;
            correction = std::chrono::microseconds(adjust);
        }
        // Refill immediately rather than waiting out the deadline when the
        // host is nearly dry. Nudging the clock alone can only correct by
        // a fraction of a burst per burst, which cannot recover from one
        // long scheduling gap before the ring runs out, so a burst that
        // arrives late is followed by a second one straight away.
        constexpr uint64_t kLowWater = static_cast<uint64_t>(kFramesPerBurst) * kChannels * 2 * 2;
        if (fill > 0 && fill < kLowWater) {
            next_deadline = std::chrono::steady_clock::now();
            continue;
        }
        next_deadline += burst_duration + correction;
        const auto now = std::chrono::steady_clock::now();
        if (next_deadline > now) {
            std::this_thread::sleep_for(next_deadline - now);
        } else {
            // Fell behind (a long GL call held the connection, say):
            // resynchronise rather than trying to catch up by spinning,
            // which would only flood the host's queue.
            next_deadline = now;
        }
        if (result != AAUDIO_CALLBACK_RESULT_CONTINUE) {
            s->running.store(false, std::memory_order_relaxed);
            s->state.store(AAUDIO_STREAM_STATE_STOPPED, std::memory_order_relaxed);
        }
    }
}

}  // namespace

extern "C" {

int32_t AAudio_createStreamBuilder(void** builder) {
    if (builder == nullptr) return AAUDIO_ERROR_NULL;
    *builder = new Builder();
    return AAUDIO_OK;
}

int32_t AAudioStreamBuilder_delete(void* builder) {
    delete static_cast<Builder*>(builder);
    return AAUDIO_OK;
}

void AAudioStreamBuilder_setFormat(void* builder, int32_t format) {
    if (builder != nullptr) static_cast<Builder*>(builder)->format = format;
    std::fprintf(stderr, "stud: libaaudio: engine asked for format %d (%s)\n", format,
                 format == AAUDIO_FORMAT_PCM_FLOAT  ? "float"
                 : format == AAUDIO_FORMAT_PCM_I16 ? "i16"
                                                   : "unspecified/other");
}
void AAudioStreamBuilder_setDirection(void* builder, int32_t direction) {
    if (builder != nullptr) static_cast<Builder*>(builder)->direction = direction;
}
void AAudioStreamBuilder_setBufferCapacityInFrames(void* builder, int32_t frames) {
    if (builder != nullptr && frames > 0) static_cast<Builder*>(builder)->buffer_capacity = frames;
}
void AAudioStreamBuilder_setDataCallback(void* builder, AAudioStreamCallback cb, void* user) {
    if (builder == nullptr) return;
    static_cast<Builder*>(builder)->data_callback = cb;
    static_cast<Builder*>(builder)->data_callback_user = user;
}
void AAudioStreamBuilder_setErrorCallback(void* builder, AAudioErrorCallback cb, void* user) {
    if (builder == nullptr) return;
    static_cast<Builder*>(builder)->error_callback = cb;
    static_cast<Builder*>(builder)->error_callback_user = user;
}
// Accepted and ignored: these are hints a real device uses to pick a
// low-latency path or an input source. Stud has one output path.
void AAudioStreamBuilder_setPerformanceMode(void*, int32_t) {}
void AAudioStreamBuilder_setUsage(void*, int32_t) {}
void AAudioStreamBuilder_setInputPreset(void*, int32_t) {}

int32_t AAudioStreamBuilder_openStream(void* builder, void** stream_out) {
    if (builder == nullptr || stream_out == nullptr) return AAUDIO_ERROR_NULL;
    auto* b = static_cast<Builder*>(builder);
    if (b->direction != AAUDIO_DIRECTION_OUTPUT) {
        // Voice chat. The host opens the real microphone here and only
        // here, never at startup, and says so in the log.
        const uint64_t args[8] = {static_cast<uint64_t>(kSampleRate), 1, 0, 0, 0, 0, 0, 0};
        if (audio_connection().call(CallId::AudioOpenInputStream, args, nullptr, 0, nullptr, 0,
                                    nullptr) == 0) {
            std::printf("stud: libaaudio: no microphone available\n");
            std::fflush(stdout);
            return AAUDIO_ERROR_INVALID_STATE;
        }
        auto* input = new Stream();
        input->format = b->format;
        input->channels = 1;
        input->is_input = true;
        *stream_out = input;
        std::printf("stud: libaaudio: input stream opened\n");
        std::fflush(stdout);
        return AAUDIO_OK;
    }

    // Float frames: the host takes what the engine produces, without a
    // 16-bit narrowing in between.
    uint64_t args[8] = {static_cast<uint64_t>(kSampleRate), static_cast<uint64_t>(kChannels),
                        static_cast<uint64_t>(kChannels * 4)};
    uint64_t host = audio_connection().call(CallId::AudioOpenStream, args, nullptr, 0, nullptr, 0,
                                      nullptr);
    if (host == 0) {
        std::printf("stud: libaaudio: the host has no usable audio output\n");
        std::fflush(stdout);
        return AAUDIO_ERROR_INVALID_STATE;
    }

    auto* s = new Stream();
    s->host_handle = host;
    s->format = b->format == AAUDIO_FORMAT_PCM_FLOAT ? AAUDIO_FORMAT_PCM_FLOAT
                                                      : AAUDIO_FORMAT_PCM_I16;
    s->buffer_capacity = b->buffer_capacity;
    s->buffer_size = b->buffer_capacity;
    s->data_callback = b->data_callback;
    s->data_callback_user = b->data_callback_user;
    *stream_out = s;
    std::printf("stud: libaaudio: opened stream (%d Hz, %d ch, %s, callback=%s)\n", kSampleRate,
                kChannels, s->format == AAUDIO_FORMAT_PCM_FLOAT ? "float" : "i16",
                s->data_callback != nullptr ? "yes" : "no");
    std::fflush(stdout);
    return AAUDIO_OK;
}

int32_t AAudioStream_requestStart(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr) return AAUDIO_ERROR_NULL;
    s->state.store(AAUDIO_STREAM_STATE_STARTED, std::memory_order_relaxed);
    // A capture stream has nothing to feed, the engine reads from it,
    // and the host is already capturing from the moment it opened.
    if (!s->is_input && !s->running.exchange(true)) {
        s->feeder = std::thread(feed, s);
    }
    return AAUDIO_OK;
}

int32_t AAudioStream_requestPause(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr) return AAUDIO_ERROR_NULL;
    s->state.store(AAUDIO_STREAM_STATE_PAUSED, std::memory_order_relaxed);
    return AAUDIO_OK;
}

int32_t AAudioStream_requestStop(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr) return AAUDIO_ERROR_NULL;
    s->state.store(AAUDIO_STREAM_STATE_STOPPED, std::memory_order_relaxed);
    return AAUDIO_OK;
}

int32_t AAudioStream_close(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr) return AAUDIO_ERROR_NULL;
    s->running.store(false, std::memory_order_relaxed);
    if (s->feeder.joinable()) s->feeder.join();
    uint64_t args[8] = {s->host_handle};
    if (s->is_input) {
        const uint64_t none[8] = {};
        audio_connection().call(CallId::AudioCloseInputStream, none, nullptr, 0, nullptr, 0,
                                nullptr);
        delete s;
        return AAUDIO_OK;
    }
    audio_connection().call(CallId::AudioCloseStream, args, nullptr, 0, nullptr, 0, nullptr);
    delete s;
    return AAUDIO_OK;
}

int32_t AAudioStream_read(void* stream, void* buffer, int32_t frames, int64_t timeout_ns) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr || !s->is_input || buffer == nullptr || frames <= 0) return 0;
    const int32_t bytes_per_frame = s->channels * 4;  // float frames, as opened
    const uint64_t args[8] = {};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::nanoseconds(timeout_ns > 0 ? timeout_ns : 0);
    int32_t got = 0;
    // A real AAudio read blocks until it has the frames asked for or the
    // timeout runs out. The host never blocks, so the waiting is done
    // here, and with a zero timeout this returns whatever is already
    // captured, which is the same contract.
    for (;;) {
        uint32_t written = 0;
        const uint32_t want = static_cast<uint32_t>((frames - got) * bytes_per_frame);
        audio_connection().call(CallId::AudioReadFrames, args, nullptr, 0,
                                static_cast<uint8_t*>(buffer) + got * bytes_per_frame, want,
                                &written);
        got += static_cast<int32_t>(written / bytes_per_frame);
        if (got >= frames) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return got;
}

int32_t AAudioStream_getSampleRate(void*) { return kSampleRate; }
int32_t AAudioStream_getChannelCount(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    return s != nullptr ? s->channels : kChannels;
}
int32_t AAudioStream_getFormat(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    return s != nullptr ? s->format : AAUDIO_FORMAT_PCM_I16;
}
int32_t AAudioStream_getFramesPerBurst(void*) { return kFramesPerBurst; }
int32_t AAudioStream_getBufferCapacityInFrames(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    return s != nullptr ? s->buffer_capacity : kFramesPerBurst * 4;
}
int32_t AAudioStream_getBufferSizeInFrames(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    return s != nullptr ? s->buffer_size : kFramesPerBurst * 2;
}
int32_t AAudioStream_setBufferSizeInFrames(void* stream, int32_t frames) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr) return AAUDIO_ERROR_NULL;
    if (frames > 0) s->buffer_size = frames;
    return s->buffer_size;
}
int32_t AAudioStream_getState(void* stream) {
    auto* s = static_cast<Stream*>(stream);
    return s != nullptr ? s->state.load(std::memory_order_relaxed) : AAUDIO_STREAM_STATE_OPEN;
}
// Real underrun counter. Stud does not currently detect underruns (the
// host write blocks, so the feeder cannot outrun the device), and a
// fabricated count would misinform the engine's own latency tuning.
int32_t AAudioStream_getXRunCount(void*) { return 0; }

// Real state-change wait. Stud's own state transitions are immediate,
// there is no device handshake to wait on, since the host side owns the
// real stream, so the current state is reported straight back rather
// than sleeping out the timeout.
int32_t AAudioStream_waitForStateChange(void* stream, int32_t /*inputState*/, int32_t* nextState,
                                         int64_t /*timeoutNanoseconds*/) {
    auto* s = static_cast<Stream*>(stream);
    if (s == nullptr) return AAUDIO_ERROR_NULL;
    if (nextState != nullptr) *nextState = s->state.load(std::memory_order_relaxed);
    return AAUDIO_OK;
}

}  // extern "C"
