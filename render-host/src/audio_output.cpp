// Real audio output for Process C.
//
// Process B is bionic and cannot talk to the host's audio server, exactly
// as it cannot talk to the GPU, so audio crosses the same narrow,
// enumerable boundary the GL calls already do, and the device work happens
// here, in ordinary glibc code.
//
// Why this exists at all: the engine's own FMOD initialises an Android
// audio device when a game starts, and with no device it fails outright
// (live-caught in the engine's own log: FMOD_RESULT 51,
// FMOD_ERR_OUTPUT_INIT, immediately before the join stalls). Audio is not
// a nicety here; the game-start path runs through it.
//
// miniaudio is the backend: it speaks ALSA, PulseAudio, JACK and OSS
// itself rather than wrapping another library, so one implementation
// covers every host this targets, and its callback model is exactly the
// shape needed, it runs its own realtime thread and pulls, so nothing
// here has to invent a clock.
//
// It is compiled in rather than resolved by name at runtime, which is
// what makes audio simply always present: no package to declare, none
// to be missing, and nothing for a bundle to carry a private copy of.
// It adds no link-time dependency either: it opens libasound, libpulse
// and libjack as it finds them, so the same binary works on a host that
// has only one of the three.
//
// The device work must stay off the render dispatch thread. Opening a
// device talks to the audio server and blocks; doing that inline froze the
// app the instant a game started, the engine opens its device exactly
// then, and every GL call queued behind it. Live-caught, twice.

#include "stud/audio_output.h"

#include <atomic>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <cstdlib>

#include "miniaudio.h"

#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace stud::render_host {
namespace {

// One process-wide miniaudio context: it is what enumerates the backends
// and holds the connection to whichever sound server answered, and both
// the output device and the microphone are opened from it.
struct Audio {
    bool tried = false;
    bool ready = false;
    ma_context context{};
};

// ALSA writes its own diagnostics straight to stderr, and backend
// initialisation probes every PCM the system defines, so a normal
// launch printed `Unknown PCM cards.pcm.rear`, `...center_lfe`,
// `...side` and a `find_matching_chmap` complaint every time, about
// devices this machine simply does not have. None of it is a Stud error
// and none of it predicts a problem, but it sits in the log next to
// things that are.
//
// Kept rather than discarded: ALSA's messages are the only explanation
// available when audio genuinely fails to open, so they are collected
// and printed only if that happens.
std::mutex& alsa_message_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::string>& alsa_messages() {
    static std::vector<std::string> messages;
    return messages;
}

void collect_alsa_message(const char* file, int line, const char* function, int err,
                          const char* fmt, ...) {
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt != nullptr ? fmt : "", args);
    va_end(args);

    char full[700];
    if (err != 0) {
        std::snprintf(full, sizeof(full), "%s:%d:(%s) %s: %s", file != nullptr ? file : "?", line,
                      function != nullptr ? function : "?", text, std::strerror(err));
    } else {
        std::snprintf(full, sizeof(full), "%s:%d:(%s) %s", file != nullptr ? file : "?", line,
                      function != nullptr ? function : "?", text);
    }
    std::lock_guard<std::mutex> lock(alsa_message_mutex());
    // A bound, because a machine with a broken sound configuration can
    // produce these indefinitely and this is a diagnostic, not a log.
    if (alsa_messages().size() < 64) alsa_messages().emplace_back(full);
}

// Printed only when something audio-related actually failed.
void report_alsa_messages(const char* what) {
    std::vector<std::string> messages;
    {
        std::lock_guard<std::mutex> lock(alsa_message_mutex());
        messages.swap(alsa_messages());
    }
    if (messages.empty()) return;
    std::printf("stud-render-host: ALSA said this while %s:\n", what);
    for (const std::string& message : messages) {
        std::printf("stud-render-host:   %s\n", message.c_str());
    }
    std::fflush(stdout);
}

void silence_alsa_probe_noise() {
    static bool done = false;
    if (done) return;
    done = true;
    void* asound = ::dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (asound == nullptr) return;
    using ErrorHandler = void (*)(const char*, int, const char*, int, const char*, ...);
    using SetHandlerFn = int (*)(ErrorHandler);
    auto* set_handler =
        reinterpret_cast<SetHandlerFn>(::dlsym(asound, "snd_lib_error_set_handler"));
    if (set_handler != nullptr) set_handler(collect_alsa_message);
}

