#include "stud/linking_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/game_activity_stubs.h"
#include "stud/trap_recovery.h"

#include <cstdio>
#include <memory>
#include <vector>
#include <string>

namespace stud::jni_bridge {

namespace {

using GetStringFn = jstring (*)(JNIEnv*, jclass);
using GetMessageIdFn = jstring (*)(JNIEnv*, jclass, jstring, jstring);
using DoSubscribeRawFn = jobject (*)(JNIEnv*, jobject, jstring, jobject, jboolean);
using SetRequestHandlerRawFn = void (*)(JNIEnv*, jobject, jstring, jstring, jobject);

constexpr const char* kProtocolClass = "com/roblox/universalapp/linking/JNILinkingProtocol";
constexpr const char* kSymbolPrefix = "Java_com_roblox_universalapp_linking_JNILinkingProtocol_";

std::string read_jstring(FakeJni::Env& env, jstring value) {
    if (value == nullptr) return {};
    auto resolved = env.resolveReference(value);
    auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved);
    return as_string ? as_string->asStdString() : std::string();
}

std::string call_string_getter(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                               const char* method) {
    const std::string symbol = std::string(kSymbolPrefix) + method;
    auto* fn = reinterpret_cast<GetStringFn>(lib.find_symbol(symbol.c_str()));
    if (fn == nullptr) {
        std::fprintf(stderr, "stud: linking: %s not exported\n", symbol.c_str());
        return {};
    }
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass(kProtocolClass);
    jstring result = nullptr;
    if (!call_trapping_abort_with_result(fn, result, jni_env, cls)) {
        std::fprintf(stderr, "stud: linking: %s trapped\n", method);
        return {};
    }
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

// Minimal JSON string field read, same shape webview_bridge uses: the
// payloads here are small and engine-generated.
std::string json_string_field(const std::string& json, const std::string& key) {
    if (key.empty()) return {};
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
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
        out.push_back(json[pos++]);
    }
    return out;
}

// A URL can carry a one-time ticket in its query, so only the part that
// identifies the page is ever logged.
std::string loggable_url(const std::string& url) {
    const auto cut = url.find('?');
    return cut == std::string::npos ? url : url.substr(0, cut) + "?<query withheld>";
}

}  // namespace

LinkingIds read_linking_ids(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();

    LinkingIds ids;
    ids.protocol = call_string_getter(env, lib, "getProtocolName");
    ids.open_url_id = call_string_getter(env, lib, "getOpenURLId");
    ids.url_key = call_string_getter(env, lib, "getUrlKey");
    ids.success_key = call_string_getter(env, lib, "getSuccessKey");
    ids.resolved = !ids.protocol.empty() && !ids.open_url_id.empty();

    std::printf("stud: linking protocol: name=\"%s\" openURL=\"%s\" urlKey=\"%s\" "
                "successKey=\"%s\"\n",
                ids.protocol.c_str(), ids.open_url_id.c_str(), ids.url_key.c_str(),
                ids.success_key.c_str());
    std::fflush(stdout);
    return ids;
}

bool run_linking_protocol_bootstrap(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                     std::function<bool(const std::string&)> on_open_url) {
    const LinkingIds ids = read_linking_ids(jvm, lib);
    if (!ids.resolved) {
        std::fprintf(stderr, "stud: linking: protocol identifiers unavailable -- not registering\n");
        return false;
    }

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* set_request_handler = reinterpret_cast<SetRequestHandlerRawFn>(lib.find_symbol(
        "Java_com_roblox_universalapp_messagebus_MessageBus_setRequestHandlerRaw"));
    if (set_request_handler == nullptr) {
        std::fprintf(stderr, "stud: linking: MessageBus.setRequestHandlerRaw missing\n");
        return false;
    }

    // openURL is a REQUEST, not a publish: the protocol exports
    // getOpenURLRequestId/getOpenURLResponseId, so the Lua side calls it
    // and waits for an answer. Subscribing with doSubscribeRaw (the
    // web-view protocol's shape) registers nothing it will ever call,
    // which is why the buttons stayed silent even once "subscribed".
    const std::string url_key = ids.url_key;
    const std::string success_key = ids.success_key;

    auto register_handler = [&](const std::string& method,
                                std::function<std::string(const std::string&)> fn) {
        auto handler = std::make_shared<MessageBusRequestHandlerRawStub>();
        handler->handler = std::move(fn);
        auto bus = std::make_shared<MessageBusStub>();
        const bool ok = call_trapping_abort(
            set_request_handler, jni_env, env.createLocalReference(bus),
            env.NewStringUTF(ids.protocol.c_str()), env.NewStringUTF(method.c_str()),
            env.createLocalReference(handler));
        clear_pending_jni_exception(jni_env, "MessageBus.setRequestHandlerRaw");
        std::printf("stud: linking: request handler for %s.%s: %s\n", ids.protocol.c_str(),
                    method.c_str(), ok ? "ok" : "trapped");
        std::fflush(stdout);
        // The bus keeps calling these, so they must outlive this frame.
        static std::vector<std::shared_ptr<MessageBusRequestHandlerRawStub>> kept;
        kept.push_back(handler);
        return ok;
    };

    const bool ok = register_handler(
        ids.open_url_id, [url_key, success_key, on_open_url](const std::string& payload) {
            const std::string url = json_string_field(payload, url_key);
            if (url.empty()) {
                std::printf("stud: linking: openURL with no url field\n");
                std::fflush(stdout);
                return "{\"" + success_key + "\":false}";
            }
            const bool handled = on_open_url && on_open_url(url);
            std::printf("stud: linking: openURL %s -> %s\n", loggable_url(url).c_str(),
                        handled ? "handed to the desktop" : "not handled");
            std::fflush(stdout);
            // Answered honestly: a false here lets the Lua side fall back
            // rather than believe a link opened when it did not.
            return "{\"" + success_key + "\":" + (handled ? "true" : "false") + "}";
        });

    // "Is this URL registered to an app on this device?" -- answered
    // false, which is true of Stud: it registers no URL schemes of its
    // own. Left unanswered, a request handler the app expects can stall
    // whatever asked.
    const std::string is_registered_id = call_string_getter(env, lib, "getIsURLRegisteredId");
    const std::string registered_key = call_string_getter(env, lib, "getIsRegisteredKey");
    if (!is_registered_id.empty() && !registered_key.empty()) {
        register_handler(is_registered_id, [registered_key](const std::string&) {
            return "{\"" + registered_key + "\":false}";
        });
    }

    return ok;
}

}  // namespace stud::jni_bridge
