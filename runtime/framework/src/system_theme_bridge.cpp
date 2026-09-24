#include "stud/system_theme_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/app_java_classes.h"
#include "stud/trap_recovery.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

namespace stud::jni_bridge {

namespace {

using GetStringFn = jstring (*)(JNIEnv*, jclass);
using GetMessageIdFn = jstring (*)(JNIEnv*, jclass, jstring, jstring);
using DoSubscribeRawFn = jobject (*)(JNIEnv*, jobject, jstring, jobject, jboolean);
using PublishRawFn = void (*)(JNIEnv*, jobject, jstring, jstring);

constexpr const char* kProtocolClass = "com/roblox/universalapp/systemtheme/JNISystemThemeProtocol";
constexpr const char* kSymbolPrefix = "Java_com_roblox_universalapp_systemtheme_JNISystemThemeProtocol_";

// The real enum from SystemThemeProtocol.a, confirmed against the app's own code.
constexpr int kThemeError = 0;
constexpr int kThemeLight = 1;
constexpr int kThemeDark = 2;
constexpr int kThemeSystemLight = 3;
constexpr int kThemeSystemDark = 4;

std::atomic<bool>& dark_flag() {
    static std::atomic<bool> dark{false};
    return dark;
}

std::mutex& theme_mutex() {
    static std::mutex m;
    return m;
}

std::string& theme_name_storage() {
    static std::string name;
    return name;
}

std::string read_jstring(FakeJni::Env& env, jstring value) {
    if (value == nullptr) return {};
    auto resolved = env.resolveReference(value);
    auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved);
    return as_string ? as_string->asStdString() : std::string();
}

// Every identifier in this protocol has its own exported getter, so none
// of the wire names are written down here.
std::string call_string_getter(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                               const char* method) {
    const std::string symbol = std::string(kSymbolPrefix) + method;
    auto* fn = reinterpret_cast<GetStringFn>(lib.find_symbol(symbol.c_str()));
    if (fn == nullptr) {
        std::fprintf(stderr, "stud: system theme: %s not exported\n", symbol.c_str());
        return {};
    }
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass(kProtocolClass);
    jstring result = nullptr;
    if (!call_trapping_abort_with_result(fn, result, jni_env, cls)) return {};
    clear_pending_jni_exception(jni_env, method);
    return read_jstring(env, result);
}

std::string message_id(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                       const std::string& protocol, const std::string& method) {
    auto* fn = reinterpret_cast<GetMessageIdFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_getMessageId"));
    if (fn == nullptr) return {};
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass("com/roblox/universalapp/messagebus/MessageBus");
    jstring result = nullptr;
    if (!call_trapping_abort_with_result(fn, result, jni_env, cls,
                                         env.NewStringUTF(protocol.c_str()),
                                         env.NewStringUTF(method.c_str()))) {
        return {};
    }
    clear_pending_jni_exception(jni_env, "MessageBus.getMessageId");
    return read_jstring(env, result);
}

// The value arrives as a JSON string holding an integer, the real
// handler does `optString(key)` then `toIntOrNull`, but a bare number
// is accepted too rather than depending on which of the two the app
// happens to send.
int theme_value_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return kThemeError;
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return kThemeError;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '"')) ++pos;
    int value = 0;
    bool any = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
        any = true;
    }
    return any ? value : kThemeError;
}

// The real enum's own themeName: LIGHT and SYSTEM_LIGHT are "light",
// DARK and SYSTEM_DARK are "dark", ERROR is empty.
const char* theme_name_for(int value) {
    switch (value) {
        case kThemeLight:
        case kThemeSystemLight: return "light";
        case kThemeDark:
        case kThemeSystemDark: return "dark";
        default: return "";
    }
}

struct ProtocolIds {
    std::string protocol;
    std::string set_theme_message;
    std::string updated_message;
    std::string param_key;
    bool valid = false;
};

ProtocolIds& cached_ids() {
    static ProtocolIds ids;
    return ids;
}

ProtocolIds read_ids(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    {
        std::lock_guard<std::mutex> lock(theme_mutex());
        if (cached_ids().valid) return cached_ids();
    }
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();

    ProtocolIds ids;
    ids.protocol = call_string_getter(env, lib, "getProtocolName");
    ids.set_theme_message = call_string_getter(env, lib, "getSystemThemeSetThemeMessage");
    ids.updated_message = call_string_getter(env, lib, "getSystemThemeUpdatedMessage");
    ids.param_key = call_string_getter(env, lib, "getSystemThemeParamKey");
    ids.valid = !ids.protocol.empty() && !ids.set_theme_message.empty();

    std::lock_guard<std::mutex> lock(theme_mutex());
    cached_ids() = ids;
    return ids;
}

}  // namespace

void set_system_dark_mode(bool dark) { dark_flag().store(dark); }

bool system_dark_mode() { return dark_flag().load(); }