Audio& audio_context() {
    static Audio a;
    if (a.tried) return a;
    a.tried = true;
    // Present this stream as "Stud", with Stud's icon, in the desktop's
    // volume mixer.
    //
    // On the PulseAudio backend the application name below does this on
    // its own, because miniaudio hands it straight to libpulse. All of
    // this is for the ALSA path, which is what is left when there is no
    // sound server, or when one is reached through an ALSA compatibility
    // plugin: that plugin labels the entry with this process's own name
    // ("PipeWire ALSA [stud-render-host]"), an implementation detail no
    // user should have to recognise as Roblox.
    //
    // Every spelling is set because which one applies depends on which
    // server is running and which compatibility path it takes:
    //   - PULSE_PROP_* is read by the PulseAudio client library, and by
    //     PipeWire's pulse compatibility layer.
    //   - PIPEWIRE_PROPS is read by PipeWire's own client.
    //   - application.process.binary is what several mixers fall back to
    //     for the display name, which is exactly where
    //     "stud-render-host" was coming from.
    // The icon name matches Stud's installed hicolor icon ("stud"), so
    // the mixer resolves it through the normal icon theme lookup rather
    // than needing a path.
    ::setenv("PULSE_PROP_application.name", "Stud", /*overwrite=*/0);
    ::setenv("PULSE_PROP_application.icon_name", "stud", /*overwrite=*/0);
    ::setenv("PULSE_PROP_application.process.binary", "Stud", /*overwrite=*/0);
    ::setenv("PULSE_PROP_media.role", "game", /*overwrite=*/0);
    // node.description is the one that actually shows. PipeWire's ALSA
    // plugin builds its own label as "PipeWire ALSA [<program name>]"
    // from the process name, and that is what a mixer displays, so
    // application.name alone never replaced it (live-confirmed: the icon
    // set through these same props DID appear, proving the props are
    // applied, while the label stayed "PipeWire ALSA [stud-render-host]").
    // Setting node.description/node.nick overrides the plugin's
    // generated string directly.
    // PipeWire's ALSA plugin is a SEPARATE client from the one
    // PIPEWIRE_PROPS configures, and it is the one a mixer actually
    // shows. Confirmed by dumping the live graph: our props landed
    // perfectly on the JACK-side nodes (application.name = Stud,
    // node.description = Stud, icon and all) while the playback node
    // read `application.name = PipeWire ALSA [stud-render-host]` and
    // `node.name = alsa_playback.stud-render-host`, built by the
    // plugin from this process's own name, which no amount of
    // PIPEWIRE_PROPS could reach. PIPEWIRE_ALSA is that plugin's own
    // property channel.
    ::setenv("PIPEWIRE_ALSA",
             "{ application.name = Stud application.icon-name = stud "
             "node.description = Stud node.name = Stud node.nick = Stud "
             "media.role = Game }",
             /*overwrite=*/0);
    // And the fallback the plugin uses when it builds that string
    // itself: the program name. Only the library-visible name is
    // changed, not the thread comm, so `ps` still shows
    // stud-render-host and debugging is unaffected.
    program_invocation_short_name = const_cast<char*>("Stud");
    ::setenv("PIPEWIRE_PROPS",
             "{ application.name = Stud application.icon-name = stud "
             "application.process.binary = Stud media.role = Game "
             "node.description = Stud node.name = Stud node.nick = Stud }",
             /*overwrite=*/0);
    // The same job again for the OTHER ALSA plugin. Where the sound
    // server is reached through alsa-plugins-pulseaudio rather than
    // PipeWire's own plugin, which is what a container without
    // /dev/snd ends up using, the name comes out as
    // "ALSA plug-in [stud-render-host]": that plugin builds it from
    // "ALSA plug-in [%s]" and libpulse's own pa_get_binary_name(),
    // which reads /proc/self/exe and so ignores the program name set
    // above. PULSE_PROP_OVERRIDE is the one channel it does honour.
    //
    // media.name stays "ALSA Playback" whatever is put here, since the
    // plugin sets that per stream rather than on the connection. Mixers
    // show application.name, which is the one that matters.
    ::setenv("PULSE_PROP_OVERRIDE",
             "application.name=Stud media.role=game "
             "application.icon_name=io.github.catpieleaf.Stud",
             /*overwrite=*/0);
    silence_alsa_probe_noise();
    // The backends to try, in this order, named explicitly rather than
    // left to miniaudio's default list.
    //
    // PulseAudio first, because that is what a desktop actually runs:
    // PipeWire answers the same protocol through pipewire-pulse, and it
    // is the path that gives Stud a real entry in the volume mixer under
    // its own name and icon. ALSA next, for a machine with no sound
    // server at all. Then JACK, for a system deliberately built on it,
    // and OSS last, for the BSDs.
    //
    // miniaudio's default list would end with its null backend, which
    // accepts every device and plays nothing. It is compiled out
    // (MA_NO_NULL) and left out here as well: audio that silently
    // pretends to work is worse than audio that reports it never opened,
    // because the report is the only way anyone finds out.
    static const ma_backend backends[] = {
        ma_backend_pulseaudio,
        ma_backend_alsa,
        ma_backend_jack,
        ma_backend_oss,
    };
    ma_context_config config = ma_context_config_init();
    // What the volume mixer shows. miniaudio hands these straight to
    // libpulse and to JACK, so on those backends this is the whole job
    // and none of the environment variables above are involved.
    config.pulse.pApplicationName = "Stud";
    config.jack.pClientName = "Stud";
    const ma_result result = ma_context_init(
        backends, sizeof(backends) / sizeof(backends[0]), &config, &a.context);
    if (result != MA_SUCCESS) {
        std::printf("stud-render-host: no audio output (no backend started: %s)\n",
                    ma_result_description(result));
        std::fflush(stdout);
        report_alsa_messages("initialising audio");
        return a;
    }
    a.ready = true;
    std::printf("stud-render-host: audio: %s\n", ma_get_backend_name(a.context.backend));
    std::fflush(stdout);
    return a;
}

