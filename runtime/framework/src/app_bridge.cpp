#include "stud/stud_paths.h"
#include "stud/app_bridge.h"
#include "stud/app_java_classes.h"  // real_app_version()
#include "stud/atomic_file.h"

#include "stud/trap_recovery.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

namespace stud::jni_bridge {

namespace {

using AppBridgeAppStartFn = void (*)(JNIEnv*, jclass, jstring, jstring, jboolean, jstring, jstring,
                                      jstring);

// Real Settings.Secure.ANDROID_ID format: a 64-bit value, rendered as 16
// lowercase hex characters. Stud has no real Android device to read one
// from, so this generates a real-format, persistent-per-install value
// once and caches it, honest in the sense that matters here (real
// format, stable across launches, not silently regenerated every run,
// which native code receiving a *different* ID on every call would be a
// much stranger thing to hand it than a stable synthetic one) rather
// than the empty string this project passed before, which is not a real
// Android ID a device would ever actually send.
// The device identity belongs with the data, not the cache: it is meant
// to be the same machine across launches, and it used to sit in
// ~/.cache/stud, where any cache clean turned Stud into a different
// device as far as Roblox is concerned.
std::string dir_for_android_id() { return stud::paths::data_dir(); }

std::string real_persistent_android_id() {
    std::string path = dir_for_android_id() + "/android_id";
    std::ifstream in(path);
    std::string existing;
    if (in >> existing && existing.size() == 16) {
        return existing;
    }
    // Two draws: random_device gives 32 bits, and one of them as a seed would
    // leave only 2^32 possible ids.
    std::random_device rd;
    const uint64_t value = (static_cast<uint64_t>(rd()) << 32) | rd();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    std::string generated(buf);
    std::error_code ec;
    std::filesystem::create_directories(dir_for_android_id(), ec);
    // Beside it and renamed over, so a crash cannot leave half an id that the
    // next launch would replace with a different device.
    write_file_atomically(path, generated);
    return generated;
}

}  // namespace


// The shape the app's own User-Agent builder produces:
//   "Mozilla/5.0 (%dMB; %dx%d; %dx%d; %dx%d; %s; %s) %s (KHTML, like "
//   "Gecko)  %s %s %s Hybrid() %s"
// memory, three screen pairs, device name, Android release,
// "AppleWebKit/537.36", client token, Roblox version, form factor, then a
// nested "%s RobloxApp/%s (%s; %s)" of store, version, distributor and store.
// One builder, two callers. `android_app_token` picks the client token
// the agent carries: the engine's own HTTP wants a desktop-class client
// (see the long note further down), while the app's WEB VIEW is
// genuinely the Android app's web view and must say so; roblox.com
// classifies the request from this string and serves the hybrid page
// accordingly.
std::string build_user_agent(bool android_app_token) {
    // STUD_USER_AGENT overrides the whole string.
    //
    // The agent is not just device analytics: it decides which
    // experiences Roblox lists. Observed directly, with the old
    // placeholder "Stud/0.1" the catalogue included PC-only titles but
    // the join was rejected as invalid; with a real Android agent the
    // join works and those titles disappear. So the two behaviours are
    // both agent-driven, and finding one that satisfies both is an
    // experiment, not a deduction. This makes trying a candidate a
    // relaunch instead of a rebuild.
    //
    // The real Windows desktop client sends "Roblox/WinInet" (confirmed
    // by that literal string in libroblox.so, alongside a platform-name
    // list that includes Linux as well as Windows and Android).
    if (const char* override_agent = std::getenv("STUD_USER_AGENT");
        override_agent != nullptr && *override_agent != '\0') {
        static bool announced_override = false;
        if (!announced_override) {
            announced_override = true;
            std::printf("stud: User-Agent (STUD_USER_AGENT override): %s\n", override_agent);
            std::fflush(stdout);
        }
        return override_agent;
    }

    // The default is an Android device envelope carrying a desktop client
    // token. Neither half works alone: a bare token gets no account switcher,
    // and the full Android agent gets no PC-targeted experiences.
    //
    // STUD_ANDROID_USER_AGENT=1 selects the pure-Android variant ("ROBLOX
    // Android App" and a trailing " ChromeOS"), a real client string kept as
    // a useful comparison.
    static const bool env_android_app_token = [] {
        const char* v = std::getenv("STUD_ANDROID_USER_AGENT");
        return v != nullptr && std::string_view(v) == "1";
    }();
    const bool use_android_app_token = android_app_token || env_android_app_token;

    // Every number here is measured from the machine Stud was launched
    // on. Nothing in this agent is a literal.
    //
    // All three pairs describe the SCREEN, never Stud's own window. A
    // device agent says what display the client is running on; the
    // window is a detail of this session and changes when the user
    // resizes it, which would make the same machine report a different
    // device from one launch to the next.
    //
    // What the three pairs are, and where this deviates from the
    // builder (the app's own User-Agent builder):
    //   1st  Display.getSize(), the display in pixels
    //   2nd  xdpi/ydpi in the real builder
    //   3rd  widthPixels/density, density-independent size
    //
    // The 2nd really is a dpi, not a third resolution. The honest value is
    // the display's real dot pitch, and it is computed, not assumed: the
    // compositor reports
    // the panel in both pixels and millimetres, which is exactly the
    // division Android's own xdpi/ydpi are.
    const DisplayFacts display = real_display_facts();
    // The compositor's own output is the screen. Falling back to the
    // window is falling back to another real measurement, not to a
    // constant, there is no number here to invent if neither exists.
    const int screen_w = display.output_width_px > 0 ? display.output_width_px : display.width_px;
    const int screen_h = display.output_height_px > 0 ? display.output_height_px : display.height_px;
    // The measured density, not the one DisplayMetrics was seeded with
    // (that one is deliberately 1.0; see set_measured_display_density).
    const float density = display.measured_density > 0.0f  ? display.measured_density
                          : (display.density > 0.0f ? display.density : 1.0f);
    // Truncated, not rounded, matching the real builder's own `(int)` cast.
    // real_display_facts() derives these from the compositor's reported
    // pixel and millimetre size; when it reports no physical size there
    // is no honest dot pitch to state, and it falls back to 160 * density
    // which is the definition of density, not a guessed number.
    const int dpi_x = static_cast<int>(display.xdpi);
    const int dpi_y = static_cast<int>(display.ydpi);
    const int dip_w = static_cast<int>(static_cast<float>(screen_w) / density);
    const int dip_h = static_cast<int>(static_cast<float>(screen_h) / density);

    // Total system memory in MB, from ActivityManager.
    const int memory_mb = real_total_memory_mb();

    // MANUFACTURER + " " + MODEL. Deliberately NOT this
    // machine's DMI model. The server does not check this field, so
    // putting a real model number here would identify the user's exact
    // hardware to no functional end. "Linux PC" is true of every
    // machine Stud runs on and adds no entropy.
    const char* device_name = "Linux PC";

    // The one source of truth for it, rather than a second copy that can
    // drift from what every other caller is told.
    const std::string android_release = BuildVersionJava::RELEASE->asStdString();
    // Read from the APK the user configured. It is deliberately NOT
    // defaulted to some build this project once saw: claiming a version
    // Stud is not running is worse than saying nothing, and an empty one
    // here means the version never reached this process, which is a real
    // bug worth seeing rather than papering over.
    const std::string& roblox_version_str = real_app_version();
    if (roblox_version_str.empty()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::printf("stud: the app version is empty, the User-Agent will say so\n");
            std::fflush(stdout);
        }
    }
    const char* roblox_version = roblox_version_str.c_str();
    // Real constants from com.roblox.client.personasdk.BuildConfig.
    const char* distributor_type = "GlobalDist";
    const char* store_type = "GooglePlayStore";
    // The app's own User-Agent builder: VR, then Phone (isTablet false), then TV, else
    // Tablet. The app derives this from the same isTablet the params
    // carry, so it follows that flag rather than being chosen here,
    // reporting one and sending the other is the inconsistency this
    // used to have.
    const char* form_factor = "Tablet";