int system_theme_value() {
    // SYSTEM_DARK/SYSTEM_LIGHT is what a platform honestly reports: the
    // desktop's own setting, with "system" meaning the app is free to
    // follow its own preference instead. STUD_FORCE_THEME exists to test
    // whether this app follows it at all; it claims the platform theme
    // is explicitly dark or light (DARK 2 / LIGHT 1), which is what a
    // device would report only if the user had chosen that per-app.
    if (const char* forced = std::getenv("STUD_FORCE_THEME");
        forced != nullptr && *forced != '\0') {
        const std::string_view value(forced);
        if (value == "dark") return kThemeDark;
        if (value == "light") return kThemeLight;
    }
    return dark_flag().load() ? kThemeSystemDark : kThemeSystemLight;
}

std::string current_theme_name() {
    std::lock_guard<std::mutex> lock(theme_mutex());
    if (!theme_name_storage().empty()) return theme_name_storage();
    // Nothing published yet: the desktop's own setting is the honest
    // answer, and it is what the app will settle on anyway once it asks.
    return dark_flag().load() ? "dark" : "light";
}

bool run_system_theme_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                std::function<void(const std::string&)> on_theme) {
    const ProtocolIds ids = read_ids(jvm, lib);
    if (!ids.valid) {
        std::fprintf(stderr, "stud: system theme: protocol identifiers unavailable\n");
        return false;
    }
    std::printf("stud: system theme: name=\"%s\" setTheme=\"%s\" updated=\"%s\" key=\"%s\" "
                "desktop=%s\n",
                ids.protocol.c_str(), ids.set_theme_message.c_str(), ids.updated_message.c_str(),
                ids.param_key.c_str(), dark_flag().load() ? "dark" : "light");
    std::fflush(stdout);

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* do_subscribe = reinterpret_cast<DoSubscribeRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw"));
    if (do_subscribe == nullptr) {
        std::fprintf(stderr, "stud: system theme: MessageBus.doSubscribeRaw missing\n");
        return false;
    }

    const std::string topic = message_id(env, lib, ids.protocol, ids.set_theme_message);
    if (topic.empty()) return false;

    auto bus = std::make_shared<MessageBusJava>();
    auto callback = std::make_shared<MessageBusRawCallbackJava>();
    const std::string param_key = ids.param_key;
    callback->handler = [param_key, on_theme](const std::string& payload) {
        const int value = theme_value_field(payload, param_key);
        const std::string name = theme_name_for(value);
        if (name.empty()) return;
        {
            std::lock_guard<std::mutex> lock(theme_mutex());
            theme_name_storage() = name;
        }
        std::printf("stud: system theme: app set theme %d (%s)\n", value, name.c_str());
        std::fflush(stdout);
        if (on_theme) on_theme(name);
    };
    jobject connection = nullptr;
    const bool ok = call_trapping_abort_with_result(
        do_subscribe, connection, jni_env, env.createLocalReference(bus),
        env.NewStringUTF(topic.c_str()), env.createLocalReference(callback),
        static_cast<jboolean>(JNI_FALSE));
    clear_pending_jni_exception(jni_env, "MessageBus.doSubscribeRaw");
    std::printf("stud: system theme: subscribed to %s: %s\n", topic.c_str(), ok ? "ok" : "trapped");
    std::fflush(stdout);
    // The bus keeps calling this, so it has to outlive the frame.
    static std::vector<std::shared_ptr<MessageBusRawCallbackJava>> kept;
    kept.push_back(callback);
    return ok;
}

void publish_system_theme_updated(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    const ProtocolIds ids = read_ids(jvm, lib);
    if (!ids.valid || ids.updated_message.empty()) return;

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* publish = reinterpret_cast<PublishRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw"));
    if (publish == nullptr) return;
    const std::string topic = message_id(env, lib, ids.protocol, ids.updated_message);
    if (topic.empty()) return;

    // The real publisher sends an empty object: the message says only
    // "ask again", and the app reads the value back through
    // SystemThemeProtocol.getSystemTheme().
    auto bus = std::make_shared<MessageBusJava>();
    const bool ok = call_trapping_abort(publish, jni_env, env.createLocalReference(bus),
                                        env.NewStringUTF(topic.c_str()), env.NewStringUTF("{}"));
    clear_pending_jni_exception(jni_env, "MessageBus.publishRaw");
    std::printf("stud: system theme: published themeUpdated: %s\n", ok ? "ok" : "trapped");
    std::fflush(stdout);
}

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

// Defined here rather than in the header so the class does not have to know
// where the desktop's setting comes from.
FakeJni::JInt SystemThemeProtocolJava::getSystemTheme() {
    const int value = system_theme_value();
    // Whether the app asks at all is the whole question when its own
    // appearance setting and the desktop's disagree, so say so.
    std::printf("stud: system theme: the app asked, answered %d (%s)\n", value,
                theme_name_for(value));
    std::fflush(stdout);
    return static_cast<FakeJni::JInt>(value);
}

}  // namespace stud::jni_bridge
