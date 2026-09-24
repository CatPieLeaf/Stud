#include "stud/haptics_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/app_java_classes.h"
#include "stud/trap_recovery.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace stud::jni_bridge {

namespace {

using GetMessageIdFn = jstring (*)(JNIEnv*, jclass, jstring, jstring);
using DoSubscribeRawFn = jobject (*)(JNIEnv*, jobject, jstring, jobject, jboolean);
using PublishResponseRawFn = void (*)(JNIEnv*, jobject, jstring, jstring, jstring, jint, jstring);
using UpdateStateFn = void (*)(JNIEnv*, jclass, jboolean, jstring);

constexpr const char* kProtocol = "HapticProtocol";
// A real device names one entry per vibrator it found; a pad is one
// motor, so it is named the same way the first vibrator would be.
constexpr const char* kMotor = "vibrator_0";

struct State {
    std::mutex mutex;
    int device_id = -1;
    bool can_rumble = false;
    RumbleFn rumble;
    // Waveform playback. The generation counter is what stops a running
    // waveform: a new request bumps it and the old thread notices and
    // returns, which is simpler and safer than trying to join a thread
    // from inside a MessageBus callback.
    std::atomic<uint64_t> generation{0};
};

State& state() {
    static State s;
    return s;
}

std::string read_jstring(FakeJni::Env& env, jstring value) {
    if (value == nullptr) return {};
    auto resolved = env.resolveReference(value);
    auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved);
    return as_string ? as_string->asStdString() : std::string();
}

// The engine's payloads here are flat and small: one number, or one
// object holding a number and an array of numbers. Reading them directly
// keeps this layer free of a parser dependency, the same way the
// web-view bridge already does for its two string fields.
double json_number_field(const std::string& json, const std::string& key, double fallback) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '"')) ++pos;
    try {
        return std::stod(json.substr(pos));
    } catch (const std::exception&) {
        return fallback;
    }
}

std::vector<float> json_number_array(const std::string& json, const std::string& key) {
    std::vector<float> out;
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return out;
    pos = json.find('[', pos + needle.size());
    if (pos == std::string::npos) return out;
    const auto end = json.find(']', pos);
    if (end == std::string::npos) return out;
    ++pos;
    while (pos < end) {
        while (pos < end && (json[pos] == ' ' || json[pos] == ',')) ++pos;
        if (pos >= end) break;
        size_t used = 0;
        try {
            out.push_back(std::stof(json.substr(pos, end - pos), &used));
        } catch (const std::exception&) {
            break;
        }
        pos += used == 0 ? 1 : used;
    }
    return out;
}

// One intensity drives both motors. A pad's heavy and light motors are
// not separately addressable through this protocol, the engine asks
// for a single per-motor intensity, so both are given the same value,
// which is what a single-vibrator device does too.
void apply(float intensity, int duration_ms) {
    RumbleFn rumble;
    int device_id = -1;
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        if (!state().can_rumble) return;
        rumble = state().rumble;
        device_id = state().device_id;
    }
    if (rumble) rumble(device_id, intensity, intensity, duration_ms);
}

void play_waveform(std::vector<float> intensities, float ms_per_sample) {
    const uint64_t mine = ++state().generation;
    if (intensities.empty() || ms_per_sample <= 0.0f) {
        apply(0.0f, 0);
        return;
    }
    std::thread([intensities = std::move(intensities), ms_per_sample, mine] {
        for (float level : intensities) {
            if (state().generation.load() != mine) return;  // superseded
            // Each sample is held for its own duration, so the kernel
            // keeps the motor running even if the next sample is late.
            apply(level, static_cast<int>(ms_per_sample) + 1);
            std::this_thread::sleep_for(
                std::chrono::microseconds(static_cast<long long>(ms_per_sample * 1000.0f)));
        }
        if (state().generation.load() == mine) apply(0.0f, 0);
    }).detach();
}

std::string motors_json() {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (!state().can_rumble) return "{}";
    // The position vector is what a real device reports for a vibrator
    // it cannot place: zero. Inventing one would be fabricated data.
    return std::string("{\"") + kMotor + "\":{\"x\":0.0,\"y\":0.0,\"z\":0.0}}";
}

bool supports_haptics() {
    std::lock_guard<std::mutex> lock(state().mutex);
    return state().can_rumble;
}

std::string message_id(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                       const std::string& method) {
    auto* fn = reinterpret_cast<GetMessageIdFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_getMessageId"));
    if (fn == nullptr) return {};
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass("com/roblox/universalapp/messagebus/MessageBus");
    jstring result = nullptr;
    if (!call_trapping_abort_with_result(fn, result, jni_env, cls, env.NewStringUTF(kProtocol),
                                         env.NewStringUTF(method.c_str()))) {
        return {};
    }
    clear_pending_jni_exception(jni_env, "MessageBus.getMessageId");
    return read_jstring(env, result);
}