// Bursts queued by the client, drained by miniaudio's own thread. Bounded
// and dropping the oldest on overflow: a late buffer is worth less than a
// stalled renderer, and the client is paced by its own clock.
constexpr size_t kMaxQueuedBursts = 20;   // ~200ms at 48 kHz in 10ms bursts

// One process-wide output device, opened when Stud starts and kept open
// for its lifetime, playing silence until the engine has something to
// feed it. That is what makes Stud appear in the desktop's volume mixer
// from launch rather than only once a game happens to start a sound,
// which is how every other desktop application behaves, and what the user
// expects when looking for Stud's volume slider.
//
// The engine's own streams are attached to this one device rather than
// each opening their own: FMOD opens and closes its output as experiences
// come and go, and a mixer entry that appears and disappears with it would
// be useless to control.
struct Device {
    // ma_device is a transparent struct rather than a handle, and
    // miniaudio requires its address to stay put for its lifetime, so it
    // is held here by value and never copied.
    ma_device ma_dev{};
    bool device_open = false;
    int channels = 2;
    int rate = 48000;
    // 32-bit float, which is what the engine actually produces and what a
    // real Android device hands an AAudio stream that does not ask for a
    // format. Stud used to convert to 16-bit here, and that ceiling is
    // audible: a hot mix, loud, bassy content, hard-clips against full
    // scale instead of keeping its headroom until the system mixer has
    // applied the user's volume. Measured before changing anything:
    // samples pinned at full scale on exactly the passages that sounded
    // "deepfried".
    int bytes_per_sample = 4;

    // A single-producer/single-consumer ring, sized once and never
    // resized, replacing a mutex-guarded deque of per-burst vectors.
    //
    // Two real defects in that arrangement made audio sound bad, and
    // both were in the realtime callback. It took the lock with
    // `try_to_lock` and, whenever the producer happened to hold it,
    // filled the ENTIRE period with silence, so ordinary lock
    // contention became an audible dropout rather than a slightly late
    // sample. And on a partial take it did `erase(begin(), ...)`, an
    // O(n) memmove, in the same callback whose own comment says it must
    // not do that.
    //
    // With a ring the consumer only ever reads bytes that are already
    // there: no lock, no allocation, no memmove, and silence only on a
    // genuine underrun. Half a second of headroom absorbs scheduling
    // jitter without adding audible latency, since the steady-state fill
    // is whatever the engine is feeding, not the capacity.
    std::vector<uint8_t> ring;
    std::atomic<size_t> ring_read{0};   // monotonic byte counters, not indices
    std::atomic<size_t> ring_write{0};
    std::atomic<uint64_t> underruns{0};

