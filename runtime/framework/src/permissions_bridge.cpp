#include "stud/permissions_bridge.h"

#include "stud/game_activity_stubs.h"
#include "stud/trap_recovery.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace stud::jni_bridge {
namespace {

using SetRequestHandlerRawFn = void (*)(JNIEnv*, jobject, jstring, jstring, jobject);

// The app's own literals. No exported getters exist for this protocol --
// see the header.
constexpr const char* kProtocol = "PermissionsProtocol";
constexpr const char* kMicrophone = "MICROPHONE_ACCESS";

// The one permission Stud can honestly claim, and the shape of the
// answers, both read from the app's own implementation: a status plus the
// permissions that are still missing.
bool is_granted(const std::string& permission) { return permission == kMicrophone; }

// The engine sends {"permissions":["A","B"]}. Pulled out by hand rather
// than with a JSON library: this runs inside Process B, where the only
// parser available is the one the bridge already avoids pulling in, and
// the shape is one flat array of quoted strings.
std::vector<std::string> requested_permissions(const std::string& payload) {
    std::vector<std::string> out;
    const size_t key = payload.find("\"permissions\"");
    if (key == std::string::npos) return out;
    const size_t open = payload.find('[', key);
    const size_t close = payload.find(']', open == std::string::npos ? key : open);
    if (open == std::string::npos || close == std::string::npos) return out;
    size_t pos = open;
    while (true) {
        const size_t q1 = payload.find('"', pos);
        if (q1 == std::string::npos || q1 > close) break;
        const size_t q2 = payload.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > close) break;
        out.push_back(payload.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;
    }
    return out;
}

std::string json_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) out += ",";
        out += "\"" + items[i] + "\"";
    }
    out += "]";
    return out;
}

// AUTHORIZED when everything asked for is granted, ACCESS_DENIED
// otherwise -- the engine's own two words, from its own log strings.
std::string status_response(const std::string& payload) {
    std::vector<std::string> missing;
    for (const auto& permission : requested_permissions(payload)) {
        if (!is_granted(permission)) missing.push_back(permission);
    }
    const char* status = missing.empty() ? "AUTHORIZED" : "ACCESS_DENIED";
    return std::string("{\"status\":\"") + status + "\",\"missingPermissions\":" +
           json_array(missing) + "}";
}

// What this platform has a permission model for at all. Only the
// microphone: a desktop has no runtime permission for the rest, and Stud
// has no camera, contacts, media store or notification permission.
std::string supports_response(const std::string&) {
    return std::string("{\"permissions\":[\"") + kMicrophone + "\"]}";
}

// No upsell, ever. The upsell is Android's "you dismissed this, here is
// why we need it" screen, which needs a permission dialog to send the
// user to -- and there is none here.
std::string upsell_response(const std::string&) {
    return "{\"upsellStatus\":\"HIDE_UPSELL\",\"hiddenUpsellPermissions\":[]}";
}

}  // namespace

bool run_permissions_bridge(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib) {
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    auto* set_request_handler = reinterpret_cast<SetRequestHandlerRawFn>(lib.find_symbol(
        "Java_com_roblox_universalapp_messagebus_MessageBus_setRequestHandlerRaw"));
    if (set_request_handler == nullptr) {
        std::fprintf(stderr, "stud: permissions: MessageBus.setRequestHandlerRaw missing\n");
        return false;
    }

    // The handlers outlive this frame: the bus keeps calling them for the
    // life of the process.
    static std::vector<std::shared_ptr<MessageBusRequestHandlerRawStub>> kept;

    auto answer = [&](const char* method, std::string (*reply)(const std::string&)) {
        auto handler = std::make_shared<MessageBusRequestHandlerRawStub>();
        handler->handler = [reply](const std::string& payload) { return reply(payload); };
        auto bus = std::make_shared<MessageBusStub>();
        const bool ok = call_trapping_abort(set_request_handler, jni_env,
                                            env.createLocalReference(bus),
                                            env.NewStringUTF(kProtocol),
                                            env.NewStringUTF(method),
                                            env.createLocalReference(handler));
        clear_pending_jni_exception(jni_env, "MessageBus.setRequestHandlerRaw");
        kept.push_back(handler);
        std::printf("stud: permissions: %s handler registered: %s\n", method,
                    ok ? "ok" : "trapped");
        std::fflush(stdout);
        return ok;
    };

    bool all = true;
    // Both of these answer with a status: one asks, one demands. A
    // desktop cannot show a permission dialog, so the demand is answered
    // with the same truth as the question.
    all &= answer("HasPermissions", &status_response);
    all &= answer("PermissionsRequest", &status_response);
    all &= answer("SupportsPermissions", &supports_response);
    all &= answer("ShouldShowPermissionUpsell", &upsell_response);
    all &= answer("ShouldShowRequestPermissionRationale", &upsell_response);
    return all;
}

}  // namespace stud::jni_bridge