    char nested[160];
    std::snprintf(nested, sizeof(nested), "%s RobloxApp/%s (%s; %s)", store_type, roblox_version,
                  distributor_type, store_type);

    // The client token: "Roblox/WinInet" by default, the literal in
    // libroblox.so, or the mobile client's "ROBLOX Android App"
    // (STUD_ANDROID_USER_AGENT=1). It's the one field here that isn't
    // literally true. "Roblox/Linux" is not in the binary, and comparing
    // tokens properly needs repeated runs that each start from a cleared
    // ~/.cache/stud/cache, because the engine's cache carries state over.
    const char* client_token = use_android_app_token ? "ROBLOX Android App" : "Roblox/WinInet";

    char full[640];
    std::snprintf(full, sizeof(full),
                  "Mozilla/5.0 (%dMB; %dx%d; %dx%d; %dx%d; %s; %s) %s (KHTML, like Gecko)  "
                  "%s %s %s Hybrid() %s",
                  memory_mb, screen_w, screen_h, dpi_x, dpi_y, dip_w, dip_h, device_name,
                  android_release.c_str(), "AppleWebKit/537.36", client_token, roblox_version, form_factor,
                  nested);

    // " ChromeOS" belongs to the pure-Android variant only (the app's own
    // HTTP/cookie layer appends it when the app sees the ARC system feature).
    const std::string agent =
        use_android_app_token ? std::string(full) + " ChromeOS" : std::string(full);
    // Printed once: this string decides how Roblox classifies the client
    // (it is what the join API reads), so it is worth being able to see
    // exactly what was sent rather than inferring it.
    static bool announced_engine = false;
    static bool announced_web_view = false;
    bool& announced = use_android_app_token ? announced_web_view : announced_engine;
    if (!announced) {
        announced = true;
        std::printf("stud: User-Agent%s: %s\n", use_android_app_token ? " (web view)" : "",
                    agent.c_str());
        std::fflush(stdout);
    }
    return agent;
}