    std::mutex mutex;
    std::condition_variable cv;
    bool closing = false;
    uint64_t dropped = 0;
    // Opens the real output device off the calling thread, because that
    // can take a while and the caller is the engine's own.
    std::thread opener;

    // This lives in a function-local static, so its destructor really does
    // run at exit, and destroying a joinable std::thread calls
    // std::terminate(). Live-caught as `terminate called without an active
    // exception` on every shutdown that had started audio. The thread only
    // opens the device and returns, so joining is a genuine wait for it to
    // finish rather than a cancellation.
    ~Device() {
        if (opener.joinable()) opener.join();
    }
};

Device& device() {
    static Device d;
    return d;
}

// miniaudio's realtime thread pulls from here. It must not block or
// allocate, so it only moves whole bursts out of the queue and pads with
// silence when the client has not fed one yet.
void playback_callback(ma_device* dev, void* output, const void* /*input*/, ma_uint32 frames) {
    auto* s = static_cast<Device*>(dev->pUserData);
    auto* out = static_cast<uint8_t*>(output);
    const size_t wanted = static_cast<size_t>(frames) * static_cast<size_t>(s->channels) *
                          static_cast<size_t>(s->bytes_per_sample);
    const size_t capacity = s->ring.size();
    if (capacity == 0) {
        std::memset(out, 0, wanted);
        return;
    }
    const size_t read = s->ring_read.load(std::memory_order_relaxed);
    const size_t write = s->ring_write.load(std::memory_order_acquire);
    size_t available = write - read;
    if (available > capacity) available = capacity;  // producer overran; take what is valid
    const size_t take = available < wanted ? available : wanted;
    for (size_t i = 0; i < take; ++i) {
        out[i] = s->ring[(read + i) % capacity];
    }
    if (take < wanted) {
        std::memset(out + take, 0, wanted - take);
        // Silence padded because the producer had not published enough.
        // Counted rather than logged here; this runs on the realtime
        // thread. A steady trickle of these is what an open-loop feeder
        // clock sounds like: small, regular gaps in the waveform.
        s->underruns.fetch_add(1, std::memory_order_relaxed);
    }
    s->ring_read.store(read + take, std::memory_order_release);
}

