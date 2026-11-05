#include "stud/webview_bridge.h"

#include "stud/bionic_jvm.h"
#include "stud/hybrid_command.h"
#include "stud/json_field.h"
#include "stud/web_origin.h"
#include "stud/webview_cookies.h"
#include "stud/app_java_classes.h"
#include "stud/trap_recovery.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>

namespace stud::jni_bridge {

namespace {

using GetStringFn = jstring (*)(JNIEnv*, jclass);
using GetMessageIdFn = jstring (*)(JNIEnv*, jclass, jstring, jstring);
using InitFn = void (*)(JNIEnv*, jclass);
using DoSubscribeRawFn = jobject (*)(JNIEnv*, jobject, jstring, jobject, jboolean);
using SetRequestHandlerRawFn = void (*)(JNIEnv*, jobject, jstring, jstring, jobject);
using PublishRawFn = void (*)(JNIEnv*, jobject, jstring, jstring);

constexpr const char* kProtocolClass = "com/roblox/protocols/webview/WebViewProtocol";
constexpr const char* kSymbolPrefix = "Java_com_roblox_protocols_webview_WebViewProtocol_";

std::string read_jstring(FakeJni::Env& env, jstring value) {
    if (value == nullptr) return {};
    auto resolved = env.resolveReference(value);
    auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved);
    return as_string ? as_string->asStdString() : std::string();
}

// Every identifier in this protocol has its own exported getter, so the
// wire names are read from the engine rather than written down here.
std::string call_string_getter(FakeJni::Env& env, const stud::linker::LoadedLibrary& lib,
                               const char* method) {
    const std::string symbol = std::string(kSymbolPrefix) + method;
    auto* fn = reinterpret_cast<GetStringFn>(lib.find_symbol(symbol.c_str()));
    if (fn == nullptr) {
        std::fprintf(stderr, "stud: webview: %s not exported\n", symbol.c_str());
        return {};
    }
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jclass cls = env.FindClass(kProtocolClass);
    jstring result = nullptr;
    if (!call_trapping_abort_with_result(fn, result, jni_env, cls)) {
        std::fprintf(stderr, "stud: webview: %s trapped\n", method);
        return {};
    }
    clear_pending_jni_exception(jni_env, method);
    return read_jstring(env, result);
}

// A URL can carry a one-time authentication ticket in its query, so only
// the part that identifies the page is ever logged.
std::string loggable_url(const std::string& url) {
    const auto cut = url.find('?');
    return cut == std::string::npos ? url : url.substr(0, cut) + "?<query withheld>";
}

// How a panel is opened, kept from bring-up for the engine's security check.
std::function<void(const std::string&, const std::string&)>& panel_opener() {
    static std::function<void(const std::string&, const std::string&)> opener;
    return opener;
}
std::atomic<bool> g_security_check_open{false};
std::atomic<bool> g_login_open{false};
std::atomic<bool> g_login_prepared{false};
std::function<void(const std::string&)>& login_page_shower() {
    static std::function<void(const std::string&)> shower;
    return shower;
}
std::function<void()>& panel_closer() {
    static std::function<void()> closer;
    return closer;
}
std::atomic<bool> g_web_login_enabled{true};

std::mutex& ids_mutex() {
    static std::mutex m;
    return m;
}

WebViewProtocolIds& cached_ids() {
    static WebViewProtocolIds ids;
    return ids;
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

}  // namespace

WebViewProtocolIds read_webview_protocol_ids(FakeJni::Jvm& jvm,
                                             const stud::linker::LoadedLibrary& lib) {
    {
        std::lock_guard<std::mutex> lock(ids_mutex());
        if (cached_ids().valid) return cached_ids();
    }
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();

    WebViewProtocolIds ids;
    ids.protocol = call_string_getter(env, lib, "getProtocolName");
    ids.open_window_id = call_string_getter(env, lib, "getOpenWindowId");
    ids.mutate_window_id = call_string_getter(env, lib, "getMutateWindowId");
    ids.close_window_id = call_string_getter(env, lib, "getCloseWindowId");
    ids.is_available_id = call_string_getter(env, lib, "getIsAvailableId");
    ids.handle_window_close_id = call_string_getter(env, lib, "getHandleWindowCloseId");
    ids.url_key = call_string_getter(env, lib, "getUrlKey");
    ids.title_key = call_string_getter(env, lib, "getTitleKey");
    // The app says itself which kind of window it wants. Guessing from
    // the domain does not work: blog.roblox.com is an EXTERNAL link that
    // belongs in a browser, and it ends in .roblox.com like every
    // in-app panel does.
    ids.window_type_key = call_string_getter(env, lib, "getWindowTypeKey");
    ids.available_key = call_string_getter(env, lib, "getAvailableKey");
    ids.valid = !ids.protocol.empty() && !ids.open_window_id.empty();

    std::lock_guard<std::mutex> lock(ids_mutex());
    cached_ids() = ids;
    return ids;
}

bool run_webview_protocol_bootstrap(
    FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
    std::function<void(const std::string& url, const std::string& title)> on_open,
    std::function<void()> on_close) {
    panel_opener() = on_open;
    panel_closer() = on_close;
    const WebViewProtocolIds ids = read_webview_protocol_ids(jvm, lib);
    if (!ids.valid) {
        std::fprintf(stderr, "stud: webview: protocol identifiers unavailable, not registering\n");
        return false;
    }

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* do_subscribe = reinterpret_cast<DoSubscribeRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_doSubscribeRaw"));
    auto* set_request_handler = reinterpret_cast<SetRequestHandlerRawFn>(lib.find_symbol(
        "Java_com_roblox_universalapp_messagebus_MessageBus_setRequestHandlerRaw"));
    auto* init = reinterpret_cast<InitFn>(
        lib.find_symbol(std::string(kSymbolPrefix) + "initializeAndroidWebViewProtocol"));
    if (do_subscribe == nullptr || set_request_handler == nullptr) {
        std::fprintf(stderr, "stud: webview: MessageBus entry points missing\n");
        return false;
    }

    // Answer "can this platform show a web view". Without this the Lua
    // app has no reason to send an open request at all.
    {
        auto handler = std::make_shared<MessageBusRequestHandlerRawJava>();
        const std::string available_key = ids.available_key;
        handler->handler = [available_key](const std::string&) {
            return "{\"" + available_key + "\":true}";
        };
        auto bus = std::make_shared<MessageBusJava>();
        const bool ok = call_trapping_abort(
            set_request_handler, jni_env, env.createLocalReference(bus),
            env.NewStringUTF(ids.protocol.c_str()),
            env.NewStringUTF(ids.is_available_id.c_str()), env.createLocalReference(handler));
        clear_pending_jni_exception(jni_env, "MessageBus.setRequestHandlerRaw");
        std::printf("stud: webview: availability handler registered: %s\n", ok ? "ok" : "trapped");
        // The callback object has to outlive this frame, the bus keeps calling it.
        static std::shared_ptr<MessageBusRequestHandlerRawJava> kept_handler;
        kept_handler = handler;
    }

    auto subscribe = [&](const std::string& method,
                         std::function<void(const std::string&)> on_payload) {
        const std::string topic = message_id(env, lib, ids.protocol, method);
        if (topic.empty()) {
            std::fprintf(stderr, "stud: webview: no message id for %s\n", method.c_str());
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
        std::printf("stud: webview: subscribed to %s: %s\n", topic.c_str(),
                    ok ? "ok" : "trapped");
        // Same lifetime point as the request handler above.
        static std::vector<std::shared_ptr<MessageBusRawCallbackJava>> kept;
        kept.push_back(callback);
    };

    const std::string url_key = ids.url_key;
    const std::string title_key = ids.title_key;
    const std::string window_type_key = ids.window_type_key;
    subscribe(ids.open_window_id, [url_key, title_key, window_type_key,
                                    on_open](const std::string& payload) {
        const std::string url = json_string_field(payload, url_key);
        const std::string title = json_string_field(payload, title_key);
        const std::string window_type = json_string_field(payload, window_type_key);
        std::printf("stud: webview: windowType=\"%s\"\n", window_type.c_str());
        std::fflush(stdout);
        std::printf("stud: webview: open requested: title=\"%s\" url=%s\n", title.c_str(),
                    loggable_url(url).c_str());
        std::fflush(stdout);
        if (on_open && !url.empty()) on_open(url, title);
    });
    subscribe(ids.close_window_id, [on_close](const std::string&) {
        std::printf("stud: webview: close requested by the engine\n");
        std::fflush(stdout);
        if (on_close) on_close();
    });

    if (init != nullptr) {
        jclass cls = env.FindClass(kProtocolClass);
        const bool ok = call_trapping_abort(init, jni_env, cls);
        clear_pending_jni_exception(jni_env, "initializeAndroidWebViewProtocol");
        std::printf("stud: webview: initializeAndroidWebViewProtocol: %s\n",
                    ok ? "ok" : "trapped");
    }
    std::fflush(stdout);
    return true;
}

void open_security_check(const std::string& payload) {
    const std::string url = captcha_view_url(payload);
    if (url.empty() || !panel_opener()) {
        std::printf("stud: webview: could not open the security check\n");
        std::fflush(stdout);
        return;
    }
    g_security_check_open.store(true);
    panel_opener()(url, "Roblox verification");
}

void set_web_login_enabled(bool enabled) { g_web_login_enabled.store(enabled); }

void set_login_page_shower(std::function<void(const std::string&)> shower) {
    login_page_shower() = std::move(shower);
}

void prepare_login_page() {
    if (!g_web_login_enabled.load() || !panel_opener() || g_login_open.load() ||
        g_login_prepared.exchange(true)) {
        return;
    }
    panel_opener()(stud::web::kLoginPageUrl, "");
}

void discard_prepared_login() {
    if (g_login_prepared.exchange(false) && panel_closer()) panel_closer()();
}

void open_login_page(const char* url, const char* title) {
    // Landing and the form behind its button both ask, so one window answers.
    if (!g_web_login_enabled.load() || !panel_opener() || g_login_open.exchange(true)) return;
    if (g_login_prepared.exchange(false) && login_page_shower()) {
        login_page_shower()(url);
    } else {
        panel_opener()(url, title);
    }
}

void publish_webview_closed(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    g_login_open.store(false);
    g_login_prepared.store(false);
    if (!has_session_cookie()) prepare_login_page();
    if (g_security_check_open.exchange(false)) {
        static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
        FakeJni::LocalFrame frame(jvm);
        auto& env = frame.getJniEnv();
        auto* jni_env = static_cast<JNIEnv*>(&env);
        auto* broadcast = reinterpret_cast<void (*)(JNIEnv*, jclass, jstring, jstring, jstring)>(
            lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_"
                            "nativeBroadcastEventWithNamespace"));
        if (broadcast != nullptr) {
            jclass klass = env.FindClass("com/roblox/engine/jni/NativeGLInterface");
            call_trapping_abort(broadcast, jni_env, klass, env.NewStringUTF("AppInput"),
                                env.NewStringUTF(""), env.NewStringUTF("Focused"));
            clear_pending_jni_exception(jni_env, "nativeBroadcastEventWithNamespace");
        }
    }
    const WebViewProtocolIds ids = read_webview_protocol_ids(jvm, lib);
    if (!ids.valid || ids.handle_window_close_id.empty()) return;

    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* publish = reinterpret_cast<PublishRawFn>(
        lib.find_symbol("Java_com_roblox_universalapp_messagebus_MessageBus_publishRaw"));
    if (publish == nullptr) return;
    const std::string topic = message_id(env, lib, ids.protocol, ids.handle_window_close_id);
    if (topic.empty()) return;

    auto bus = std::make_shared<MessageBusJava>();
    const bool ok = call_trapping_abort(publish, jni_env, env.createLocalReference(bus),
                                        env.NewStringUTF(topic.c_str()),
                                        env.NewStringUTF("{}"));
    clear_pending_jni_exception(jni_env, "MessageBus.publishRaw");
    std::printf("stud: webview: told the app the window closed: %s\n", ok ? "ok" : "trapped");
    std::fflush(stdout);
}

void signal_webview_javascript(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                const std::string& message) {
    if (message.empty()) return;
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    // The real app's own path for this: WebViewProtocol ->
    // signalJavascriptCallback(String) ( the app's own web-view bridge hands the web
    // dialog's JavaScript listener straight to it). It is what finishes
    // a login challenge, the page completes the OTP or captcha, calls
    // the bridge, and this is the only way the engine hears about it.
    auto* signal = reinterpret_cast<void (*)(JNIEnv*, jclass, jstring)>(
        lib.find_symbol("Java_com_roblox_protocols_webview_WebViewProtocol_"
                        "signalJavascriptCallback"));
    if (signal == nullptr) {
        std::fprintf(stderr, "stud: webview: signalJavascriptCallback not found\n");
        return;
    }
    jclass klass = env.FindClass("com/roblox/protocols/webview/WebViewProtocol");
    const bool ok = call_trapping_abort(signal, jni_env, klass,
                                        env.NewStringUTF(message.c_str()));
    clear_pending_jni_exception(jni_env, "WebViewProtocol.signalJavascriptCallback");
    // Never the message itself: a login challenge's answer is a
    // credential in all but name.
    std::printf("stud: webview: passed a bridge message to the engine (%zu bytes): %s\n",
                message.size(), ok ? "ok" : "trapped");
    std::fflush(stdout);
}

void report_webview_user_agent(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                               const std::string& agent) {
    if (agent.empty()) return;
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    // The real app answers NativeGLJavaInterface.getWebViewUserAgent()
    // asynchronously, through this setter (
    // NativeHelper / ActivityNativeMain, both
    // NativeGLInterface.setWebviewUserAgent(...)). The engine reports
    // the answer to Roblox when it creates a login challenge, and the
    // page that answers that challenge runs in Stud's own viewer, so
    // the string has to be the one the viewer really sends, or the
    // challenge is created against a client that never shows up.
    auto* setter = reinterpret_cast<void (*)(JNIEnv*, jclass, jstring)>(
        lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_setWebviewUserAgent"));
    if (setter == nullptr) {
        std::fprintf(stderr, "stud: webview: setWebviewUserAgent not exported\n");
        return;
    }
    jclass klass = env.FindClass("com/roblox/engine/jni/NativeGLInterface");
    const bool ok = call_trapping_abort(setter, jni_env, klass,
                                        env.NewStringUTF(agent.c_str()));
    clear_pending_jni_exception(jni_env, "NativeGLInterface.setWebviewUserAgent");
    std::printf("stud: webview: answered the engine's user-agent request: %s\n",
                ok ? "ok" : "trapped");
    std::fflush(stdout);
}

}  // namespace stud::jni_bridge
