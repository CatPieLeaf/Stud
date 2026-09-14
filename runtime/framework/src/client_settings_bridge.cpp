#include "stud/client_settings_bridge.h"

#include <cstdlib>

#include "stud/game_activity_stubs.h"
#include "stud/trap_recovery.h"

#include <cstdio>

namespace stud::jni_bridge {

const char* client_settings_group() {
    static const char* group = [] {
        const char* v = std::getenv("STUD_CLIENT_SETTINGS_GROUP");
        return (v != nullptr && *v != '\0') ? v : "PCDesktopClient";
    }();
    return group;
}


namespace {

using InitClientSettingsFn = jint (*)(JNIEnv*, jclass, jstring, jstring, jstring);
using PostInitFn = void (*)(JNIEnv*, jclass, jobject);

}  // namespace

ClientSettingsBridgeResult run_client_settings_bridge(FakeJni::Jvm& jvm,
                                                       const stud::linker::LoadedLibrary& lib,
                                                       const std::string& body, long http_status) {
    ClientSettingsBridgeResult result;
    result.http_status_code = http_status;
    result.http_fetch_succeeded = (http_status == 200);
    if (!result.http_fetch_succeeded) {
        std::fprintf(stderr,
                     "stud: client_settings_bridge: pre-fetched HTTP status was %ld, not calling "
                     "nativeInitClientSettings\n",
                     http_status);
        return result;
    }
    std::printf("stud: client_settings_bridge: using pre-fetched content, %zu bytes\n", body.size());

    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    void* init_addr =
        lib.find_symbol("Java_com_roblox_engine_jni_NativeGLInterface_nativeInitClientSettings");
    if (init_addr == nullptr) {
        throw stud::linker::LoadError(
            "client_settings_bridge: required symbol not found: "
            "Java_com_roblox_engine_jni_NativeGLInterface_nativeInitClientSettings");
    }

    jstring data_ref = env.NewStringUTF(body.c_str());
    jstring overrides_ref = env.NewStringUTF("{}");
    jstring group_name_ref = env.NewStringUTF(client_settings_group());
    auto* init_fn = reinterpret_cast<InitClientSettingsFn>(init_addr);

    result.init_client_settings_called = true;
    jint init_result = -1;
    result.init_client_settings_trapped_abort = !call_trapping_abort_with_result(
        init_fn, init_result, jni_env, nullptr, data_ref, overrides_ref, group_name_ref);
    result.init_client_settings_result = init_result;
    clear_pending_jni_exception(jni_env, "nativeInitClientSettings");

    // Real sequence, confirmed via Sober's own working logs: only on
    // success (result == 0, matching the app's own app-shell manager's own real
    // `f.onPostExecute()` check) does a real device call
    // nativePostClientSettingsLoadedInitialization3 next.
    if (!result.init_client_settings_trapped_abort && init_result == 0) {
        void* post_addr = lib.find_symbol(
            "Java_com_roblox_engine_jni_NativeGLInterface_"
            "nativePostClientSettingsLoadedInitialization3");
        if (post_addr != nullptr) {
            auto* post_fn = reinterpret_cast<PostInitFn>(post_addr);
            result.post_init_called = true;
            // Real bug found and fixed (the engineering notes): passing raw
            // null here trapped, a real device always passes a real
            // (possibly empty) List<ApplicationExitInfoCpp>
            // (the app's own exit-info list), never null. A real, empty
            // ArrayList (no fabricated ApplicationExitInfo content,
            // just an honest empty container) matches that.
            jobject empty_list_ref =
                env.createLocalReference(std::make_shared<EmptyArrayListStub>());
            result.post_init_trapped_abort =
                !call_trapping_abort(post_fn, jni_env, nullptr, empty_list_ref);
            clear_pending_jni_exception(jni_env, "nativePostClientSettingsLoadedInitialization3");
        }
    }

    return result;
}

}  // namespace stud::jni_bridge