void open_device(Device* s) {
    Audio& a = audio_context();
    if (a.ready) {
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = s->bytes_per_sample == 4 ? ma_format_f32 : ma_format_s16;
        config.playback.channels = static_cast<ma_uint32>(s->channels);
        config.sampleRate = static_cast<ma_uint32>(s->rate);
        // 10 ms, which is the burst size the engine feeds in, so a
        // callback consumes exactly what one write produced.
        config.periodSizeInFrames = 480;
        config.dataCallback = playback_callback;
        config.pUserData = s;
        // Do NOT let miniaudio clip the buffer.
        //
        // It hard-clips a float32 playback buffer into [-1, 1] in place
        // after every data callback unless this is set, which is exactly
        // the ceiling the float32 format above exists to avoid: a hot
        // mix, loud bassy content, pinned at full scale instead of
        // keeping its headroom until the system mixer has applied the
        // user's volume. Live-caught as audio sounding compressed and
        // "deepfried" on precisely those passages, the same defect the
        // old 16-bit conversion produced and for the same reason.
        //
        // The sound server is the right place for this. PulseAudio and
        // PipeWire both take float samples outside [-1, 1] and resolve
        // them after volume, so nothing downstream needs Stud to have
        // flattened them first.
        config.noClip = MA_TRUE;
        // The per-stream label, beside the application name set on the
        // context.
        //
        // "Playback", not "Stud", and the difference is the whole point:
        // a mixer shows the client name and the stream name joined, so
        // naming the stream after the application too came out as
        // "Stud . Stud". Plasma's applet drops the stream half when it
        // matches /playback|audio|stream|alsa|pulse|pipewire/i, which is
        // why every other application on this machine appears under a
        // single name: Helium's stream is "Playback", speech-dispatcher's
        // is "playback". This is that convention, not a guess at one
        // mixer's behaviour, and a mixer that shows both still reads
        // correctly.
        //
        // It must be set to something: left null, miniaudio names the
        // stream "miniaudio:0".
        config.pulse.pStreamNamePlayback = "Playback";
        ma_result err = ma_device_init(&a.context, &config, &s->ma_dev);
        if (err == MA_SUCCESS) {
            err = ma_device_start(&s->ma_dev);
            if (err == MA_SUCCESS) {
                std::lock_guard<std::mutex> lock(s->mutex);
                s->device_open = true;
                std::printf("stud-render-host: audio: device open (%d Hz, %d ch, %s)\n", s->rate,
                            s->channels, s->bytes_per_sample == 4 ? "float32" : "16-bit");
                // And a warning, only when the device did not take what
                // it was handed. Silence otherwise: the matching case is
                // the normal one and does not need a line.
                //
                // This is worth saying out loud because of what it costs.
                // When the two differ miniaudio converts on every
                // callback, and a conversion into an integer format
                // clamps at full scale, destroying exactly the peaks
                // float32 output exists to preserve. That shipped once
                // already, as audio that sounded compressed on loud,
                // bassy passages, and it was invisible from the outside.
                const ma_format got = s->ma_dev.playback.internalFormat;
                const bool converting = got != ma_format_f32 ||
                                        s->ma_dev.playback.internalSampleRate !=
                                            static_cast<ma_uint32>(s->rate) ||
                                        s->ma_dev.playback.internalChannels !=
                                            static_cast<ma_uint32>(s->channels);
                if (converting) {
                    std::printf("stud-render-host: audio: the device took %u Hz, %u ch, %s "
                                "instead, so every buffer is converted; an integer format here "
                                "clips peaks above full scale\n",
                                s->ma_dev.playback.internalSampleRate,
                                s->ma_dev.playback.internalChannels, ma_get_format_name(got));
                }
                std::fflush(stdout);
            } else {
                ma_device_uninit(&s->ma_dev);
            }
        }
        if (!s->device_open) {
            std::printf("stud-render-host: audio: could not open a device (%s); this stream is "
                        "silent\n",
                        ma_result_description(err));
            std::fflush(stdout);
            report_alsa_messages("opening the output device");
        }
    }

    // Either way this thread now just waits for the close request: with a
    // device, miniaudio drives everything from its own thread; without
    // one, the queue is drained below so the client's feeder keeps running
    // normally rather than backing up behind a device that never arrived.
    std::unique_lock<std::mutex> lock(s->mutex);
    while (!s->closing) {
        s->cv.wait_for(lock, std::chrono::milliseconds(100));
        // No device: keep discarding so the client's feeder keeps
        // running normally instead of backing up behind a device that
        // never arrived.
        if (!s->device_open) {
            s->ring_read.store(s->ring_write.load(std::memory_order_acquire),
                               std::memory_order_release);
        }
    }
}

}  // namespace

void audio_start_output_device() {
    Device& d = device();
    if (d.ring.empty()) {
        // Half a second at the device's own format. Sized once so the
        // realtime callback never allocates.
        d.ring.assign(static_cast<size_t>(d.rate) * static_cast<size_t>(d.channels) *
                          static_cast<size_t>(d.bytes_per_sample) / 2,
                      0);
    }
    std::lock_guard<std::mutex> lock(d.mutex);
    if (d.opener.joinable()) return;   // already starting or started
    d.opener = std::thread(open_device, &d);
}

uint64_t audio_open_stream(int sample_rate, int channels, int bytes_per_frame) {
    if (!audio_context().ready) return 0;
    // Only 16-bit interleaved PCM at the device's own rate is accepted:
    // the client asks the engine for exactly that, and silently
    // mis-reading the samples would be worse than refusing.
    const Device& d = device();
    if (bytes_per_frame != channels * d.bytes_per_sample || channels != d.channels ||
        sample_rate != d.rate) {
        std::printf("stud-render-host: audio: refusing %d Hz / %d ch / %d bytes-per-frame (the "
                    "output device is %d Hz / %d ch / 16-bit)\n",
                    sample_rate, channels, bytes_per_frame, d.rate, d.channels);
        std::fflush(stdout);
        return 0;
    }
    audio_start_output_device();
    // Every engine stream plays through the one process-wide device, so
    // the handle is only a token saying "the engine has audio open now".
    static uint64_t next_handle = 1;
    const uint64_t id = next_handle++;
    std::printf("stud-render-host: audio: engine opened stream %llu (%d Hz, %d ch, 16-bit)\n",
                static_cast<unsigned long long>(id), sample_rate, channels);
    std::fflush(stdout);
    return id;
}

