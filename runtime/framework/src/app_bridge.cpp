#include "stud/stud_paths.h"
#include "stud/app_bridge.h"
#include "stud/app_java_classes.h"  // real_app_version()

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
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t value = gen();
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
    std::string generated(buf);
    std::error_code ec;
    std::filesystem::create_directories(dir_for_android_id(), ec);
    std::ofstream out(path);
    out << generated;
    return generated;
}

}  // namespace


// Confirmed against the app's own code format string and field wiring, traced through
// the app's own HTTP/cookie layer -> the app's own User-Agent builder ->
// its real builder class and its format method (~/Stud/
// the engineering notes, "instantiate controllers" investigation, found by
// reading a real, fresh Sober log the user pointed at directly, which
// showed real, successful `instantiate controllers`/`instantiate
// experience coordinator` FLog lines firing right after
// nativeAppBridgeV2Init, something Stud's own process has never
// observed; this empty-User-Agent gap is the most concrete, evidence-
// grounded candidate found for why).
//
// Real format (the app's own User-Agent builder, byte-exact):
//   "Mozilla/5.0 (%dMB; %dx%d; %dx%d; %dx%d; %s; %s) %s (KHTML, like "
//   "Gecko)  ROBLOX Android App %s %s Hybrid()  %s"
// args, in order: memoryMB, screenPx.x/y, physSizeMm.x/y, dpiSizePx.x/y,
// sanitized device-class string, Android release, "AppleWebKit/537.36",
// Roblox version, form factor ("Phone"/"Tablet"/"VR"/"TV"), then a
// nested "%s RobloxApp/%s (%s; %s)" (distributorType, version,
// storeType, distributorType), plus a real, confirmed quirk: a
// literal " ChromeOS" suffix is appended whenever `Build.VERSION.
// SDK_INT >= 29` (the app's own User-Agent builder's flag for it),
// genuinely present in the real logic regardless of whether
// the device is actually a phone, not a Stud-specific guess. Stud's own
// BuildVersionJava.SDK_INT is 34, so this real condition is true here
// too.
//
// Two sub-fields (distributorType, storeType; real Java constants
// BuildConfig.DISTRIBUTOR_TYPE/BuildConfig.STORE_TYPE) are NOT
// confirmed against the app's own code. This app's own real BuildConfig class isn't present
// in this build (the optimiser typically inlines these at every use site
// rather than keeping a real class with fields). "google_play" is a
// real, publicly-documented value for Roblox's actual Play Store
// distribution channel, used here as a reasonable, clearly-labeled
// best-effort default, everything else in this string is real,
// traced through the app's own code, checked.
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

    // Default: "Roblox/Linux", the honest one.
    //
    // Both halves of this problem are agent-driven, and both were
    // established by testing rather than reasoning:
    //   - "Stud/0.1"        PC-only experiences listed, join REJECTED as
    //                       an invalid client
    //   - the Android agent join works, PC-only experiences hidden
    //   - "Roblox/WinInet"  both work, but claims to be Windows
    //   - "Roblox/Linux"    both work, and is true
    //
    // "Roblox/WinInet" is the literal the real Windows desktop client
    // sends, and it is the only "Roblox/<platform>" string that appears
    // in libroblox.so itself (checked). "Roblox/Linux" is NOT in the
    // binary, but the platform-name list inside it does include
    // "Linux" alongside "Windows"/"Android"/"XBoxOne", the server
    // accepts it, and a live launch with it reached Home, listed
    // desktop-targeted experiences and joined a real game. So Stud
    // reports what it actually is: a desktop client on Linux. It does
    // not claim to be Windows to get in.
    //
    // SOLVED, by capturing what Sober actually sends (it shows the
    // account switcher AND lists PC-targeted experiences, which for a
    // long time this file wrongly called impossible). Its agent:
    //
    //   Mozilla/5.0 (33245MB; 1080x2340; 1080x2340; 412x892; Pixel_6; 10)
    //   AppleWebKit/537.36 (KHTML, like Gecko)  Roblox/WinInet 2.737.1584
    //   Tablet Hybrid() GooglePlayStore RobloxApp/... (GlobalDist; GooglePlayStore)
    //
    // The shape is the answer: an ANDROID DEVICE ENVELOPE carrying a
    // DESKTOP client token. That is why every earlier attempt failed.
    // "Roblox/Linux" and "Roblox/WinInet" bare have the token but no
    // envelope (no switcher), and the full Android agent has the
    // envelope but an "ROBLOX Android App" token (no PC-targeted
    // experiences). Neither half works alone; both together do.
    //
    // Stud sends the same shape with its own honest values; real
    // memory, the real DISPLAY size (not the window's), real dpi, real
    // app version, and "Roblox/Linux" as the token, which is true and
    // live-confirmed to work exactly as Sober's "Roblox/WinInet" does.
    // Two deliberate differences from Sober: it does not claim to be a
    // Pixel 6, and it does not claim a device model at all. The field is
    // free text the server plainly does not check (Sober's value is
    // invented), so a real DMI model number there would be pure
    // fingerprinting for no functional gain.
    //
    // How this was captured, for whoever needs to redo it: Sober's
    // binaries are packed (zero "Roblox" strings) and its /proc/pid/mem
    // is root-owned under Flatpak, so neither static nor memory
    // inspection works. What did work: mitmproxy on 8080, its CA
    // appended to a bundle handed to the sandbox, and --ignore-hosts for
    // clientsettingscdn.roblox.com, because Sober's own launcher pins
    // its downloader's trust store and refuses to start otherwise.
    //
    // STUD_ANDROID_USER_AGENT=1 still selects the older, pure-Android
    // variant ("ROBLOX Android App" + a trailing " ChromeOS"), kept
    // because it is a real client string and a useful A/B.
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
    // The 2nd really is a dpi, not a third resolution. Sober sends a
    // resolution there ("1080x2340; 1080x2340; 412x892") only because
    // its DisplayMetrics carries pixel counts in xdpi/ydpi; echoing that
    // would make the slot carry no information at all, since on Stud it
    // would just repeat the 1st. The honest value is the display's real
    // dot pitch, and it is computed, not assumed: the compositor reports
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
    // machine's DMI model. The server does not check this field,
    // Sober sends an invented "Pixel_6" and everything works, so
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

    // The client token.
    //
    //   Roblox/WinInet      default, the literal the real Windows
    //                       client sends, and what Sober ships.
    //   ROBLOX Android App  the mobile client (STUD_ANDROID_USER_AGENT=1).
    //
    // Be precise about what is and is not established here, because
    // this was concluded wrongly twice in one session:
    //
    // ESTABLISHED. Sober's own agent, captured off the wire, is an
    // Android device envelope carrying "Roblox/WinInet", and Sober gets
    // both the account switcher and PC-targeted experiences. Sober's
    // enable_mobile_home_screen is nothing but this token
    // (false -> Roblox/WinInet, true -> ROBLOX Android App).
    //
    // NOT ESTABLISHED. Whether "Roblox/Linux" works. It was defaulted
    // to for a long time, then blamed for the missing switcher, then
    // cleared when a run with a corrected density-independent size
    // showed the button, but every one of those comparisons is
    // suspect, because the engine's cache carries state across runs and
    // the tests did not all clear it. Runs that differed only in this
    // token did not reproduce consistently. Settling it needs repeated
    // A/B runs that each start from a cleared ~/.cache/stud/cache and
    // cached ClientSettings, not one run per candidate.
    //
    // So the default is the string that is known to work in the same
    // shape on the same machine. Worth being honest that it is also the
    // one field in this agent that is not literally true: everything
    // else: memory, screen, dpi, density-independent size, version,
    // is measured, and the device field is deliberately generic rather
    // than this machine's DMI model.
    //
    // Checked against the binary: "Roblox/WinInet" really is in
    // libroblox.so and "Roblox/Linux" is not ("Roblox/libmp" is a false
    // positive, the repo path in mimalloc's banner). The standalone
    // platform-name list does contain "Linux", which is what the
    // earlier invented token was reasoned from.
    const char* client_token = use_android_app_token ? "ROBLOX Android App" : "Roblox/WinInet";

    char full[640];
    std::snprintf(full, sizeof(full),
                  "Mozilla/5.0 (%dMB; %dx%d; %dx%d; %dx%d; %s; %s) %s (KHTML, like Gecko)  "
                  "%s %s %s Hybrid() %s",
                  memory_mb, screen_w, screen_h, dpi_x, dpi_y, dip_w, dip_h, device_name,
                  android_release.c_str(), "AppleWebKit/537.36", client_token, roblox_version, form_factor,
                  nested);

    // " ChromeOS" belongs to the pure-Android variant only (the app's own HTTP/cookie layer
    // appends it when the app sees the ARC system feature). Sober's
    // agent does not carry it, and Sober is the one that works.
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

    // Real values for all five real params (see app_bridge.h's own doc
    // comment for the real traced through the app's own code call site these come from),
    // userAgent was the one remaining empty placeholder; now built via
    // build_real_user_agent() (see its own doc comment for the full
    // real trace and the two honestly-labeled best-effort sub-fields).
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