// The engine answers a request by publishing a response on the same
// protocol/method pair, exactly as the real Java handler does.
void publish_response(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                      const std::string& method, const std::string& json) {
    auto* publish = reinterpret_cast<PublishResponseRawFn>(lib.find_symbol(
        "Java_com_roblox_universalapp_messagebus_MessageBus_publishProtocolMethodResponseRaw"));
    if (publish == nullptr) return;
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    auto bus = std::make_shared<MessageBusJava>();
    call_trapping_abort(publish, jni_env, env.createLocalReference(bus),
                        env.NewStringUTF(kProtocol), env.NewStringUTF(method.c_str()),
                        env.NewStringUTF(json.c_str()), static_cast<jint>(0),
                        env.NewStringUTF("{}"));
    clear_pending_jni_exception(jni_env, "MessageBus.publishProtocolMethodResponseRaw");
}

void report_state(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    auto* update = reinterpret_cast<UpdateStateFn>(lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeGLInterface_nativeUpdateMobileHapticsState"));
    if (update == nullptr) return;
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass("com/roblox/engine/jni/NativeGLInterface");
    const std::string motors = motors_json();
    call_trapping_abort(update, jni_env, cls,
                        static_cast<jboolean>(supports_haptics() ? JNI_TRUE : JNI_FALSE),
                        env.NewStringUTF(motors.c_str()));
    clear_pending_jni_exception(jni_env, "nativeUpdateMobileHapticsState");
}

bool g_registered = false;

}  // namespace

bool run_haptics_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                        RumbleFn rumble) {
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().rumble = std::move(rumble);
    }
    if (g_registered) {
        report_state(jvm, lib);
        return true;
    }

    auto* do_subscribe = reinterpret_cast<DoSubscribeRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw"));
    if (do_subscribe == nullptr) {
        std::fprintf(stderr, "stud: haptics: MessageBus entry points missing\n");
        return false;
    }

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto subscribe = [&](const char* method, std::function<void(const std::string&)> on_payload) {
        const std::string topic = message_id(env, lib, method);
        if (topic.empty()) {
            std::fprintf(stderr, "stud: haptics: no message id for %s\n", method);
            return;
        }
        auto bus = std::make_shared<MessageBusJava>();
        auto callback = std::make_shared<MessageBusRawCallbackJava>();
        callback->handler = std::move(on_payload);
        jobject connection = nullptr;
        const bool ok = call_trapping_abort_with_result(
            do_subscribe, connection, jni_env, env.createLocalReference(bus),
            env.NewStringUTF(topic.c_str()), env.createLocalReference(callback),
            static_cast<jboolean>(JNI_FALSE));
        clear_pending_jni_exception(jni_env, "MessageBus.doSubscribeRaw");
        std::printf("stud: haptics: subscribed to %s: %s\n", method, ok ? "ok" : "trapped");
        // The callbacks outlive this frame, the bus keeps calling them.
        static std::vector<std::shared_ptr<MessageBusRawCallbackJava>> kept;
        kept.push_back(callback);
    };

    // These capture the Jvm and library by reference deliberately: both
    // outlive the process's whole run (Process B tears down by _exit()).
    auto* jvm_ptr = &jvm;
    const auto* lib_ptr = &lib;

    subscribe("SupportsHaptics", [jvm_ptr, lib_ptr](const std::string&) {
        publish_response(*jvm_ptr, *lib_ptr, "SupportsHaptics",
                         std::string("{\"supportsHaptics\":") +
                             (supports_haptics() ? "true" : "false") + "}");
    });
    subscribe("GetMotors", [jvm_ptr, lib_ptr](const std::string&) {
        publish_response(*jvm_ptr, *lib_ptr, "GetMotors", motors_json());
    });
    subscribe("UpdateSingletonVibration", [](const std::string& payload) {
        const float intensity = static_cast<float>(json_number_field(payload, "intensity", 0.0));
        // Duration 0 is "hold it": the real handler loops a 1000ms
        // waveform for the same effect, and the engine sends a zero when
        // it wants the motor to stop.
        ++state().generation;  // a singleton vibration supersedes a waveform
        apply(intensity, 0);
    });
    subscribe("SetMotorState", [](const std::string& payload) {
        const float ms = static_cast<float>(json_number_field(payload, "millisecondsPerSample", 0.0));
        play_waveform(json_number_array(payload, "intensities"), ms);
    });

    g_registered = true;
    report_state(jvm, lib);
    std::fflush(stdout);
    return true;
}

void set_haptics_device(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib, int device_id,
                        bool can_rumble) {
    {
        std::lock_guard<std::mutex> lock(state().mutex);
        state().device_id = device_id;
        state().can_rumble = can_rumble;
    }
    if (!can_rumble) ++state().generation;  // nothing left to play on
    std::printf("stud: haptics: %s\n",
                can_rumble ? "controller rumble available" : "no controller rumble");
    std::fflush(stdout);
    if (g_registered) report_state(jvm, lib);
}

void stop_haptics() {
    ++state().generation;
    apply(0.0f, 0);
}

}  // namespace stud::jni_bridge