uint64_t audio_underruns() { return device().underruns.load(std::memory_order_relaxed); }

uint64_t audio_write_frames(uint64_t /*stream*/, const void* data, size_t bytes) {
    if (data == nullptr || bytes == 0) return 0;
    Device& d = device();
    // Hands off and returns immediately: the caller is the shared render
    // dispatch loop, and blocking it on the audio device stalls every GL
    // call behind it.
    const auto* first = static_cast<const uint8_t*>(data);
    const size_t capacity = d.ring.size();
    if (capacity == 0) return bytes;
    const size_t write = d.ring_write.load(std::memory_order_relaxed);
    const size_t read = d.ring_read.load(std::memory_order_acquire);
    const size_t space = capacity - (write - read);
    if (bytes > space) {
        // The device is behind. Dropping this burst keeps the ring
        // coherent for the reader; overwriting what it is mid-read would
        // not.
        if (++d.dropped == 1 || d.dropped % 500 == 0) {
            std::printf("stud-render-host: audio: dropped %llu late burst(s)\n",
                        static_cast<unsigned long long>(d.dropped));
            std::fflush(stdout);
        }
        return bytes;
    }
    for (size_t i = 0; i < bytes; ++i) {
        d.ring[(write + i) % capacity] = first[i];
    }
    d.ring_write.store(write + bytes, std::memory_order_release);
    // Report how much is now buffered. The feeder on the other side runs
    // on its own wall clock, which drifts against the device's real
    // sample clock; handing it the true fill lets it track the device
    // instead of free-running. Without this the two clocks separate and
    // the ring underruns on a regular cadence.
    const size_t fill = (write + bytes) - d.ring_read.load(std::memory_order_acquire);
    static uint64_t reported = 0;
    const uint64_t under = d.underruns.load(std::memory_order_relaxed);
    if (under != reported && (under - reported) >= 50) {
        reported = under;
        std::printf("stud-render-host: audio: %llu underrun(s) so far\n",
                    static_cast<unsigned long long>(under));
        std::fflush(stdout);
    }
    return fill;
}

void audio_close_stream(uint64_t stream) {
    // The device itself stays open for the process's lifetime (see
    // Device's own comment), what ends here is the engine feeding it, so
    // anything still queued is dropped rather than played after the sound
    // that produced it is gone.
    Device& d = device();
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.ring_read.store(d.ring_write.load(std::memory_order_acquire),
                          std::memory_order_release);
    }
    std::printf("stud-render-host: audio: engine closed stream %llu (device stays open)\n",
                static_cast<unsigned long long>(stream));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------