std::string build_real_user_agent() { return build_user_agent(/*android_app_token=*/false); }

// What Stud's own web-view viewer sends. The real app's WebView uses the
// app's own agent (built by the same builder as everything else), and that agent carries "ROBLOX Android App", the
// literal in the app's own User-Agent builder's own format string.
//
// This is load-bearing, not cosmetic. roblox.com decides from the
// request's agent whether the page is running inside the Android app,
// and says so in the page itself as data-is-android-app. Measured
// against the live challenge page: every desktop-token agent gets
// "false", "ROBLOX Android App" gets "true". With "false" the challenge
// page never enables its native transport, so a completed OTP is
// announced to nobody, live-observed as "Sending hybrid call:
// challengeCompleted to origin: undefined" and a login that never
// finishes.
std::string build_web_view_user_agent() { return build_user_agent(/*android_app_token=*/true); }

AppBridgeResult run_app_bridge_start(FakeJni::Jvm& jvm, const stud::linker::LoadedLibrary& lib,
                                      const std::string& base_url) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    void* addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeAppBridgeInterface_nativeAppBridgeAppStart__"
        "Ljava_lang_String_2Ljava_lang_String_2ZLjava_lang_String_2Ljava_lang_String_2Ljava_"
        "lang_String_2");
    if (addr == nullptr) {
        throw stud::linker::LoadError(
            "app_bridge: required symbol not found: "
            "Java_com_roblox_engine_jni_NativeAppBridgeInterface_nativeAppBridgeAppStart");
    }

    // See app_bridge.h for what each argument is.
    std::string real_base_url = base_url.empty() ? "https://www.roblox.com" : base_url;
    jstring base_url_ref = env.NewStringUTF(real_base_url.c_str());
    std::string user_agent = build_real_user_agent();
    jstring user_agent_ref = env.NewStringUTF(user_agent.c_str());
    jstring android_id_ref = env.NewStringUTF(real_persistent_android_id().c_str());
    // Confirmed constant (com/roblox/client/personasdk/
    // BuildConfig in the app itself), not
    // guessed, but a real per-package build value that could go stale
    // on a future Roblox release, same category of risk as the FFlag
    // names already hardcoded elsewhere in this project.
    jstring app_upgrade_key_ref = env.NewStringUTF("AppAndroidV");
    jstring empty = env.NewStringUTF("");
    auto* fn = reinterpret_cast<AppBridgeAppStartFn>(addr);

    AppBridgeResult result;
    result.app_start_called = true;
    result.app_start_trapped_abort =
        !call_trapping_abort(fn, jni_env, nullptr, base_url_ref, user_agent_ref,
                              static_cast<jboolean>(JNI_FALSE), android_id_ref, app_upgrade_key_ref,
                              empty);
    clear_pending_jni_exception(jni_env, "nativeAppBridgeAppStart");
    return result;
}

}  // namespace stud::jni_bridge
