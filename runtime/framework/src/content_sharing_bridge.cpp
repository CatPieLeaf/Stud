#include "stud/content_sharing_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/game_activity_stubs.h"
#include "stud/trap_recovery.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace stud::jni_bridge {

namespace {

using DoSubscribeRawFn = jobject (*)(JNIEnv*, jobject, jstring, jobject, jboolean);
using GetStringFn = jstring (*)(JNIEnv*, jclass);

constexpr const char* kProtocolClass =
    "com/roblox/universalapp/externalcontentsharing/JNIExternalContentSharingProtocol";
constexpr const char* kSymbolPrefix =
    "Java_com_roblox_universalapp_externalcontentsharing_"
    "JNIExternalContentSharingProtocol_";

std::string read_jstring(FakeJni::Env& env, jstring value) {
    if (value == nullptr) return {};
    auto resolved = env.resolveReference(value);
    auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved);
    return as_string ? as_string->asStdString() : std::string();
}

// The four share ids DO have getters, unlike the clipboard one.
std::string call_string_getter(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                               const char* method) {
    const std::string symbol = std::string(kSymbolPrefix) + method;
    auto* fn = reinterpret_cast<GetStringFn>(lib.find_symbol(symbol.c_str()));
    if (fn == nullptr) return {};
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass(kProtocolClass);
    jstring result = nullptr;
    if (!call_trapping_abort_with_result(fn, result, jni_env, cls)) return {};
    clear_pending_jni_exception(jni_env, method);
    return read_jstring(env, result);
}

constexpr const char* kSetClipboardTextTopic = "ExternalContentSharing.setClipboardText";
constexpr const char* kTextKey = "text";

// The payloads here are the engine's own and small; one string field out
// of a flat object does not justify a parser in this layer, same as the
// other MessageBus bridges.
std::string json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            ++pos;
            switch (json[pos]) {
                case 'n': out.push_back('\n'); ++pos; continue;
                case 't': out.push_back('\t'); ++pos; continue;
                case 'r': ++pos; continue;
                default: break;
            }
        }
        out.push_back(json[pos++]);
    }
    return out;
}

}  // namespace

bool run_content_sharing_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                std::function<void(const std::string& text)> on_text) {
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* do_subscribe = reinterpret_cast<DoSubscribeRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw"));
    if (do_subscribe == nullptr) {
        std::fprintf(stderr, "stud: clipboard: MessageBus.doSubscribeRaw missing\n");
        return false;
    }

    // Every way the engine can ask for content to leave the app, not
    // just the clipboard one. A desktop has no Android share sheet, so
    // sharing a link or some text here means putting it on the
    // clipboard -- and subscribing to all of them is also what says,
    // in the log, which one a given button actually uses.
    auto subscribe = [&](const std::string& topic,
                         std::function<void(const std::string&)> handler) {
        if (topic.empty()) return;
        auto cb = std::make_shared<MessageBusRawCallbackStub>();
        cb->handler = std::move(handler);
        auto b = std::make_shared<MessageBusStub>();
        jobject conn = nullptr;
        const bool ok = call_trapping_abort_with_result(
            do_subscribe, conn, jni_env, env.createLocalReference(b),
            env.NewStringUTF(topic.c_str()), env.createLocalReference(cb),
            static_cast<jboolean>(JNI_FALSE));
        clear_pending_jni_exception(jni_env, "MessageBus.doSubscribeRaw");
        std::printf("stud: clipboard: subscribed to %s: %s\n", topic.c_str(),
                    ok ? "ok" : "trapped");
        std::fflush(stdout);
        static std::vector<std::shared_ptr<MessageBusRawCallbackStub>> kept_share;
        kept_share.push_back(cb);
    };
    for (const char* getter : {"getShareTextId", "getShareUrlId"}) {
        const std::string topic = call_string_getter(env, lib, getter);
        subscribe(topic, [on_text, topic](const std::string& payload) {
            // Both carry their content under a key of their own; try the
            // two this protocol uses and take whichever is there.
            std::string text = json_string_field(payload, "url");
            if (text.empty()) text = json_string_field(payload, kTextKey);
            std::printf("stud: clipboard: %s asked to share %zu bytes\n", topic.c_str(),
                        text.size());
            std::fflush(stdout);
            if (!text.empty() && on_text) on_text(text);
        });
    }
    for (const char* getter : {"getShareImageId", "getShareVideoId"}) {
        const std::string topic = call_string_getter(env, lib, getter);
        subscribe(topic, [topic](const std::string&) {
            // A file, not text. Nothing honest to do with it yet, so it
            // says so rather than pretending to have shared something.
            std::printf("stud: clipboard: %s asked to share a file -- not supported yet\n",
                        topic.c_str());
            std::fflush(stdout);
        });
    }

    auto callback = std::make_shared<MessageBusRawCallbackStub>();
    callback->handler = [on_text](const std::string& payload) {
        const std::string text = json_string_field(payload, kTextKey);
        if (text.empty()) {
            std::printf("stud: clipboard: the app asked to copy nothing\n");
            std::fflush(stdout);
            return;
        }
        // Never the text itself: an invite link carries a one-time code.
        std::printf("stud: clipboard: the app asked to copy %zu bytes\n", text.size());
        std::fflush(stdout);
        if (on_text) on_text(text);
    };
    auto bus = std::make_shared<MessageBusStub>();
    jobject connection = nullptr;
    const bool ok = call_trapping_abort_with_result(
        do_subscribe, connection, jni_env, env.createLocalReference(bus),
        env.NewStringUTF(kSetClipboardTextTopic), env.createLocalReference(callback),
        static_cast<jboolean>(JNI_FALSE));
    clear_pending_jni_exception(jni_env, "MessageBus.doSubscribeRaw");
    std::printf("stud: clipboard: subscribed to %s: %s\n", kSetClipboardTextTopic,
                ok ? "ok" : "trapped");
    std::fflush(stdout);
    // The bus keeps calling it, so it outlives this frame.
    static std::vector<std::shared_ptr<MessageBusRawCallbackStub>> kept;
    kept.push_back(callback);
    return ok;
}

}  // namespace stud::jni_bridge