// Capture, for voice chat.
//
// Deliberately NOT opened at startup the way output is. A microphone is
// not something to hold open because it might be wanted later: it is
// opened when the engine asks for an input stream, which is when a user
// has joined voice chat, and closed when it stops asking. Everything
// here says so in the log, because a program holding a microphone open
// should be visible about it.
namespace {

struct Capture {
    ma_device ma_dev{};
    int channels = 1;
    int rate = 48000;
    // Same single-producer/single-consumer ring as the output side, with
    // the roles swapped: miniaudio's realtime thread writes, the engine's
    // own thread reads. It must not lock or allocate, so it does not.
    std::vector<uint8_t> ring;
    std::atomic<size_t> ring_read{0};
    std::atomic<size_t> ring_write{0};
    std::atomic<uint64_t> overruns{0};
    bool open = false;
};

Capture& capture() {
    static Capture c;
    return c;
}

void capture_callback(ma_device* dev, void* /*output*/, const void* input, ma_uint32 frames) {
    auto* c = static_cast<Capture*>(dev->pUserData);
    if (input == nullptr || c->ring.empty()) return;
    const size_t bytes = static_cast<size_t>(frames) * static_cast<size_t>(c->channels) * 4;
    const size_t write = c->ring_write.load(std::memory_order_relaxed);
    const size_t read = c->ring_read.load(std::memory_order_acquire);
    const size_t free_space = c->ring.size() - (write - read);
    if (bytes > free_space) {
        // Nobody is reading fast enough. Dropping the oldest audio is the
        // only option that keeps latency bounded, and a counter says it
        // happened rather than leaving it silent.
        c->overruns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto* src = static_cast<const uint8_t*>(input);
    for (size_t i = 0; i < bytes; ++i) {
        c->ring[(write + i) % c->ring.size()] = src[i];
    }
    c->ring_write.store(write + bytes, std::memory_order_release);
}

}  // namespace

uint64_t audio_open_input_stream(int sample_rate, int channels) {
    Capture& c = capture();
    if (c.open) return 1;
    Audio& a = audio_context();
    if (!a.ready) {
        std::printf("stud-render-host: audio: no capture device available\n");
        std::fflush(stdout);
        return 0;
    }
    c.channels = channels > 0 ? channels : 1;
    c.rate = sample_rate > 0 ? sample_rate : 48000;
    // Half a second of headroom, the same as the output ring: enough to
    // absorb scheduling jitter, short enough that a stall is noticed
    // rather than accumulated.
    c.ring.assign(static_cast<size_t>(c.rate) * static_cast<size_t>(c.channels) * 4 / 2, 0);
    c.ring_read.store(0);
    c.ring_write.store(0);

    ma_device_config config = ma_device_config_init(ma_device_type_capture);
    config.capture.format = ma_format_f32;
    config.capture.channels = static_cast<ma_uint32>(c.channels);
    config.sampleRate = static_cast<ma_uint32>(c.rate);
    config.periodSizeInFrames = 480;
    config.dataCallback = capture_callback;
    config.pUserData = &c;
    // Named for what it is rather than for the application again, the
    // same reasoning as the playback stream above. This one deliberately
    // does NOT use a word the mixer filters out: a recording stream
    // reading "Stud . Voice chat" tells a user looking at why their
    // microphone light is on exactly which part of Stud opened it, and
    // that is worth a second line where a playback stream's was pure
    // duplication.
    config.pulse.pStreamNameCapture = "Voice chat";
    ma_result err = ma_device_init(&a.context, &config, &c.ma_dev);
    if (err != MA_SUCCESS) {
        std::printf("stud-render-host: audio: could not open the microphone (%s)\n",
                    ma_result_description(err));
        std::fflush(stdout);
        report_alsa_messages("opening the microphone");
        return 0;
    }
    err = ma_device_start(&c.ma_dev);
    if (err != MA_SUCCESS) {
        std::printf("stud-render-host: audio: could not start the microphone (%s)\n",
                    ma_result_description(err));
        std::fflush(stdout);
        ma_device_uninit(&c.ma_dev);
        return 0;
    }
    c.open = true;
    std::printf("stud-render-host: audio: MICROPHONE OPEN (%d Hz, %d channel(s)), the engine "
                "asked for an input stream\n",
                c.rate, c.channels);
    std::fflush(stdout);
    return 1;
}

uint64_t audio_read_frames(void* out, size_t bytes) {
    Capture& c = capture();
    if (!c.open || out == nullptr || bytes == 0) return 0;
    const size_t read = c.ring_read.load(std::memory_order_relaxed);
    const size_t write = c.ring_write.load(std::memory_order_acquire);
    size_t available = write - read;
    if (available == 0) return 0;
    if (available > bytes) available = bytes;
    auto* dst = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < available; ++i) {
        dst[i] = c.ring[(read + i) % c.ring.size()];
    }
    c.ring_read.store(read + available, std::memory_order_release);
    return available;
}

void audio_close_input_stream() {
    Capture& c = capture();
    if (!c.open) return;
    // Uninit stops the device first, so the microphone is released here
    // rather than merely left idle.
    ma_device_uninit(&c.ma_dev);
    c.open = false;
    std::printf("stud-render-host: audio: microphone closed (%llu overrun(s))\n",
                static_cast<unsigned long long>(c.overruns.load()));
    std::fflush(stdout);
}

}  // namespace stud::render_host
