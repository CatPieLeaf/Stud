#include "stud/linking_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/app_java_classes.h"
#include "stud/trap_recovery.h"

#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
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
        std::fprintf(stderr, "stud: linking: protocol identifiers unavailable, not registering\n");
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
        auto handler = std::make_shared<MessageBusRequestHandlerRawJava>();
        handler->handler = std::move(fn);
        auto bus = std::make_shared<MessageBusJava>();
        const bool ok = call_trapping_abort(
            set_request_handler, jni_env, env.createLocalReference(bus),
            env.NewStringUTF(ids.protocol.c_str()), env.NewStringUTF(method.c_str()),
            env.createLocalReference(handler));
        clear_pending_jni_exception(jni_env, "MessageBus.setRequestHandlerRaw");
        std::printf("stud: linking: request handler for %s.%s: %s\n", ids.protocol.c_str(),
                    method.c_str(), ok ? "ok" : "trapped");
        std::fflush(stdout);
        // The bus keeps calling these, so they must outlive this frame.
        static std::vector<std::shared_ptr<MessageBusRequestHandlerRawJava>> kept;
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

    // "Is this URL registered to an app on this device?", answered
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

namespace {

using PublishRawFn = void (*)(JNIEnv*, jobject, jstring, jstring);

// URLs asked about and not yet answered for, oldest first.
//
// The engine's answer carries an empty `matchedUrl` (live-measured), so
// the only way to know which URL it is about is to remember the order
// they were asked in. The bus delivers in order, so the oldest pending
// one is the answer's own.
std::mutex& pending_mutex() {
    static std::mutex m;
    return m;
}
std::deque<std::string>& pending_urls() {
    static std::deque<std::string> urls;
    return urls;
}

// Read once: every getter is a real JNI call into the engine, and this
// runs on every navigation the web view attempts.
LinkingUrlIds& cached_url_ids() {
    static LinkingUrlIds ids;
    return ids;
}

std::string json_escaped(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Some of this protocol's own getters return an id that is ALREADY
// qualified ("Linking.detectURL") while others return a bare method
// name ("openURL"), so running every one through MessageBus.getMessageId
// produces "Linking.Linking.detectURL" for half of them, a topic
// nothing publishes to and nothing hears. Live-caught exactly that way:
// the subscription reported ok and never fired.
std::string topic_for(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                      const std::string& protocol, const std::string& id) {
    if (id.rfind(protocol + ".", 0) == 0) return id;
    return message_id(env, lib, protocol, id);
}

void publish_url(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                 const std::string& method, const std::string& url, const char* what) {
    const LinkingUrlIds& ids = cached_url_ids();
    if (!ids.resolved || method.empty() || url.empty()) return;

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* publish = reinterpret_cast<PublishRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw"));
    if (publish == nullptr) return;
    const std::string topic = topic_for(env, lib, ids.protocol, method);
    if (topic.empty()) return;

    const std::string payload = "{\"" + ids.url_key + "\":\"" + json_escaped(url) + "\"}";
    auto bus = std::make_shared<MessageBusJava>();
    const bool ok = call_trapping_abort(publish, jni_env, env.createLocalReference(bus),
                                        env.NewStringUTF(topic.c_str()),
                                        env.NewStringUTF(payload.c_str()));
    clear_pending_jni_exception(jni_env, "MessageBus.publishRaw");
    std::printf("stud: linking: %s %s: %s\n", what, loggable_url(url).c_str(),
                ok ? "sent" : "trapped");
    std::fflush(stdout);
}

}  // namespace

bool run_linking_url_detection_bootstrap(
    FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
    std::function<void(const std::string& url, bool registered)> on_answer) {
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    LinkingUrlIds& ids = cached_url_ids();
    ids.protocol = call_string_getter(env, lib, "getProtocolName");
    ids.url_key = call_string_getter(env, lib, "getUrlKey");
    ids.detect_url_id = call_string_getter(env, lib, "getDetectURLId");
    ids.is_url_registered_request_id = call_string_getter(env, lib, "getIsURLRegisteredRequestId");
    ids.is_url_registered_response_id = call_string_getter(env, lib, "getIsURLRegisteredResponseId");
    ids.is_registered_key = call_string_getter(env, lib, "getIsRegisteredKey");
    ids.matched_url_key = call_string_getter(env, lib, "getMatchedUrlKey");
    ids.resolved = !ids.protocol.empty() && !ids.url_key.empty() && !ids.detect_url_id.empty() &&
                   !ids.is_url_registered_request_id.empty() &&
                   !ids.is_url_registered_response_id.empty();
    std::printf("stud: linking: urlKey=\"%s\" detectURL=\"%s\" isURLRegisteredRequest=\"%s\" "
                "isURLRegisteredResponse=\"%s\" isRegisteredKey=\"%s\" matchedUrlKey=\"%s\"\n",
                ids.url_key.c_str(), ids.detect_url_id.c_str(),
                ids.is_url_registered_request_id.c_str(),
                ids.is_url_registered_response_id.c_str(), ids.is_registered_key.c_str(),
                ids.matched_url_key.c_str());
    std::fflush(stdout);
    if (!ids.resolved) {
        std::fprintf(stderr, "stud: linking: URL-detection identifiers unavailable\n");
        return false;
    }

    auto* do_subscribe = reinterpret_cast<DoSubscribeRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw"));
    if (do_subscribe == nullptr) return false;

    const std::string topic = topic_for(env, lib, ids.protocol, ids.is_url_registered_response_id);
    if (topic.empty()) return false;

    const std::string registered_key = ids.is_registered_key;
    const std::string matched_url_key = ids.matched_url_key;
    auto callback = std::make_shared<MessageBusRawCallbackJava>();
    callback->handler = [registered_key, matched_url_key,
                         on_answer](const std::string& payload) {
        // The engine answers with the URL it matched, so no request has
        // to be correlated by hand.
        std::string url = json_string_field(payload, matched_url_key);
        if (url.empty()) {
            std::lock_guard<std::mutex> lock(pending_mutex());
            if (!pending_urls().empty()) {
                url = pending_urls().front();
                pending_urls().pop_front();
            }
        } else {
            std::lock_guard<std::mutex> lock(pending_mutex());
            for (auto it = pending_urls().begin(); it != pending_urls().end(); ++it) {
                if (*it == url) {
                    pending_urls().erase(it);
                    break;
                }
            }
        }
        const bool registered =
            payload.find("\"" + registered_key + "\":true") != std::string::npos;
        std::printf("stud: linking: the engine %s %s\n",
                    registered ? "claims" : "does not claim", loggable_url(url).c_str());
        std::fflush(stdout);
        if (on_answer) on_answer(url, registered);
    };
    auto bus = std::make_shared<MessageBusJava>();
    jobject connection = nullptr;
    const bool ok = call_trapping_abort_with_result(
        do_subscribe, connection, jni_env, env.createLocalReference(bus),
        env.NewStringUTF(topic.c_str()), env.createLocalReference(callback),
        static_cast<jboolean>(JNI_FALSE));
    clear_pending_jni_exception(jni_env, "MessageBus.doSubscribeRaw");
    std::printf("stud: linking: subscribed to %s: %s\n", topic.c_str(), ok ? "ok" : "trapped");
    std::fflush(stdout);
    // The bus keeps calling it, so it outlives this frame.
    static std::vector<std::shared_ptr<MessageBusRawCallbackJava>> kept;
    kept.push_back(callback);
    return ok;
}

void ask_engine_about_url(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                          const std::string& url) {
    {
        std::lock_guard<std::mutex> lock(pending_mutex());
        if (pending_urls().size() >= 16) pending_urls().pop_front();
        pending_urls().push_back(url);
    }
    publish_url(jvm, lib, cached_url_ids().is_url_registered_request_id, url, "asking about");
}

void hand_url_to_engine(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                        const std::string& url) {
    publish_url(jvm, lib, cached_url_ids().detect_url_id, url, "handing over");
}

}  // namespace stud::jni_bridge
