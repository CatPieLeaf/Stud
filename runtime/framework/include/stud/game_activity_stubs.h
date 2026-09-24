#pragma once

#include "stud/webview_cookies.h"

#include <fake-jni/fake-jni.h>

#include <dlfcn.h>

#include <string>
#include <vector>

#include "stud/android_framework_stubs.h"
#include "stud/device_params.h"
#include "stud/linker.h"
#include "stud/system_locale.h"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Minimal FakeJni stub classes AGDK's GameActivity_register() (real,
// confirmed; see the engineering notes) FindClass's/GetMethodID's/
// GetFieldID's during GameActivity_initializeNativeCode(), plus the small
// number of other classes Roblox's own code has been found (via real
// crashes, see each class's own comment) to FindClass and dereference
// unconditionally. Promoted here, out of tools/try_bootstrap.cpp, once
// proven against the real libroblox.so, shared by the diagnostic tool
// and the real stud-runtime binary so both stay in sync automatically.
//
// Field layouts and signatures throughout this file are ground-truth, not
// guessed, each class's own comment documents its real source (AOSP
// GameActivity.cpp/GameActivity.java, live tracing against the real
// binary, or a live crash-report string).
namespace stud::jni_bridge {

// GameActivity.getWindowInsets/getWaterfallInsets's real return type,
// confirmed live, checked: broke on jnivm's own GetMethodID and
// read the actual requested signature string libroblox.so passes,
// "(I)Landroidx/core/graphics/Insets;"; it's the AndroidX Core compat
// class, not the android.graphics.Insets framework class this was first
// (wrongly) assumed to be. GetMethodID's full-signature-string match means
// the exact package matters, not just the simple name.
class InsetsStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("androidx/core/graphics/Insets")
    FakeJni::JInt left = 0;
    FakeJni::JInt top = 0;
    FakeJni::JInt right = 0;
    FakeJni::JInt bottom = 0;
};

// java.util.Locale; real GameActivity_register caches getLanguage/
// getScript/getCountry/getVariant regardless of runtime locale count
// (method-ID caching happens unconditionally at init, not lazily), per the
// real GameActivity.cpp source fetched from AOSP.
class LocaleStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/util/Locale")
    // The real system locale, not a literal. A real device answers this
    // from its own language settings; a desktop's equivalent is the
    // locale environment. See stud/system_locale.h.
    std::shared_ptr<FakeJni::JString> getLanguage() {
        return std::make_shared<FakeJni::JString>(
            stud::android_glue::system_locale().language.c_str());
    }
    std::shared_ptr<FakeJni::JString> getScript() { return std::make_shared<FakeJni::JString>(""); }
    std::shared_ptr<FakeJni::JString> getCountry() {
        return std::make_shared<FakeJni::JString>(
            stud::android_glue::system_locale().country.c_str());
    }
    // java.util.Locale.toString(): "pt_BR". The real implementation of
    // NativeLocaleJavaInterface.getLocale() is exactly
    // Configuration.getLocales().get(0).toString(), so this is the method
    // that answers it.
    std::shared_ptr<FakeJni::JString> toString() {
        return std::make_shared<FakeJni::JString>(
            stud::android_glue::system_locale().java_tag.c_str());
    }
    std::shared_ptr<FakeJni::JString> getVariant() { return std::make_shared<FakeJni::JString>(""); }
};

// android.os.LocaleList; real, live-caught correction (~/Stud/
// the engineering notes, "GameActivity_initializeNativeCode returns NULL" entry):
// this used to report size()==0. AGDK's own real readConfigurationValues()
// (game-activity/.../GameActivity.cpp) calls getLocales().size(), then
// resizes its own locale-data vector to that count, a real device
// always has at least one system locale, so size()==0 here produces an
// empty locale list no real device would ever have. Live evidence this
// session (a one-shot breakpoint dump of the real, live RAX-held string
// at the exact check gating initializeNativeCode's null-return branch)
// found that branch reached with an empty string, structurally
// consistent with Roblox's own extended initializeNativeCode reading a
// locale-derived string and finding it empty because of exactly this
// stub. Reporting one real locale, the system's own (LocaleStub reads
// it from the desktop's locale environment), so real code that
// legitimately expects >=1 system locale gets one.
class LocaleListStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/LocaleList")
    jint size() { return 1; }
    std::shared_ptr<LocaleStub> get(jint /*index*/) { return std::make_shared<LocaleStub>(); }
};

// androidx.core.view.WindowInsetsCompat$Type; real GameActivity_register
// caches these nine STATIC int-returning methods (window-inset-type-mask
// constants: caption bar, display cutout, IME, etc). Values are the real,
// stable AndroidX WindowInsetsCompat.Type bit-flag constants (public,
// documented API).
class WindowInsetsCompatTypeStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("androidx/core/view/WindowInsetsCompat$Type")
    static jint captionBar() { return 1 << 4; }
    static jint displayCutout() { return 1 << 7; }
    static jint ime() { return 1 << 3; }
    static jint mandatorySystemGestures() { return 1 << 8; }
    static jint navigationBars() { return 1 << 1; }
    static jint statusBars() { return 1 << 0; }
    static jint systemBars() { return statusBars() | navigationBars() | captionBar(); }
    static jint systemGestures() { return 1 << 9; }
    static jint tappableElement() { return 1 << 10; }
};

// android.content.res.Configuration. Real AOSP field list (fetched from
// aosp-mirror/platform_frameworks_base's actual source, checked).
// AGDK's native code reads these directly (GetFieldID, confirmed
// empirically, first failed on "colorMode"). `windowConfiguration`
// (android.app.WindowConfiguration) is still omitted, added only if a later
// error demands it.
//
// `locale` is the deprecated primary-locale field, which a real
// Configuration keeps equal to getLocales().get(0). The AGDK GameActivity
// in Roblox 2.740 looks it up in GameActivity_register and aborts when it
// is missing ("Unable to find field locale"), so the engine never boots.
class ConfigurationStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/content/res/Configuration")
    FakeJni::JFloat fontScale = 1.0f;
    FakeJni::JInt mcc = 0;
    FakeJni::JInt mnc = 0;
    FakeJni::JBoolean userSetLocale = false;
    FakeJni::JInt colorMode = 0;
    FakeJni::JInt screenLayout = 0;
    FakeJni::JInt fontWeightAdjustment = 0;
    FakeJni::JInt touchscreen = 1;    // TOUCHSCREEN_NOTOUCH, matches the desktop spoof
    FakeJni::JInt keyboard = 2;       // KEYBOARDHIDDEN_NO analog; real value: KEYBOARD_QWERTY
    FakeJni::JInt keyboardHidden = 1; // KEYBOARDHIDDEN_NO
    FakeJni::JInt hardKeyboardHidden = 1;
    FakeJni::JInt navigation = 1;   // NAVIGATION_NONAV
    FakeJni::JInt navigationHidden = 1;
    FakeJni::JInt orientation = 1;  // ORIENTATION_PORTRAIT-adjacent default; real value TBD
    FakeJni::JInt uiMode = 1;       // UI_MODE_TYPE_NORMAL
    FakeJni::JInt screenWidthDp = 1920;
    FakeJni::JInt screenHeightDp = 1080;
    FakeJni::JInt smallestScreenWidthDp = 1080;
    FakeJni::JInt densityDpi = 160;  // DENSITY_DEFAULT
    FakeJni::JInt compatScreenWidthDp = 1920;
    FakeJni::JInt compatScreenHeightDp = 1080;
    FakeJni::JInt compatSmallestScreenWidthDp = 1080;
    FakeJni::JInt assetsSeq = 0;
    FakeJni::JInt seq = 0;
    std::shared_ptr<LocaleStub> locale = std::make_shared<LocaleStub>();

    std::shared_ptr<LocaleListStub> getLocales() { return std::make_shared<LocaleListStub>(); }
};

// Real AGDK GameActivity_initializeNativeCode calls FindClass on its own
// Java class to RegisterNatives its own exported native methods back onto
// it. Method list below and every signature is ground-truth, fetched from
// AGDK's real, public, Apache-licensed
// GameActivity.java/GameActivity.cpp source
// (android.googlesource.com/platform/frameworks/opt/gamesdk,
// android-games-sdk-game-activity-release branch). These are the
// "@Keep"-annotated instance methods native code calls back into (as
// opposed to the `protected native` methods, which go the other direction
// see game_engine_boot.h's drive_game_activity_lifecycle(), which calls
// those).
// android.content.res.AssetManager; real class name, empty descriptor.
// Stud's own AAssetManager_fromJava() (android-glue/src/asset_manager.cpp)
// deliberately ignores both its arguments and routes real asset access
// through set_asset_base_directory()'s path-based logic instead, so this
// exists purely to give GameActivityStub::getAssets() below a real,
// non-null, correctly-typed jobject to hand back, same "type-shaped
// call convention only" reasoning as SurfaceStub above.
// The real app version, set once from the configured APK's own manifest
// (main.cpp, before dlopen) and read wherever the engine asks. Falls
// back to an empty string if the manifest could not be read, an honest
// "unknown", never an invented version number.
// Whether the per-keystroke text-input callbacks print. STUD_INPUT_TRACE
// covers the rest of the input path, so it covers these too.
bool text_input_trace_enabled();

const std::string& real_app_version();

// How Stud answers NativeGLJavaInterface.getWebViewUserAgent(): the
// reporter calls the engine's own setWebviewUserAgent with the agent the
// web view really sends. Set once at bring-up, by the process holding
// the library handle.
void set_webview_user_agent_reporter(std::function<void()> reporter);
void set_real_app_version(const std::string& version);

class AssetManagerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/content/res/AssetManager")
};

// java.io.File; real, minimal (absolute-path-only) stub. Real
// signature match for Context.getFilesDir()/getCacheDir()'s own real
// return type; getAbsolutePath()/getPath()/toString() cover every real
// caller found using File objects elsewhere in this project (the app's own code's own
// FlagCacheUtils, etc, all just build child paths off the
// absolute path string).
class JavaIoFileStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/io/File")
    JavaIoFileStub() = default;
    explicit JavaIoFileStub(std::string path) : path_(std::move(path)) {}
    std::shared_ptr<FakeJni::JString> getAbsolutePath() {
        return std::make_shared<FakeJni::JString>(path_);
    }
    std::shared_ptr<FakeJni::JString> getPath() { return std::make_shared<FakeJni::JString>(path_); }
    std::shared_ptr<FakeJni::JString> toStringJ() { return std::make_shared<FakeJni::JString>(path_); }

private:
    std::string path_;
};

class ContextStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/content/Context")
    std::shared_ptr<FakeJni::JString> getPackageName() {
        return std::make_shared<FakeJni::JString>("com.roblox.client");
    }
    std::shared_ptr<JavaIoFileStub> getFilesDir();
    std::shared_ptr<JavaIoFileStub> getCacheDir();
    // Confirmed (see SharedPreferencesStub's own doc
    // comment in android_framework_stubs.h): real signature
    // `(Ljava/lang/String;I)Landroid/content/SharedPreferences;`.
    // `mode` is ignored. Stud's own backing store has no real
    // multi-process/world-readable distinction to honor.
    std::shared_ptr<SharedPreferencesStub> getSharedPreferences(std::shared_ptr<FakeJni::JString> name,
                                                                  FakeJni::JInt /*mode*/) {
        return SharedPreferencesStub::get_or_create(name ? name->asStdString() : "");
    }
    std::shared_ptr<ResourcesStub> getResources() { return std::make_shared<ResourcesStub>(); }
};

class ActivityStub : public ContextStub {
public:
    DEFINE_CLASS_NAME("android/app/Activity", ContextStub)
    FakeJni::JBoolean runOnUiThread(std::shared_ptr<FakeJni::JObject> runnable);
    std::shared_ptr<ActivityStub> getApplicationContext() {
        return std::static_pointer_cast<ActivityStub>(shared_from_this());
    }
};

// Real Android's own hierarchy is
// `GameActivity extends ... extends Activity extends ... extends Context`,
// and the three classes above are declared here, ahead of it, purely so
// GameActivityStub can actually express that. It used to inherit
// FakeJni::JObject directly, which meant every Context method had to be
// duplicated onto it by hand (getResources() was, after a real
// live-caught miss) and, worse, any method whose real signature
// declares an `android.content.Context` parameter could never be called
// with the real activity: FakeJni resolves the argument by
// dynamic_cast, and MainGameActivityStub was not a ContextStub, so the
// call threw `Invalid Reference, Unexpected Type` instead
// (live-caught on DeviceUtils.getScreenPhysicalSizeInMillimeters).
class GameActivityStub : public ActivityStub {
public:
    DEFINE_CLASS_NAME("com/google/androidgamesdk/GameActivity", ActivityStub)

    void finish() {}
    void setWindowFlags(jint /*flags*/, jint /*mask*/) {}
    void setWindowFormat(jint /*format*/) {}
    std::shared_ptr<InsetsStub> getWindowInsets(jint /*type*/) {
        return std::make_shared<InsetsStub>();
    }
    std::shared_ptr<InsetsStub> getWaterfallInsets() { return std::make_shared<InsetsStub>(); }
    // Real param type is android.view.inputmethod.EditorInfo, accepted
    // generically as a plain JObject rather than a fully-fielded stub
    // class, since nothing here reads its fields.
    void setImeEditorInfo(std::shared_ptr<FakeJni::JObject> /*info*/) {}
    void setImeEditorInfoFields(jint /*a*/, jint /*b*/, jint /*c*/) {}
    // Confirmed-needed: AGDK's own GameActivity_initializeNativeCode
    // native implementation calls back via GetMethodID(activityClass,
    // "getAssets", ...) + CallObjectMethod rather than relying solely on
    // the AssetManager jobject argument main.cpp already passes (nullptr,
    // see game_engine_boot.cpp's drive_game_activity_lifecycle()), a
    // real, reproducible SIGSEGV (near-null address) occurred with this
    // method missing from the descriptor (GetMethodID returned a null
    // jmethodID, and AGDK's native code called it unconditionally with no
    // null check).
    std::shared_ptr<AssetManagerStub> getAssets() { return std::make_shared<AssetManagerStub>(); }
};

// com.roblox.client.startup.MainGameActivity, the real, concrete
// GameActivity subclass Roblox's manifest actually declares as its
// android.app.lib_name="roblox" entry point (real class
// name, confirmed via strings in libroblox.so:
// Java_com_roblox_client_startup_MainGameActivity_native*). Needed as its
// own class, not just GameActivityStub, so bootstrapTheApp(), a real,
// confirmed (strings: "bootstrapTheApp", "[FLog::NativeDM]
// bootstrapTheApp_:") native-to-Java callback, is reachable on whatever
// jobject Stud hands the engine as `thiz` when calling
// GameActivity_initializeNativeCode.
//
// Ground-truth-traced from the real MainGameActivity
// (the app's own code, checked): onCreate() calls its native-init step
// (nativeSetAssetPath, then conditionally nativePreloadFlagOverrides)
// BEFORE super.onCreate() (= GameActivity_initializeNativeCode). The
// InitParams call (nativeAppBridgeSetInitParams) is NOT made eagerly by
// Java at a fixed point at all. It's reached via setInitParamsForEngine,
// from bootstrapTheApp(), and bootstrapTheApp() is @Keep, i.e. called BACK
// from the native engine itself (on a background thread it spawns) once
// IT decides it's ready, not something Java calls proactively. Stud's
// own boot sequence previously called nativeAppBridgeSetInitParams
// eagerly, synchronously, before the engine ever asked; see
// game_engine_boot.h's doc comment for the real ordering fix this
// enables.
// Forward-declared here, fully defined further down this file (real,
// confirmed against the app's own code `com/roblox/client/startup/NativeHelper`; see that
// class's own doc comment), MainGameActivityStub only needs the
// incomplete type for its own `nativeHelper` field declaration below;
// the constructor that actually allocates one is defined out-of-line
// in game_activity_stubs.cpp, after NativeHelperStub's real definition.
class NativeHelperStub;

class MainGameActivityStub : public GameActivityStub {
public:
    DEFINE_CLASS_NAME("com/roblox/client/startup/MainGameActivity", GameActivityStub)

    MainGameActivityStub();

    // Real methods the engine looks up on this activity, both live-caught as
    // `GetMethodID MISS` in an ordinary run.
    //
    // getAppUpgradeKey() is the real, confirmed BuildConfig literal
    // "AppAndroidV", the same value Stud already passes to
    // nativeAppBridgeAppStart, so answering with anything else here would
    // contradict what the engine was told at startup.
    //
    // syncCookiesFromEngine() is the engine asking the platform to pull its
    // cookie jar back out and persist it. Stud seeds the jar at bring-up and
    // has nowhere else to persist it to yet, so this is an honest no-op that
    // says so rather than silently missing.
    static std::shared_ptr<FakeJni::JString> getAppUpgradeKey();
    void syncCookiesFromEngine();

    // Minimal C++ mirror of setInitParamsForEngine: invoked when Roblox's own
    // engine calls back into this method (matching the real
    // bootstrapTheApp() -> setInitParamsForEngine chain), instead of Stud
    // calling nativeAppBridgeSetInitParams itself ahead of time. The
    // callback is set by whoever constructs this instance, right before
    // handing it to GameActivity_initializeNativeCode; see
    // run_init_params_bootstrap() in bootstrap.h.
    void bootstrapTheApp() {
        if (on_bootstrap_the_app) {
            on_bootstrap_the_app();
        }
    }

    std::function<void()> on_bootstrap_the_app;

    // Confirmed field (`public final NativeHelper
    // nativeHelper;` on the real MainGameActivity): exposed so
    // native code doing a real `GetObjectField(activity, "nativeHelper",
    // "Lcom/roblox/client/startup/NativeHelper;")` (the real, ordinary
    // way a held Activity reference would reach it, matching every
    // other real field-based lookup this project has found and fixed)
    // gets back a real, live, always-non-null instance instead of a
    // silent null, allocated once in the constructor below.
    std::shared_ptr<NativeHelperStub> nativeHelper;

    // Confirmed getter (`getNativeHelper()` on the real
    // MainGameActivity's getter returns `this.nativeHelper`,
    // Kotlin's own real synthetic accessor for a `public final val`
    // property, generated for interop callers that access it via a
    // method rather than raw field access). Gap found in testing found
    // this session (the engineering notes, "class is null gameActivity_*"
    // cluster): only the raw field was registered, not this getter,
    // if real native code reaches for `nativeHelper` via
    // `CallObjectMethod(activity, getNativeHelperMethodID)` rather than
    // `GetObjectField`, the method lookup fails silently (returns a null
    // jmethodID), the resulting call returns null, and every subsequent
    // `gameActivity_*` method lookup on that null object fails the same
    // way (`GetObjectClass(null)` -> jnivm's own "Invalid" sentinel ->
    // null class), exactly the observed, still-open symptom.
    std::shared_ptr<NativeHelperStub> getNativeHelper() { return nativeHelper; }
};

// Minimal placeholder for android.view.Surface, AGDK's
// onSurfaceCreatedNative(long, Surface) needs an argument of this Java
// type, but Stud's own ANativeWindow_fromSurface() (android-glue)
// deliberately ignores both its env and surface arguments and connects
// straight to the real host Wayland compositor regardless, so this
// class exists purely to satisfy JNI's type-shaped call convention. Real
// JNI does not runtime-check an object's dynamic type against a method's
// declared parameter type at CallVoidMethod() time, confirmed true for
// jnivm too, matching real JNI semantics.
class SurfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/view/Surface")
};

// android.app.Activity. Real class name. Also used as the `vrContext`
// argument to StartAppParams/StartGameParams (see engine_v2_bridge.h):
// a plausible, non-null placeholder for the "current Activity" a real
// VR-context lookup would use. Stud's target is never VR (isVrDevice is
// always false, see init_params.h's own desktop-spoof comment), so real
// code is expected to branch away before ever dereferencing most of
// this object's methods, same "grow against real evidence"
// discipline as every other stub in this file, not preemptively
// fleshed out. Two exceptions grown proactively this session, both
// real, extremely common Activity/Context call patterns rather than
// guesses: `runOnUiThread(Runnable)`, wired to the real
// `android_framework_stubs.h` Looper/Handler machinery (the Runnable
// really runs, on the real main-looper pump thread, not a no-op), and
// `getPackageName()` (real, honest `"com.roblox.client"`; this
// genuinely is that real app, just hosted outside Android).


// Real paths Stud already computes and hands to libroblox.so directly
// via nativeSetFilesDirectory/nativeSetCacheDirectory (native_settings.
// cpp), set once at boot (process-b/src/main.cpp) so ActivityStub's
// own getFilesDir()/getCacheDir() can hand back the SAME real paths
// through the real Context API real AGDK/native code also commonly
// uses, instead of leaving it unregistered.
void set_activity_context_directories(std::string files_dir, std::string cache_dir);

// Confirmed: libroblox.so embeds the bare literal class
// name `android/content/Context` (not just `android/app/Activity`/
// `android/app/Application`), and several real JNI signature strings
// declare a `Context`-typed parameter directly (e.g. `(Landroid/content/
// Context;)Landroid/graphics/Point;`, `(Landroid/content/Context;I)V`).
// Real Android compiled bytecode always encodes a method's *declared*
// Java source parameter/return type in its descriptor, not whatever
// concrete object is passed at a given call site, so a real method
// declared `void foo(Context c)` has descriptor `(Landroid/content/
// Context;)V` even when actually called with an Activity or
// Application instance. This means a distinct `ContextStub` base class
// (real Android hierarchy: both Activity and Application really are
// Context subclasses) is needed for exact signature matching whenever
// native code resolves a method by its base `Context`-declared type:
// a real, previously-missing distinction, not cosmetic refactoring.
// Holds every Context method already proven needed by real evidence.
// ContextStub/ActivityStub are defined further up, ahead of
// GameActivityStub, which inherits them (see the comment there).

// Confirmed (`com.roblox.client.JNIBaseUrlSetter`):
// a real `@Keep public static void setBaseUrl(String)` that native code
// calls to push the real base URL back into the Java layer. Was
// completely unregistered, so every call produced a "class is null"
// diagnostic and was silently dropped.
class JNIBaseUrlSetterStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/JNIBaseUrlSetter")

    static void setBaseUrl(std::shared_ptr<FakeJni::JString> url);
};

// Confirmed (`com.roblox.client.JNIAppRestarter`):
// `@Keep public static void restartApp(Context, String)`. On a real
// device this fires an ACTION_VIEW intent for the given URL and then
// calls `Runtime.getRuntime().exit(0)`, i.e. it really does tear the
// process down and relaunch it.
//
// Stud deliberately does NOT exit here. There is no Android activity
// manager to relaunch us, so exiting would simply kill Stud with no
// restart; and the engine was already calling this (three times per
// run, live-observed) into an unregistered class, so the request has
// always been dropped anyway. Registering it makes that request
// *visible*, the requested URL is real, useful evidence about what
// the engine is trying to do, without taking a destructive action
// Stud cannot honestly complete.
// com.roblox.universalapp.experience.JNIExperienceProtocol, a real
// class whose methods are all native (getLaunchId() among them, which
// Stud calls itself to learn the MessageBus topic the Lua app publishes
// experience-launch requests on). Registering it only so FindClass has
// something to return: FakeJni's FindClass hands back null for any class
// Stud has not registered, and passing a null jclass into a native entry
// point is the exact shape that has silently broken calls here before.
// No methods, the real ones live in libroblox.so and are called through
// their exported symbols, not through this descriptor.
class JNIExperienceProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/experience/JNIExperienceProtocol")
};

// com.roblox.universalapp.linking.JNILinkingProtocol, the protocol
// behind GuiService:OpenBrowserWindow, i.e. Settings' About Us / Careers
// / Parents links. Same reason as the class above: FakeJni's FindClass
// returns null for anything unregistered, and Stud calls this class's
// own exported getters (getProtocolName, getOpenURLId, getUrlKey, ...)
// to learn the protocol's wire names, with a null jclass every one of
// those returns nothing and the protocol cannot be registered at all.
// No methods: the real ones are called through their exported symbols.
class JNILinkingProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/linking/JNILinkingProtocol")
};

class JNIAppRestarterStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/JNIAppRestarter")

    static void restartApp(std::shared_ptr<ContextStub> context,
                            std::shared_ptr<FakeJni::JString> url);

    // Set once during bring-up. Only render-host can reach the desktop
    // (Process B is sandboxed), so the URL travels the same render IPC
    // the web-view panel already uses.
    static inline std::function<void(const std::string&)> on_open_external_url;
};





// Confirmed (libroblox.so embeds the literal class name
// `android/app/Application` plus the real signature `()Landroid/app/
// Application;`), a real, separate class from `android/app/Activity`
// (real Android: Application is the process-wide singleton Context,
// Activity is per-screen), needed purely so a real caller resolving
// `ActivityThread.getApplication()` by its real declared return type
// gets a class whose name actually matches (see ActivityThreadStub
// below). Inherits every real Context method from ContextStub, backed
// by the same real global state (set_activity_context_directories()),
// since Stud has only one real process-wide files/cache directory pair
// regardless of which Context-shaped object hands it out.
class ApplicationStub : public ContextStub {
public:
    DEFINE_CLASS_NAME("android/app/Application", ContextStub)
    static std::shared_ptr<ApplicationStub> singleton();
};

// Confirmed: libroblox.so embeds the literal real class
// name `android/app/ActivityThread` plus real method names
// `currentActivityThread` (signature `()Landroid/app/ActivityThread;`)
// and `getApplication` (signature `()Landroid/app/Application;`).
// This is real Android's own well-known internal
// `ActivityThread.currentActivityThread().getApplication()` idiom, the
// standard way real native/utility code fetches the process-wide
// Context from anywhere without a Context reference already in hand.
// Plausible significance: if any of Roblox's own native code
// uses exactly this idiom to bootstrap a Context before calling
// Context-shaped methods (SharedPreferences, Resources, files/cache
// dirs, ...), every one of those calls would have failed the same
// silent "class is null" way this whole session's sweep keeps finding,
// specifically because `ActivityThread` was never registered at all,
// checked, this is the exact real mechanism real Android uses.
class ActivityThreadStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/app/ActivityThread")
    std::shared_ptr<ApplicationStub> getApplication() { return ApplicationStub::singleton(); }

    static std::shared_ptr<ActivityThreadStub> currentActivityThread() {
        static auto singleton = std::make_shared<ActivityThreadStub>();
        return singleton;
    }
};

// Confirmed (`org.fmod.FMOD`, `org.fmod.AudioDevice`
// : real, vendored FMOD Android SDK glue classes, not
// Roblox-specific), cross-confirmed via a real embedded-string hit in
// libroblox.so: `[FLog::FMODJAVA] Error during CallBooleanMethod/
// CallIntMethod/CallStringMethod`, direct, real evidence that
// native FMOD code makes real JNI calls back into these exact Java
// classes and has its own dedicated error-logging category for when
// they fail. Neither class carries `@Keep` (real evidence class
// itself, from the earlier embedded-string sweep, not the `@Keep`
// heuristic; see SessionReporterJavaInterfaceStub's own doc comment
// for why embedded-string evidence is treated as equally reliable).
// `FMOD.init(Context)` is the real, one-time entry point a real device
// calls at real audio-subsystem bring-up, storing a static Context
// every other real method here depends on (`gContext`), honest
// `false`/`0`/no-op answers throughout for anything needing real audio
// hardware info Stud has no access to at this layer, matching every
// other "capability Stud doesn't have" default in this file.
// `MediaCodec` (org.fmod.MediaCodec) deliberately NOT included.
// Real, but deep in-experience compressed-audio-file decode plumbing
// (MediaExtractor + java.lang.reflect.Proxy), not boot-relevant.
class FMODStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/fmod/FMOD")

    static void init(std::shared_ptr<ContextStub> context) {
        context_singleton() = context;
        std::printf("stud: FMOD.init() called (real FMOD Java audio bridge, honest no-op backing)\n");
    }
    static FakeJni::JBoolean checkInit() { return context_singleton() != nullptr; }
    static void close() { context_singleton() = nullptr; }
    static std::shared_ptr<AssetManagerStub> getAssetManager() {
        if (!context_singleton()) return nullptr;
        return std::make_shared<AssetManagerStub>();
    }
    // Real values, matching the audio device Stud actually provides
    // (render-client/src/aaudio_stub.cpp: 48 kHz, 480-frame bursts).
    // These used to report 0, which is what a real device reports when it
    // has no audio at all, and FMOD configures its output from them, so
    // zero told it there was nothing to configure.
    static FakeJni::JInt getOutputBlockSize() { return 480; }
    static FakeJni::JInt getOutputSampleRate() { return 48000; }
    static FakeJni::JBoolean isBluetoothOn() { return false; }
    static FakeJni::JBoolean lowLatencyFlag() { return false; }
    static FakeJni::JBoolean proAudioFlag() { return false; }
    // The app's own code: `Build.VERSION.SDK_INT >= 27`; Stud's own
    // BuildVersionStub.SDK_INT is 34, so `true` is the internally
    // consistent, honest answer (same reasoning as
    // SystemThemeProtocolStub::isSystemThemeAvailable()).
    static FakeJni::JBoolean supportsAAudio() { return true; }
    // The app's own logic, evaluated against the answers above. Stud's
    // output is a normal 10ms-burst stream, not a low-latency fast path,
    // and claiming otherwise would make FMOD ask for a buffer size the
    // host cannot honour.
    static FakeJni::JBoolean supportsLowLatency() { return false; }

private:
    static std::shared_ptr<ContextStub>& context_singleton() {
        static std::shared_ptr<ContextStub> instance;
        return instance;
    }
};

// org.fmod.AudioDevice: FMOD's Java output, an AudioTrack of 16-bit PCM
// (`org.fmod.AudioDevice`). FMOD constructs one and falls back to it
// when its native output (AAudio, which Stud implements) will not start.
// Instance methods, called back on the object FMOD made.
//
// The real class hands the bytes to AudioTrack, whose write() blocks until
// the device has room; the hooks below do the same through the render
// host's audio output, and are set during bring-up
// (runtime/src/fmod_audio_output.cpp). Without them init() fails, which
// is what a real AudioTrack does when it cannot be created.
class AudioDeviceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/fmod/AudioDevice")
    struct Output {
        // channels, sample rate; true once the host is ready for writes.
        std::function<bool(int, int)> open;
        // Interleaved 16-bit samples, in the channels and rate opened with.
        std::function<void(const int16_t*, size_t)> write;
        std::function<void()> close;
    };
    static inline Output output;

    FakeJni::JBoolean init(FakeJni::JInt channels, FakeJni::JInt sampleRate,
                            FakeJni::JInt numBuffers, FakeJni::JInt bufferLength) {
        const bool ok = output.open && output.open(channels, sampleRate);
        std::printf("stud: AudioDevice.init(%d ch, %d Hz, %d x %d): %s\n",
                    static_cast<int>(channels), static_cast<int>(sampleRate),
                    static_cast<int>(numBuffers), static_cast<int>(bufferLength),
                    ok ? "playing through the host" : "no host audio output");
        std::fflush(stdout);
        open_ = ok;
        return ok;
    }
    void close() {
        if (open_ && output.close) output.close();
        open_ = false;
    }
    void write(std::shared_ptr<FakeJni::JByteArray> data, FakeJni::JInt length) {
        if (!open_ || !data || !output.write || length <= 0) return;
        const size_t bytes = std::min<size_t>(static_cast<size_t>(length),
                                              static_cast<size_t>(data->getSize()));
        output.write(reinterpret_cast<const int16_t*>(data->getArray()), bytes / 2);
    }

private:
    bool open_ = false;
};

// A real, empty java.util.ArrayList, for callers that take a
// List<T> and are expected to handle an empty (not null) one. Real
// bug found and fixed (the engineering notes, "client_settings_bridge"
// entry): nativePostClientSettingsLoadedInitialization3
// (List<ApplicationExitInfoCpp>) crashes when handed a raw null,
// unlike most other JNI entry points already observed in this project
// a real device always passes a real (possibly empty)
// ArrayList here (the app's own exit-info list, traced from the
// the app's own code), never null. `isEmpty()`/`size()` match a real,
// freshly-constructed ArrayList's own behavior; `get(int)` is
// deliberately not implemented since nothing calling `isEmpty()`/
// `size()` first (matching Roblox's own generally defensive style)
// should ever reach it, grown against real evidence if that
// assumption turns out wrong.
class EmptyArrayListStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/util/ArrayList")
    FakeJni::JInt size() { return 0; }
    FakeJni::JBoolean isEmpty() { return true; }
};

// java.util.List, the interface, distinct from the ArrayList above and NOT
// one of the types jnivm provides itself (checked against its own
// `blacklisted[]` array, which is what makes the ByteBuffer/Class collision
// class dangerous; List is absent from it, so a Stud stub is safe here).
// Live-caught as a plain `FindClass(java/util/List) -> raw jclass=0x0` in every
// run, which means the engine could not even resolve the type.
class JavaUtilListStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/util/List")
    FakeJni::JInt size() { return 0; }
    FakeJni::JBoolean isEmpty() { return true; }
    // Asked for as soon as the class resolved. Honest for an empty list: there
    // is no element at any index, and size()/isEmpty() above say so, so a
    // caller that respects them never gets here.
    std::shared_ptr<FakeJni::JObject> get(FakeJni::JInt /*index*/) { return nullptr; }
};

// com.roblox.audio.AppRtcDeviceWrapper and org.fmod.MediaCodec, the two real
// audio classes the engine resolves and never finds
// (`FindClass(...) -> raw jclass=0x0`, every run). Stud has no audio path at
// all yet, so these are deliberately registered EMPTY rather than guessed at:
// the class resolving turns a silent null into precise
// `GetMethodID MISS class=... method=...` lines naming exactly which methods
// the engine actually wants, which is the evidence needed to implement them
// honestly instead of inventing a surface.
// AppRtcDeviceWrapper is no longer empty: the engine constructs it with a
// pointer back to itself and asks which audio route is in use and to mute
// the microphone at the system level. On a desktop the route is whatever
// the audio server chose, so the honest answer is the wired one, not an
// earpiece, which a desktop does not have. Muting is reported rather than
// claimed: Stud has no system-level mute, and the engine stops reading the
// capture stream when it mutes anyway. The enum's own order is
// SPEAKER_PHONE, WIRED_HEADSET, EARPIECE, BLUETOOTH, NONE.
class AppRtcDeviceWrapperStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/audio/AppRtcDeviceWrapper")
    FakeJni::JLong nativeReference = 0;
    AppRtcDeviceWrapperStub() = default;
    explicit AppRtcDeviceWrapperStub(FakeJni::JLong reference) : nativeReference(reference) {}

    FakeJni::JInt getSelectedAudioDeviceAsInt();
    std::shared_ptr<FakeJni::JString> getSelectedAudioDeviceName();
    FakeJni::JBoolean isValid();
    void wrapSetCommunicationMute(FakeJni::JBoolean muted);
    void wrapStartCommunication();
    void wrapStopCommunication();
};

// FMOD's hardware-decoder bridge. Stud has no MediaCodec at all, so
// `init()` answering false is the truthful "no hardware codec here",
// the same answer MediaCodecInfoUtils already gives, and FMOD decodes
// in software instead, which is what has been playing Roblox's audio all
// along. Registered empty until now, which left the engine constructing
// it and finding nothing: the real surface is small and is below, so the
// refusal is explicit rather than a silently dropped call.
class FmodMediaCodecStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/fmod/MediaCodec")
    FmodMediaCodecStub() = default;
    FakeJni::JBoolean init(FakeJni::JLong handle);
    FakeJni::JInt getChannelCount();
    FakeJni::JInt getSampleRate();
    FakeJni::JLong getLength();
    FakeJni::JInt read(std::shared_ptr<FakeJni::JByteArray> buffer, FakeJni::JInt size);
    void release();
};

// Classes the engine only ever asks for once its reflective
// ClassLoader.loadClass() path works; see register_java_lang_class_methods()
// in android_framework_stubs.cpp. Registered empty first, deliberately: that
// turns a silent null into precise `GetMethodID MISS ... method=...` lines
// naming what is actually wanted, which is the evidence needed to implement
// them honestly.
// Both of these are real Kotlin `object` singletons (confirmed against the app's own code
// against the configured APK), so native code reaches them the Kotlin
// way, a `public static final <Self> INSTANCE` field, and then calls
// instance methods on it. Registering them empty first is what named
// these members exactly, via the GetMethodID/GetFieldID MISS
// diagnostics, instead of leaving a silent null.
// The real Djinni-generated abstract base the singleton below extends
// (`class PlatformSystemDialogHandler extends IPlatformSystemDialogHandler`,
// confirmed against the app's own code). Djinni's own glue resolves the interface's methods off
// THIS class, not off the concrete singleton, so it has to exist and
// carry them, an unregistered base threw `djinni::jni_exception` and
// took the whole boot down with it.
// Signature-matching only: these two real Djinni-generated types appear
// in `open()`'s real signature, so the method cannot resolve without
// classes carrying their exact names. Nothing reads their fields, so
// they stay empty rather than inventing a layout, same approach as
// DeviceDisplayCapabilityStub in protocol_platform_stubs.h.
class SystemDialogRequestStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/protocols/systemdialogplatforminterface/generated/SystemDialogRequest")
};

class ISystemDialogCallbackStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/protocols/systemdialogplatforminterface/generated/ISystemDialogCallback")
};

class IPlatformSystemDialogHandlerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/protocols/systemdialogplatforminterface/generated/IPlatformSystemDialogHandler")

    // Overridden by the real singleton below; declared here because
    // Djinni looks it up on the base class. See that override for why
    // `false` is the honest answer.
    FakeJni::JBoolean isAvailable() { return false; }
    // Real: shows a system dialog and returns its id. Stud has no
    // Android dialog UI, so there is nothing to show and no real id to
    // return, 0, logged, rather than a fabricated handle the engine
    // would later try to dismiss. Unreachable while isAvailable() is
    // false, which is the point.
    FakeJni::JLong open(std::shared_ptr<SystemDialogRequestStub> request,
                        std::shared_ptr<ISystemDialogCallbackStub> callback);
    void dismiss(FakeJni::JLong id);
    void dismissAll();
};

class PlatformSystemDialogHandlerStub : public IPlatformSystemDialogHandlerStub {
public:
    DEFINE_CLASS_NAME("com/roblox/protocols/systemdialog/PlatformSystemDialogHandler",
                      IPlatformSystemDialogHandlerStub)

    static std::shared_ptr<PlatformSystemDialogHandlerStub> instance();
    static inline std::shared_ptr<PlatformSystemDialogHandlerStub> INSTANCE;

    // The app's own code checks whether it currently holds an Activity to show a
    // dialog on. Stud hosts no real Android dialog UI at all, so `false`
    // is the honest answer and the same one the real class gives before
    // any Activity has been attached, claiming otherwise would make the
    // engine queue dialogs nothing can ever display or dismiss.
    FakeJni::JBoolean isAvailable() { return false; }
};

class FacialAgeEstimationProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/facialageestimation/FacialAgeEstimationProtocol")

    static std::shared_ptr<FacialAgeEstimationProtocolStub> instance();
    static inline std::shared_ptr<FacialAgeEstimationProtocolStub> INSTANCE;

    // Real implementation loads Persona's face-scan SDK reflectively and
    // reports whether that succeeded (its own imports name
    // InvocationTargetException). Stud bundles no such SDK and has no
    // camera pipeline, so `false` is the honest, real answer, and it is
    // the answer that keeps the engine from starting an inquiry flow
    // that could never complete.
    FakeJni::JBoolean isAvailable() { return false; }
    // Real: stores the native listener pointer for later onComplete/
    // onCancel/onError callbacks. Stud records it so a future real
    // implementation has it, and so the call is not silently dropped.
    void setListener(FakeJni::JLong native_listener_ptr);
    // Real: launches the Persona inquiry UI. Unreachable in practice
    // while isAvailable() is false; logged rather than faked, since
    // pretending an age check happened would be inventing a result.
    void startInquiry(std::shared_ptr<FakeJni::JString> inquiry_id,
                      std::shared_ptr<FakeJni::JString> session_token);
};

// com.google.androidgamesdk.gametextinput.State, a separate AGDK
// companion module (game-text-input, IME composing-state carrier). Real
// init code inside libroblox.so FindClass's this, gets null if
// unregistered, wraps it in NewGlobalRef(null), and later dereferences
// that null class: real SIGSEGV, not a Stud bug, just a missing class.
// Public field layout (AOSP game-text-input module).
//
// Proactively-grown addition (this session, real AGDK source read
// directly, the AGDK reference source, src/game-text-input/prefab-src/
// modules/game-text-input/src/game-text-input/gametextinput.cpp, not
// guessed): `GameTextInput::GameTextInput()`'s own constructor
// unconditionally resolves `State`'s own `<init>(Ljava/lang/String;
// IIII)V` constructor (via `GameTextInput::stateToJava()`, lazily on
// first real use): real signature, real parameter order (text,
// selectionStart, selectionEnd, composingRegionStart,
// composingRegionEnd), confirmed straight from the real call site.
// Previously only the default constructor existed here (satisfied the
// original null-class crash, but not a real `stateToJava()` call, which
// would silently fail its own `GetMethodID` and log "Can't find
// gametextinput.State constructor", not yet observed live in this
// project, since no real interactive text-input session has happened
// yet, but a real, correctly-scoped gap per the real source, not a
// guess).
class GameTextInputStateStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/google/androidgamesdk/gametextinput/State")
    GameTextInputStateStub() = default;
    GameTextInputStateStub(std::shared_ptr<FakeJni::JString> text, FakeJni::JInt selection_start,
                            FakeJni::JInt selection_end, FakeJni::JInt composing_region_start,
                            FakeJni::JInt composing_region_end)
        : text(text ? std::move(text) : std::make_shared<FakeJni::JString>("")),
          selectionStart(selection_start),
          selectionEnd(selection_end),
          composingRegionStart(composing_region_start),
          composingRegionEnd(composing_region_end) {}

    std::shared_ptr<FakeJni::JString> text = std::make_shared<FakeJni::JString>("");
    FakeJni::JInt selectionStart = 0;
    FakeJni::JInt selectionEnd = 0;
    FakeJni::JInt composingRegionStart = -1;
    FakeJni::JInt composingRegionEnd = -1;
};

// Confirmed (`com.roblox.engine.jni.model.ChannelRecord`
// ), also a real, confirmed embedded class name in
// libroblox.so: a plain, real, public-field data holder (real device
// native code constructs+populates one directly via `NewObject`, same
// shape as GameTextInputStateStub above), not a native->Java callback
// surface. Real constructor: `ChannelRecord(String, long)`.
class ChannelRecordStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/model/ChannelRecord")
    ChannelRecordStub() = default;
    ChannelRecordStub(std::shared_ptr<FakeJni::JString> name, jlong id)
        : name(name ? std::move(name) : std::make_shared<FakeJni::JString>("")), id(id) {}

    std::shared_ptr<FakeJni::JString> name = std::make_shared<FakeJni::JString>("");
    jlong id = 0;
};

// Confirmed (`com.roblox.engine.jni.model.
// ApplicationExitInfoCpp`), also a real, confirmed
// embedded class name: a plain, real, public-field data holder
// mirroring real Android's own `ApplicationExitInfo` API (real device
// native code constructs+populates one via `NewObject`, matching one of
// its 3 real overloaded constructors). Already-established
// context (see EmptyArrayListStub's own doc comment): the one
// currently-known real call site
// (`nativePostClientSettingsLoadedInitialization3(List<
// ApplicationExitInfoCpp>)`) is correctly satisfied by an empty real
// ArrayList (Stud tracks no real exit history); this class itself is
// added for completeness/signature-matching in case another real call
// site constructs individual instances, not because the known call
// site needs it.
class ApplicationExitInfoCppStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/model/ApplicationExitInfoCpp")
    ApplicationExitInfoCppStub() = default;
    ApplicationExitInfoCppStub(FakeJni::JInt pid, jlong timestamp,
                                std::shared_ptr<FakeJni::JString> exitReason)
        : mPid(pid), mTimestamp(timestamp), mExitReason(std::move(exitReason)) {}
    ApplicationExitInfoCppStub(FakeJni::JInt pid, FakeJni::JInt signal, jlong timestamp,
                                std::shared_ptr<FakeJni::JString> exitReason)
        : mPid(pid), mSignal(signal), mTimestamp(timestamp), mExitReason(std::move(exitReason)) {}
    ApplicationExitInfoCppStub(FakeJni::JInt pid, FakeJni::JInt signal, jlong timestamp,
                                std::shared_ptr<FakeJni::JString> exitReason,
                                std::shared_ptr<FakeJni::JString> exitSubreason,
                                std::shared_ptr<FakeJni::JString> description, jlong pss, jlong rss,
                                FakeJni::JInt importance)
        : mPid(pid),
          mSignal(signal),
          mTimestamp(timestamp),
          mExitReason(std::move(exitReason)),
          mExitSubreason(std::move(exitSubreason)),
          mDescription(std::move(description)),
          mPss(pss),
          mRss(rss),
          mImportance(importance) {}

    FakeJni::JInt mPid = 0;
    FakeJni::JInt mSignal = 0;
    jlong mTimestamp = 0;
    std::shared_ptr<FakeJni::JString> mExitReason = std::make_shared<FakeJni::JString>("");
    std::shared_ptr<FakeJni::JString> mExitSubreason = std::make_shared<FakeJni::JString>("");
    std::shared_ptr<FakeJni::JString> mDescription = std::make_shared<FakeJni::JString>("");
    jlong mPss = 0;
    jlong mRss = 0;
    FakeJni::JInt mImportance = 0;
};

// Confirmed (`com.roblox.universalapp.achievement.
// JNIAchievement`, also confirmed as an embedded class
// name): both real, private-but-@Keep static entry points native code
// calls to request an async achievement grant/check. The app's own
// contract: the boolean return means "request accepted, dispatched
// asynchronously" (a real Kotlin coroutine that later calls back
// `success(long,boolean)`/`failure(long,String)` on the class's own
// singleton instance, entirely independent of this return value):
// `true` here is honest, not fabricated, since Stud genuinely does
// accept the call synchronously; Stud has no real achievements backend
// to actually complete the async round-trip with, and doing so isn't
// needed for real, boot-relevant behavior (achievements are strictly
// an in-experience, post-join feature).
class JNIAchievementStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/achievement/JNIAchievement")
    static FakeJni::JBoolean grantAchievementForNativeAsync(std::shared_ptr<FakeJni::JString> codeName,
                                                              jlong handle) {
        std::printf("stud: JNIAchievement.grantAchievementForNativeAsync: codeName=%s handle=%lld "
                    "(no-op, no real achievements backend)\n",
                    codeName ? codeName->asStdString().c_str() : "", static_cast<long long>(handle));
        return true;
    }
    static FakeJni::JBoolean hasAchievedForNativeAsync(std::shared_ptr<FakeJni::JString> codeName,
                                                         jlong handle) {
        std::printf("stud: JNIAchievement.hasAchievedForNativeAsync: codeName=%s handle=%lld "
                    "(no-op, no real achievements backend)\n",
                    codeName ? codeName->asStdString().c_str() : "", static_cast<long long>(handle));
        return true;
    }
};

// com.google.androidgamesdk.gametextinput.InputConnection; real,
// previously entirely-unregistered class from the same real AGDK
// source. `GameTextInput`'s own constructor resolves all three of
// `setState(State)`/`setSoftKeyboardActive(boolean,int)`/
// `restartInput()` on it unconditionally (real signatures, straight
// from the real call sites in gametextinput.cpp), with no
// registration at all, every one of these would have been a real
// "class is null"/null-jmethodID gap the moment any real text-input
// code path is exercised. Bodies are honest no-ops for now (Stud has
// no real on-screen keyboard/IME integration yet, a real, separate,
// much larger feature), matching this file's own "satisfy the real
// call shape now, grow the real behavior against real evidence later"
// convention used throughout.
class InputConnectionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/google/androidgamesdk/gametextinput/InputConnection")
    void setState(std::shared_ptr<GameTextInputStateStub> /*state*/) {}
    void setSoftKeyboardActive(FakeJni::JBoolean /*active*/, FakeJni::JInt /*flags*/) {}
    void restartInput() {}
};

// com.roblox.engine.jni.model.NativeTextBoxInfo. Real, confirmed against the app's own code
// POJO the engine hands to `showKeyboard()` describing the focused
// TextBox (its on-screen rect, font, colour, input/return-key type and
// wrapping). Stud has no soft keyboard to configure from it, but the
// class must exist for `showKeyboard`'s own JNI signature to resolve at
// all, without it the engine's real call is silently dropped, which is
// exactly what kept keyboard input dead.
class NativeTextBoxInfoStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/model/NativeTextBoxInfo")
    FakeJni::JInt font = 0;
    FakeJni::JFloat fontSize = 0.0f;
    FakeJni::JFloat height = 0.0f;
    FakeJni::JBoolean manualFocusRelease = false;
    FakeJni::JBoolean multiline = false;
    FakeJni::JInt returnKeyType = 0;
    FakeJni::JInt textColor = 0;
    FakeJni::JInt textInputType = 0;
    FakeJni::JBoolean textWrapped = false;
    FakeJni::JFloat width = 0.0f;
    FakeJni::JFloat x = 0.0f;
    FakeJni::JInt xAlignment = 0;
    FakeJni::JFloat y = 0.0f;
    FakeJni::JInt yAlignment = 0;
    FakeJni::JBoolean editable = false;

    NativeTextBoxInfoStub() = default;
    // Real 15-argument constructor, in the real parameter order, read from
    // THIS APK's own copy of the class rather than an older one; that
    // older copy is of the 2.733 build, whose constructor takes
    // 14 arguments and has no `editable` field at all. The live binary asks
    // for (FFFFFZIIIIIIZZZ), and with no constructor registered the engine
    // could not build one, so the real
    // NativeGLJavaInterface.showKeyboard(handle, ..., NativeTextBoxInfo) never
    // fired and Stud never learned which TextBox had focus, which is why
    // typing did nothing.
    NativeTextBoxInfoStub(FakeJni::JFloat x_, FakeJni::JFloat y_, FakeJni::JFloat width_,
                          FakeJni::JFloat height_, FakeJni::JFloat fontSize_,
                          FakeJni::JBoolean multiline_, FakeJni::JInt xAlignment_,
                          FakeJni::JInt yAlignment_, FakeJni::JInt textColor_, FakeJni::JInt font_,
                          FakeJni::JInt textInputType_, FakeJni::JInt returnKeyType_,
                          FakeJni::JBoolean manualFocusRelease_, FakeJni::JBoolean textWrapped_,
                          FakeJni::JBoolean editable_)
        : font(font_),
          fontSize(fontSize_),
          height(height_),
          manualFocusRelease(manualFocusRelease_),
          multiline(multiline_),
          returnKeyType(returnKeyType_),
          textColor(textColor_),
          textInputType(textInputType_),
          textWrapped(textWrapped_),
          width(width_),
          x(x_),
          xAlignment(xAlignment_),
          y(y_),
          yAlignment(yAlignment_),
          editable(editable_) {}
};

// com.roblox.engine.jni.video.VideoCodecCapability. Real, confirmed against the app's own code
// POJO describing one hardware video codec. Registered so
// MediaCodecInfoUtils.getVideoCodecs()'s real array return type resolves.
class VideoCodecCapabilityStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/video/VideoCodecCapability")
    std::shared_ptr<FakeJni::JString> codec;
    std::shared_ptr<FakeJni::JString> name;
    FakeJni::JBoolean isEncoder = false;
    FakeJni::JBoolean isHardware = false;
    FakeJni::JInt maxBitrate = 0;
    FakeJni::JInt minBitrate = 0;
    FakeJni::JInt maxFps = 0;
    FakeJni::JInt minFps = 0;
    FakeJni::JInt maxWidth = 0;
    FakeJni::JInt minWidth = 0;
    FakeJni::JInt maxHeight = 0;
    FakeJni::JInt minHeight = 0;
    FakeJni::JInt maxInstances = 0;
    // Real `public String[]` fields on the real class.
    std::shared_ptr<FakeJni::JArray<std::shared_ptr<FakeJni::JString>>> profiles;
    std::shared_ptr<FakeJni::JArray<std::shared_ptr<FakeJni::JString>>> levels;
};

// com.roblox.engine.jni.video.MediaCodecInfoUtils. Real, confirmed against the app's own code.
// The engine looks both of these up every run and logs when it cannot find
// them (`E/rbx.jni: cant find method MediaCodecInfoUtils.getVideoCodecs` /
// `...hevcHardwareEncodingSupported`).
//
// Both real implementations enumerate `MediaCodecList`, Android's codec
// database. Stud's decoders are the render host's FFmpeg (see
// media_codec_forward.cpp in the render client), so the list is whatever
// that FFmpeg can decode and encode, asked of it through libmediandk at the
// moment the engine asks. An encoder is hardware when the one that opens is
// NVENC, AMF or Quick Sync, and software (x264/x265) otherwise.
// A function of Process B's libmediandk, found by name. Not through
// RTLD_DEFAULT: the library is loaded as a dependency of libroblox, which
// Stud opens with local scope, so its symbols are not in the global one and
// a default lookup finds nothing (live-caught: every codec query answered
// "none"). Opening it by name hands back the copy already loaded.
inline void* libmediandk_symbol(const char* name) {
    static void* handle = ::dlopen("libmediandk.so", RTLD_NOW);
    return handle != nullptr ? ::dlsym(handle, name) : nullptr;
}

class MediaCodecInfoUtilsStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/video/MediaCodecInfoUtils")
    static std::shared_ptr<FakeJni::JArray<std::shared_ptr<VideoCodecCapabilityStub>>>
    getVideoCodecs() {
        using SupportedFn = int (*)(const char*);
        static const auto supported =
            reinterpret_cast<SupportedFn>(libmediandk_symbol("stud_video_decoder_supported"));
        // STUD_VIDEO_CODECS narrows the list to the MIME types it names,
        // comma-separated, for seeing what the engine does with fewer.
        const char* only = std::getenv("STUD_VIDEO_CODECS");
        std::vector<std::string> mimes;
        for (const char* mime : {"video/avc", "video/hevc", "video/x-vnd.on2.vp8",
                                 "video/x-vnd.on2.vp9", "video/av01"}) {
            if (only != nullptr && *only != '\0' && std::string(only).find(mime) == std::string::npos) {
                continue;
            }
            if (supported != nullptr && supported(mime) != 0) mimes.emplace_back(mime);
        }
        // Encoders: bit 0 supported, bit 1 hardware.
        using EncoderFn = int (*)(const char*);
        static const auto encoder =
            reinterpret_cast<EncoderFn>(libmediandk_symbol("stud_video_encoder_support"));
        std::vector<std::pair<std::string, bool>> encoders;
        for (const char* mime : {"video/hevc", "video/avc"}) {
            const int support = encoder != nullptr ? encoder(mime) : 0;
            if ((support & 1) != 0) encoders.emplace_back(mime, (support & 2) != 0);
        }
        std::string names;
        for (const auto& m : mimes) names += (names.empty() ? "" : ", ") + m;
        for (const auto& [m, hw] : encoders) {
            names += (names.empty() ? "" : ", ") + m + (hw ? " encoder (hardware)" : " encoder");
        }
        std::printf("stud: MediaCodecInfoUtils.getVideoCodecs() -> %s\n",
                    names.empty() ? "none" : names.c_str());
        std::fflush(stdout);
        auto codecs = std::make_shared<FakeJni::JArray<std::shared_ptr<VideoCodecCapabilityStub>>>(
            static_cast<FakeJni::JInt>(mimes.size() + encoders.size()));
        for (size_t i = 0; i < mimes.size() + encoders.size(); ++i) {
            const bool is_encoder = i >= mimes.size();
            const std::string mime = is_encoder ? encoders[i - mimes.size()].first : mimes[i];
            auto entry = std::make_shared<VideoCodecCapabilityStub>();
            entry->codec = std::make_shared<FakeJni::JString>(mime);
            entry->name = std::make_shared<FakeJni::JString>(
                std::string(is_encoder ? "stud.ffmpeg.encoder." : "stud.ffmpeg.") + mime.substr(6));
            entry->isEncoder = is_encoder;
            // FFmpeg's decoders run on the CPU; an encoder is whatever opened.
            entry->isHardware = is_encoder && encoders[i - mimes.size()].second;
            entry->maxWidth = 3840;
            entry->minWidth = 16;
            entry->maxHeight = 2160;
            entry->minHeight = 16;
            entry->maxFps = 60;
            entry->minFps = 1;
            entry->maxBitrate = 100000000;
            entry->minBitrate = 1;
            entry->maxInstances = 4;
            entry->profiles =
                std::make_shared<FakeJni::JArray<std::shared_ptr<FakeJni::JString>>>(0);
            entry->levels = std::make_shared<FakeJni::JArray<std::shared_ptr<FakeJni::JString>>>(0);
            (*codecs)[static_cast<FakeJni::JInt>(i)] = entry;
        }
        return codecs;
    }
    // Hardware only, as the name says: a software HEVC encode of a game at
    // its own frame rate is a load the engine is asking not to take on.
    // The size and rate are within what NVENC, AMF and Quick Sync all do.
    static FakeJni::JBoolean hevcHardwareEncodingSupported(FakeJni::JInt /*width*/,
                                                            FakeJni::JInt /*height*/,
                                                            FakeJni::JInt /*fps*/) {
        using EncoderFn = int (*)(const char*);
        static const auto encoder =
            reinterpret_cast<EncoderFn>(libmediandk_symbol("stud_video_encoder_support"));
        const bool hardware = encoder != nullptr && (encoder("video/hevc") & 2) != 0;
        std::printf("stud: MediaCodecInfoUtils.hevcHardwareEncodingSupported() -> %s\n",
                    hardware ? "yes" : "no");
        std::fflush(stdout);
        return hardware;
    }
};

// com.roblox.universalapp.logging.LoggingProtocol. Real, confirmed against the app's own code.
// The engine names this gap itself, every run:
//   W/JNIMain: Failed to find LoggingProtocol class, process timestamps
//              will be inaccurate. Check proguard config.
// and carries two more strings for the same call
// ("Failed to find the LoggingProtocol::getProcessTimestamp method" /
// "getProcessTimestamp() returned -1"). It calls exactly one thing: the
// real `public static long getProcessTimestamp()`.
//
// Real semantics, from the app's own LoggingProtocol:
// milliseconds elapsed since the process started
// (`SystemClock.elapsedRealtime() - processStartElapsedRealtime`), or -1
// if the start time was never recorded. Stud can answer this exactly.
// It knows when its own process started, so this is a real value, not a
// placeholder.
class LoggingProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/logging/LoggingProtocol")
    static FakeJni::JLong getProcessTimestamp();
};

// com.roblox.universalapp.messagebus.Connection and
// com.roblox.engine.jni.memstorage.Connection, two real, confirmed against the app's own code
// classes with the same shape: a `Connection(long)` constructor wrapping a
// native pointer, plus native methods the engine registers itself. Native
// code constructs these to hand back a subscription/connection handle, so
// an unregistered class means the construction silently fails.
class MessageBusConnectionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/messagebus/Connection")
    FakeJni::JLong nativePtr = 0;
    MessageBusConnectionStub() = default;
    explicit MessageBusConnectionStub(FakeJni::JLong ptr) : nativePtr(ptr) {}
};

// The real MessageBus the Lua app publishes on. Stud needs an instance to
// pass as `this` to the exported doSubscribeRaw, and native code needs the
// class to exist to hand back a Connection.
class MessageBusStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/messagebus/MessageBus")
};

// com.roblox.universalapp.messagebus.RawCallback, a real, single-method
// interface (`void run(String)`, confirmed against the app's own code against the configured
// APK). The engine calls this back with the raw JSON of every message
// published on a subscribed topic.
class MessageBusRawCallbackStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/messagebus/RawCallback")

    void run(std::shared_ptr<FakeJni::JString> json);

    // Per-instance handler, used when more than one subscription is live:
    // Stud constructs the callback object itself, so it can hold the
    // routing rather than sharing one process-wide slot. Falls back to
    // on_message when unset, which is what the first subscriber (the
    // experience-launch request) has always used.
    std::function<void(const std::string&)> handler;

    // Set once during bring-up. Runs on whichever engine thread published
    // the message, so it must not block.
    static inline std::function<void(const std::string&)> on_message;
};

// com.roblox.protocols.webview.WebViewProtocol, registered so the
// class the engine's own web-view getters are called against resolves.
// Stud calls those getters directly rather than constructing this, but
// an unregistered class name is never cosmetic in this codebase (see
// the engineering notes' own "class is null is NOT cosmetic" lesson).
class WebViewProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/protocols/webview/WebViewProtocol")
};

// com.roblox.universalapp.messagebus.RequestHandlerRaw, the answering
// half of the bus. the app's own code shows the interface with its methods stripped;
// the real shape comes from MessageBus's own inner class that implements
// it (`public String run(String)`), so a request handler returns the JSON
// of its response rather than publishing one.
class MessageBusRequestHandlerRawStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/messagebus/RequestHandlerRaw")

    std::shared_ptr<FakeJni::JString> run(std::shared_ptr<FakeJni::JString> json);

    // Returns the response JSON. Runs on an engine thread; must not block.
    std::function<std::string(const std::string&)> handler;
};

class MemStorageConnectionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/memstorage/Connection")
    FakeJni::JLong nativePtr = 0;
    MemStorageConnectionStub() = default;
    explicit MemStorageConnectionStub(FakeJni::JLong ptr) : nativePtr(ptr) {}
};

class NativeGLJavaInterfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/NativeGLJavaInterface")
    static void onAppBridgeNotification(std::shared_ptr<FakeJni::JString> type,
                                         std::shared_ptr<FakeJni::JString> data);

    // Confirmed static getter/setter pair
    // (`NativeGLJavaInterface`): the real field
    // (`sDeviceStaticParams`) is `private`, but the class also declares
    // a real, `public static` getter, `getDeviceStaticParams()`, the
    // idiomatic way native code would read it via JNI reflection.
    // Gap found in testing this session (the engineering notes, "get Stud
    // rendering the real Lua UI" plan): `RobloxApplication.onCreate()`'s
    // real, confirmed-taken branch (flag `EnableGameActivity8` defaults
    // `false`) calls `setDeviceStaticParams(...)` unconditionally.
    // Stud never replicated this, so any later native call to
    // `getDeviceStaticParams()` would silently get null instead of a
    // real object, matching the exact class of bug already fixed twice
    // this session (`getResources()`/`getNativeHelper()`). Populated
    // once, at bring-up, in `main.cpp`.
    static std::shared_ptr<DeviceStaticParamsStub> getDeviceStaticParams() {
        return s_device_static_params;
    }
    static void setDeviceStaticParams(std::shared_ptr<DeviceStaticParamsStub> params) {
        s_device_static_params = std::move(params);
    }

    // Confirmed soft-keyboard callbacks
    // (`NativeGLJavaInterface`). The engine calls
    // `showKeyboard()` the moment a real Lua TextBox takes focus, handing
    // over the TextBox's own native handle; on a real device that opens
    // the IME, and every subsequent keystroke goes back through
    // `NativeGLInterface.nativePassText(handle, fullText, done, cursor)`
    // (`RbxKeyboard`), NOT through key events. Stud never
    // registered either method, so the engine's real request was silently
    // dropped (live-observed as `GetMethodID MISS ... showKeyboard`) and
    // typing could never reach the app. Stud has no IME: it records the
    // focused handle and its initial text here, and the input bridge
    // replays real physical keystrokes through `nativePassText`.
    static void showKeyboard(FakeJni::JLong text_box, FakeJni::JBoolean show_native_input,
                              std::shared_ptr<FakeJni::JByteArray> initial_text,
                              std::shared_ptr<NativeTextBoxInfoStub> info);
    static void hideKeyboard();

    // The rest of the real NativeGLJavaInterface surface libroblox looks up
    // and, until now, never found. Every one of these was a live
    // `STUD_DIAG GetMethodID MISS class=com/roblox/engine/jni/
    // NativeGLJavaInterface static method=...` in an ordinary run of the app
    // this project's own hard-won lesson is that a miss is never cosmetic:
    // the call is silently dropped and whatever depended on it never happens.
    // Signatures are the ones the engine itself asked for, verbatim.
    //
    // Bodies are honest: Stud logs what it was told and does nothing where it
    // genuinely has nothing to do (no billing, no WebView, no photo album, no
    // VR, no ad id). Claiming otherwise would make the engine follow a path
    // that cannot work.
    static void gameLoadedCallback(FakeJni::JLong placeId);
    static void gameDidLeave();
    static void exitGameWithError(FakeJni::JInt error);
    static void onAppShellReloadNeeded();
    static void onDataModelNotificationCallback(std::shared_ptr<FakeJni::JString> type,
                                                 std::shared_ptr<FakeJni::JString> data);
    // The user asked to quit: the app shell's own Exit button.
    //
    // The engine announces it as a NATIVE_EXIT data-model notification and
    // then ignores its own notification ("Ignore notification: type
    // (NATIVE_EXIT, 35)"), on a real device closing the app is the Java
    // layer's job, and a phone has no Exit button to press in the first
    // place. Stud shows one because it presents as a desktop client, so
    // Stud is what has to act on it.
    static inline std::function<void()> on_native_exit;
    static void onLuaTextBoxChangedCallback(std::shared_ptr<FakeJni::JString> value);
    static void onLuaTextBoxPropertyChangedCallback();
    static void onVrSessionStateUpdate(FakeJni::JInt state);
    static void onExtendedAnalyticsRecvCallback(std::shared_ptr<FakeJni::JByteArray> data,
                                                 FakeJni::JInt length);
    static void screenOrientationChanged(FakeJni::JInt orientation);
    static void listenToMotionEvents(std::shared_ptr<FakeJni::JString> motionType);
    static void openNativeOverlay(std::shared_ptr<FakeJni::JString> a,
                                   std::shared_ptr<FakeJni::JString> b);
    static void saveImageToAlbum(std::shared_ptr<FakeJni::JString> path);
    static void getWebViewUserAgent();
    static void getMobileAdvertisingId();
    static void promptNativePurchase(FakeJni::JLong id, std::shared_ptr<FakeJni::JString> a);
    static void promptNativePurchase2(FakeJni::JLong id, std::shared_ptr<FakeJni::JString> a,
                                       std::shared_ptr<FakeJni::JString> b);
    static void promptNativePurchaseWithPayload(FakeJni::JLong id,
                                                 std::shared_ptr<FakeJni::JString> a,
                                                 std::shared_ptr<FakeJni::JString> b);
    static void promptNativePurchaseWithPaymentSessionId(FakeJni::JLong id,
                                                          std::shared_ptr<FakeJni::JString> a,
                                                          std::shared_ptr<FakeJni::JString> b);
    static void promptNativePurchaseWithPaymentSessionId3(FakeJni::JLong id,
                                                           std::shared_ptr<FakeJni::JString> a,
                                                           std::shared_ptr<FakeJni::JString> b,
                                                           std::shared_ptr<FakeJni::JString> c);

    // Live handle of the focused TextBox, or 0 when none. Read by the
    // input bridge (runtime/src/input_bridge.cpp).
    static long active_text_box();
    static std::string active_text_box_text();
    static void set_active_text_box_text(std::string text);

    // How the focused TextBox wants its text drawn, straight out of the
    // NativeTextBoxInfo the engine hands to showKeyboard. Stud has to
    // draw that text itself (the engine stops drawing it while a native
    // input is up; see the text-input entry in the engineering notes), so
    // every one of these is load-bearing rather than diagnostic.
    struct TextBoxStyle {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float font_size = 0.0f;
        int font = 0;
        unsigned color = 0xffffffffu;
        int x_alignment = 0;
        int y_alignment = 0;
        bool password = false;
    };
    static TextBoxStyle active_text_box_style();
    static void set_active_text_box_style(const TextBoxStyle& style);

private:
    static std::shared_ptr<DeviceStaticParamsStub> s_device_static_params;
};

// com.snapchat.djinni.NativeObjectManager. Real, confirmed against the app's own code class
// (): Djinni's own generic native-object lifecycle
// tracker (a PhantomReference/ReferenceQueue-based GC-notification
// singleton; when a Java-side wrapper object is collected, it calls that
// object's own class's static `nativeDestroy(long)`). Real root cause
// found this session (the engineering notes, "root cause found", the
// long-standing `getClassLoader`/null-jclass mystery): `FakeJni`'s own
// `FindClass()` (fake-jni.cpp) calls jnivm's `InternalFindClass()` with
// `returnZero=true`, unlike raw jnivm's own default, this does NOT
// auto-vivify an unregistered class, it returns a genuine null. This
// class was never registered as a `FakeJni::JObject` stub anywhere in
// this codebase, so every real `FindClass("com/snapchat/djinni/
// NativeObjectManager")` call (part of Djinni's own generic classloader-
// bootstrap idiom, confirmed in the engine much earlier this session) came
// back null, and everything chained off that result (GetObjectClass,
// GetMethodID("getClassLoader", ...)) was null too, purely as a
// downstream consequence, not a cast/lifetime bug in jnivm itself.
// Registering this class (even with a minimal, honest no-op body for
// `register()`. Stud has no real DEX/ART GC to hook this into, so
// actually freeing native objects on Java-side collection isn't
// meaningful here yet) is the real, targeted fix: it lets `FindClass`
// succeed via the normal registered-class lookup path instead of the
// no-auto-vivify branch, which is all Djinni's own bootstrap idiom
// actually needs to proceed correctly.
class NativeObjectManagerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/snapchat/djinni/NativeObjectManager")
    // Real signature: static void register(Object obj, long nativePtr).
    // The real Java source's own 3-arg overload (register(Object, Class,
    // long)) just calls this one after resolving obj.getClass() itself,
    // so this 2-arg form covers the real call shape reflection-visible
    // native code would invoke. Honest no-op: acknowledges the
    // registration (real Android would enqueue this for eventual native
    // cleanup on GC) without pretending to actually free anything, since
    // Stud has no real GC to drive that with yet.
    static void nativeObjectRegister(std::shared_ptr<FakeJni::JObject> obj, jlong native_ptr);
    static void nativeObjectStop();
};

// Direct evidence (this session): running the full real bootstrap
// end-to-end: real stud-render-host, real fetched ClientSettings, a
// real stored Roblox session cookie; every one of these static
// method lookups fails with jnivm's own "class is null" (i.e. the
// FindClass() for whatever class declares them was never registered,
// so there was nothing to look the method up on), for the entire
// lifetime of every real run so far, cookie or no cookie. The
// four-call V2 app-bridge sequence (engine_v2_bridge.cpp) never
// signals completion in any of those runs either. Not proven causally
// linked yet (no the app's own code available this session to independently confirm
// the Java-side call graph, flagged honestly, matching this file's
// own established practice for inferred-not-confirmed pieces), but a
// real, concrete, previously-overlooked gap either way: this is Stud
// failing to "house" Roblox as a real Android app the way real Android
// does; these are real user/device/locale/analytics accessor
// methods Roblox's native engine calls INTO Java for (the reverse
// direction from GameActivityStub et al above, which Java calls INTO
// native for), and until now none of their declaring classes existed
// in this FakeJni environment at all.
//
// Class names are real, confirmed against the embedded strings of the actual
// extracted libroblox.so (checked): com/roblox/engine/jni/user/
// NativeUserJavaInterface, com/roblox/engine/jni/locale/
// NativeLocaleJavaInterface, com/roblox/engine/jni/reporter/
// SessionReporterJavaInterface. Method-to-class assignment is inferred
// from name semantics (locale methods on the locale interface, session
// reporting on the reporter interface, everything else on the user
// interface), plausible and internally consistent, but not
// independently verified against the app's own code against real Java source the way this
// file's other classes' method signatures are. Return values are
// deliberately conservative, real-looking placeholders (empty/zero/
// false where no real value is knowable locally), not an attempt to
// impersonate a real Roblox account's actual data.
class NativeUserJavaInterfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/user/NativeUserJavaInterface")

    // Real authenticated user id/username/displayName, threaded in from
    // main() (set_native_user_identity() below) when a real
    // LaunchPayload with a real fetched identity is available.
    // Real, evidence-based fix (this session): a read of the engine's own code
    // traced StartAppWithParams's crash to UserController::didLogin()
    // dereferencing a null singleton, and the leading hypothesis is
    // that real native code reads a placeholder userId of 0 as "not
    // logged in" and never constructs UserController as a result. Falls
    // back to the same honest placeholders as before when no real
    // identity was fetched (no cookie, or the fetch failed).
    static FakeJni::JLong getUserId();
    static FakeJni::JBoolean getIsUnder13() { return false; }
    static std::shared_ptr<FakeJni::JString> getUsername();
    static std::shared_ptr<FakeJni::JString> getDisplayName();
    static std::shared_ptr<FakeJni::JString> getAlternateName() {
        return std::make_shared<FakeJni::JString>("");
    }
    // Real Android build (this is genuinely an Android build of the
    // client, just hosted outside Android), not "Linux"/"Stud",
    // which real server-side platform validation almost certainly
    // doesn't recognize.
    static std::shared_ptr<FakeJni::JString> getPlatformName() {
        return std::make_shared<FakeJni::JString>("Android");
    }
    // Real Roblox membership-type enum's real default value (0 ==
    // None/free account): a public, documented value, checked.
    static FakeJni::JInt getMembershipType() { return 0; }
    static FakeJni::JBoolean getHasRobloxSubscription() { return false; }
    static std::shared_ptr<FakeJni::JString> getTheme() {
        return std::make_shared<FakeJni::JString>("Light");
    }
    static std::shared_ptr<FakeJni::JString> getLastLoggedInUser() {
        return std::make_shared<FakeJni::JString>("");
    }
    static std::shared_ptr<FakeJni::JString> getLastLoggedInUserId() {
        return std::make_shared<FakeJni::JString>("");
    }
    // Real Context.getFilesDir() equivalent, returns whatever real,
    // writable, already-bound-into-the-sandbox path this process was
    // actually given (set_files_dir() below), never a hardcoded guess.
    static std::shared_ptr<FakeJni::JString> getFilesDir();
    // The real versionName of the APK the user actually configured, read
    // from its own AndroidManifest.xml at startup (see
    // set_real_app_version()). This used to be a hardcoded literal, in
    // seven places, which silently kept claiming an old build after the
    // user switched APKs.
    static std::shared_ptr<FakeJni::JString> getAppVersion() {
        return std::make_shared<FakeJni::JString>(real_app_version());
    }
    static FakeJni::JBoolean isDebuggerConnected() { return false; }
    static void setEventTrackingGoogleAnalytics(std::shared_ptr<FakeJni::JString>,
                                                 std::shared_ptr<FakeJni::JString>,
                                                 std::shared_ptr<FakeJni::JString>, FakeJni::JLong) {}
};

// Writable files-dir path threaded in from main() (process-b's
// own already-computed cache_subdir("files")), called once, before
// register_game_activity_stubs()'s classes can be queried.
void set_native_user_interface_files_dir(std::string path);

// com.roblox.engine.jni.util.NetworkUtils. Real, confirmed against the app's own code class
// (), found this session via a systematic real-binary
// string sweep, checked. This is the REAL, correct owner of
// `getPublicIPv4Addresseses()`, a real, live-confirmed "class is
// null" gap this project's history had long, wrongly assumed belonged
// to `NativeUserJavaInterfaceStub` (a plausible-looking guess, since
// nothing had ever traced the ACTUAL declaring class), and repeatedly
// documented as a mysterious, unexplained resolution failure even
// after the method was "correctly implemented" there. It never could
// have worked: `GetStaticMethodID` matches on the class actually
// looked up, and `com/roblox/engine/jni/util/NetworkUtils` was never
// registered at all. Real signature: static, no args, returns String
// (a real, best-effort colon-joined local-interface-address list on a
// real device. Stud has no real network-interface enumeration
// equivalent wired yet, so an honest empty string, matching this
// method's own real catch-block fallback for when enumeration fails).
class NetworkUtilsStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/util/NetworkUtils")
    static std::shared_ptr<FakeJni::JString> getPublicIPv4Addresseses() {
        return std::make_shared<FakeJni::JString>("");
    }
};

// com.roblox.engine.jni.NativeQuoteInterface. Real, confirmed against the app's own code
// class (), found via the same real-binary systematic
// sweep as NetworkUtils above. `requestResponse(byte[])` is a real,
// security-sensitive hardware key-attestation entry point (real
// AndroidKeyStore/StrongBox-backed attestation quote, the kind of
// mechanism anti-cheat/integrity systems use); Stud has no real TEE/
// StrongBox to attest with, so the only honest, correct answer is the
// real APP-DEFINED failure response format (confirmed from the real
// real method body's own catch block: byte 1 = version, byte 0 =
// failure, followed by a UTF-8 error string), the exact same
// response a real device with no working secure hardware would
// produce, not a fabricated/spoofed successful attestation. Never
// change this to fake a successful response; that would mean
// defeating a real integrity check rather than honest interoperability.
class NativeQuoteInterfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/NativeQuoteInterface")
    static std::shared_ptr<FakeJni::JByteArray> requestResponse(
        std::shared_ptr<FakeJni::JByteArray> /*challenge*/) {
        static const char kMessage[] = "Stud: no real hardware key attestation available";
        constexpr jint kMessageLen = sizeof(kMessage) - 1;
        auto result = std::make_shared<FakeJni::JByteArray>(2 + kMessageLen);
        (*result)[0] = 1;
        (*result)[1] = 0;
        for (jint i = 0; i < kMessageLen; ++i) {
            (*result)[2 + i] = static_cast<FakeJni::JByte>(kMessage[i]);
        }
        return result;
    }
};

// Confirmed (`com.roblox.client.LocalStorageManager`):
// `getAllocatableBytes()` is real @Keep, called by native code to check
// real available disk space before writing cache/download data (real
// body: `new StatFs(Environment.getDataDirectory().getPath())
// .getAvailableBytes()`). Honest, real answer via `statvfs()` on Stud's
// own real cache root (`~/.cache/stud`, same real filesystem Stud
// actually writes cache data to), not a fabricated/hardcoded value.
//
// It is an INSTANCE method on a Kotlin `object` (real declaration:
// `public final long getAllocatableBytes()`), not a static, so the
// native side resolves it with GetMethodID and calls it on the `this`
// handed to initStorageManagerNativeV3. Registering it static made that
// lookup miss.
class LocalStorageManagerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/LocalStorageManager")
    jlong getAllocatableBytes();
};

// Confirmed (`com.roblox.client.game.ExperienceSession`):
// `shouldDisableExperienceIdleTimer()` is real @Keep, static; real
// body checks screen-recording/media-capture/age-estimation state Stud
// has no equivalent of. Honest `false` (idle timer not disabled),
// matches every other "capability Stud doesn't have" honest default in
// this file, checked at real behavior.
class ExperienceSessionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/game/ExperienceSession")
    static FakeJni::JBoolean shouldDisableExperienceIdleTimer() { return false; }
};

// Confirmed (`com.roblox.universalapp.appratingprompt.
// AppRatingPromptHandler`): both real @Keep, static.
// `isAppRatingPromptAvailable()`'s own code unconditionally returns
// `true` on a real device, honest `false` here instead, since Stud
// has no real app-rating prompt UI/store integration to actually show
// one (same "don't claim a capability Stud doesn't have" reasoning as
// NativeQuoteInterfaceStub). `showAppRatingPrompt()` is a real no-op
// here for the same reason.
class AppRatingPromptHandlerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/appratingprompt/AppRatingPromptHandler")
    static FakeJni::JBoolean isAppRatingPromptAvailable() { return false; }
    static void showAppRatingPrompt() {
        std::printf("stud: AppRatingPromptHandler.showAppRatingPrompt() (no-op, no real prompt UI)\n");
    }
};

// Confirmed (`com.roblox.client.purchase.IAPPurchaseManager`
// ): 4 real @Keep static entry points backing Roblox's real
// "PaymentsProtocol" native-to-Java purchase flow (real store type
// detection + purchase invocation). Stud has no real store/IAP backend
// to forward to, honest empty/false/no-op answers throughout, same
// "don't claim a capability Stud doesn't have" reasoning as
// NativeQuoteInterfaceStub/AppRatingPromptHandlerStub above.
class IAPPurchaseManagerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/purchase/IAPPurchaseManager")
    static std::shared_ptr<FakeJni::JString> getPlatformPaymentMethod() {
        return std::make_shared<FakeJni::JString>("");
    }
    static std::shared_ptr<FakeJni::JString> getPlatformPaymentProviderType() {
        return std::make_shared<FakeJni::JString>("");
    }
    static FakeJni::JBoolean invokeStore(std::shared_ptr<FakeJni::JString> /*str*/,
                                          std::shared_ptr<FakeJni::JString> /*str2*/) {
        std::printf("stud: IAPPurchaseManager.invokeStore() (no-op, no real store)\n");
        return false;
    }
    static void invokeStoreV2(std::shared_ptr<FakeJni::JString> /*str*/, jlong /*j10*/,
                               std::shared_ptr<FakeJni::JString> /*str2*/) {
        std::printf("stud: IAPPurchaseManager.invokeStoreV2() (no-op, no real store)\n");
    }
};

// Confirmed (`com.roblox.engine.jni.util.AssertDialogUtil`
// ): `showAssertionPopup(String)` is real @Keep, static, native
// engine assertion-failure hook. The app's own defined fallback for
// "no activity set" (Stud's actual situation, no real dialog UI) is to
// log the assertion and return 0, matched exactly here, checked.
class AssertDialogUtilStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/util/AssertDialogUtil")
    static FakeJni::JInt showAssertionPopup(std::shared_ptr<FakeJni::JString> str) {
        std::fprintf(stderr, "stud: AssertDialogUtil assertion failed (no dialog UI): %s\n",
                     str ? str->asStdString().c_str() : "");
        return 0;
    }
};

// Confirmed (`com.roblox.universalapp.systemtheme.
// SystemThemeProtocol`): both real @Keep, static.
// `isSystemThemeAvailable()`'s own code is `Build.VERSION.SDK_INT >=
// 29`. Stud's own BuildVersionStub.SDK_INT is 34 (see
// android_framework_stubs.h), so `true` here is the internally
// consistent, honest answer, checked. `getSystemTheme()`'s real
// int return values (confirmed: ERROR=0, SYSTEM_LIGHT=3,
// SYSTEM_DARK=4). Stud never sets a real `contextRef`, so the
// correct value is the real class's own defined "no context" fallback,
// ERROR(0), exactly matching what a real device would return in this
// same situation, not a fabricated theme.
class SystemThemeProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/systemtheme/SystemThemeProtocol")
    // The desktop's own light/dark setting, in the real enum's terms
    // (SYSTEM_LIGHT 3 / SYSTEM_DARK 4). This used to return ERROR(0),
    // which was the honest answer while Stud had no way to know; it now
    // does, so ERROR would be a worse answer than the truth. See
    // system_theme_bridge.h for where the value comes from and what the
    // app does with it.
    static FakeJni::JInt getSystemTheme();
    static FakeJni::JBoolean isSystemThemeAvailable() { return true; }
};

// com.roblox.universalapp.systemtheme.JNISystemThemeProtocol; real,
// confirmed against THIS APK: four static natives and nothing else
// (getProtocolName, getSystemThemeParamKey, getSystemThemeSetThemeMessage,
// getSystemThemeUpdatedMessage), which is how the theme bridge learns
// every wire name instead of hardcoding it.
//
// No methods are declared here on purpose: the bodies are libroblox's
// own, called directly through their exported symbols
// (system_theme_bridge.cpp), and this class exists so the `jclass` those
// calls are handed is a real one. FakeJni's FindClass returns a genuine
// null for anything unregistered, so without this the bridge passed null
// which happened to work, since a static native ignores its jclass,
// but left a real `FindClass -> raw jclass=0x0` in every run and would
// break the moment anything did use it.
class JNISystemThemeProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/systemtheme/JNISystemThemeProtocol")
};

// Signature-matching-only stub (same pattern already established
// in this file for DeviceDisplayCapabilityStub/DesignTokensStub): real
// class name `java/nio/ByteBuffer`, needed only so
// NativeHelperStub::gameActivity_onFlagsLoaded's real parameter type
// resolves to a correct JNI method signature; nothing here ever reads
// or writes an actual buffer's contents.
// NOTE: there is deliberately no `ByteBufferStub` here any more. jnivm
// already ships its own real `jnivm::ByteBuffer` and registers it under
// exactly this class name itself (`vm.cpp`'s own
// `env->GetClass<ByteBuffer>("java/nio/ByteBuffer")`), and that is the
// concrete type a real `NewDirectByteBuffer()` call actually produces.
// Stud used to declare a second, separate stub class for the same real
// Java class name, which could never match: `UnpackJObject<T>`'s own
// `dynamic_cast` from the real `jnivm::ByteBuffer` to Stud's unrelated
// stub type always failed and threw `Invalid Reference, Unexpected
// Type`. That throw was fatal in practice, libroblox checks for a
// pending JNI exception, logs `RBXCRASH: JNI: Crashing due to unhandled
// Java exception` and traps, which killed the very thread the engine
// had designated as its own internal "main" thread, after which nothing
// ever drained its task queue again (see engine_thread.h). Use
// `jnivm::ByteBuffer` directly instead; it carries the correct real
// class name, so the JNI method signature still resolves correctly.

// Confirmed (`com.roblox.client.startup.NativeHelper`):
// a real, non-static Java class MainGameActivity constructs once
// (`this.nativeHelper = new NativeHelper(this, ...)`, real constructor
// call in `MainGameActivity`'s own `onCreate()`) and holds as a plain
// field (`public final NativeHelper nativeHelper`); real native code
// that already has a `jobject` reference to the Activity (which Stud
// always hands it, same as a real device) can reach this object via a
// real `GetObjectField(activity, "nativeHelper", ...)` and call any of
// its real `@Keep` callback methods directly, exactly the same "native
// calls back into a Java object it was hardly ever told about
// explicitly, just handed a field/return value" pattern this whole
// project's own framework-completeness sweep keeps finding (see
// NetworkUtilsStub, NativeGLJavaInterfaceStub, etc.).
//
// All 23 real @Keep methods below (confirmed against the app's own code method signatures,
// checked) are honest no-ops beyond a log line, their real Java
// bodies mostly just marshal into other, deeper app subsystems (push
// notifications, purchase prompts, keyboard/IME, screenshot saving,
// app-store update flow) Stud has no equivalent of and doesn't need for
// "the engine can call this without hitting a null class/method ID"
// (this file's own standing goal, see NetworkUtilsStub's doc comment).
// A 24th real @Keep method, `onCodeParsedFromSMS`, is deliberately
// NOT included here. Its real dispatch mechanism is a Java EventBus
// (a main-thread subscriber, greenrobot-style `post()`),
// never a direct native JNI call, so it isn't part of this class's real
// JNI-reachable surface at all.
class NativeHelperStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/startup/NativeHelper")

    void gameActivity_hideKeyboard() {
        std::printf("stud: NativeHelper.gameActivity_hideKeyboard()\n");
    }
    void gameActivity_onAppReady(std::shared_ptr<FakeJni::JString> data) {
        std::printf("stud: NativeHelper.gameActivity_onAppReady: data=%s\n",
                    data ? data->asStdString().c_str() : "");
        // Back in the app shell, so no experience is on screen any more.
        // Without this the flag stays set for the rest of the session
        // after a single game, and the home screen keeps the in-experience
        // input behaviour, live-caught as the avatar editor jumping
        // again once a game had been played.
        experience_loaded_storage().store(false);
    }
    void gameActivity_onDidLogInReceived(std::shared_ptr<FakeJni::JString> data) {
        std::printf("stud: NativeHelper.gameActivity_onDidLogInReceived: data=%s\n",
                    data ? data->asStdString().c_str() : "");
        std::fflush(stdout);
        if (on_account_changed) on_account_changed("login");
    }
    void gameActivity_onDidLogOutReceived() {
        std::printf("stud: NativeHelper.gameActivity_onDidLogOutReceived()\n");
        std::fflush(stdout);
        if (on_account_changed) on_account_changed("logout");
    }
    void gameActivity_onDidSignUp(std::shared_ptr<FakeJni::JString> data) {
        std::printf("stud: NativeHelper.gameActivity_onDidSignUp: data=%s\n",
                    data ? data->asStdString().c_str() : "");
    }
    void gameActivity_onDidSwitchAccountReceived() {
        std::printf("stud: NativeHelper.gameActivity_onDidSwitchAccountReceived()\n");
        std::fflush(stdout);
        if (on_account_changed) on_account_changed("switch");
    }
    void gameActivity_onEngineInitialized() {
        std::printf("stud: NativeHelper.gameActivity_onEngineInitialized()\n");
    }
    // The platform's cue to actually start the game. Ground truth from a real,
    // successful Sober join: right after the engine reaches setStage:UGCGame
    // and reports "No DM yet", the platform side acknowledges
    // (Sober logs its own `app_interface$json: {"type":"did_handle_start_game"}`),
    // and only then does the engine create its NetworkClient and log
    // "! Joining game ... at <ip>". Stud never acknowledged, so the engine sat
    // on the loading screen forever with a DataModel that was never created.
    void gameActivity_onExperienceStart() {
        std::printf("stud: NativeHelper.gameActivity_onExperienceStart()\n");
        std::fflush(stdout);
        if (on_experience_start) on_experience_start();
    }
    // Set once during bring-up (runtime/src/main.cpp). Runs on the engine's own
    // callback thread, so it must not block; see the call site.
    static inline std::function<void()> on_experience_start;
    static std::atomic<long long>& place_id_storage() {
        static std::atomic<long long> id{0};
        return id;
    }
    void gameActivity_onExperienceStop(double timeInMs) {
        std::printf("stud: NativeHelper.gameActivity_onExperienceStop: timeInMs=%f\n", timeInMs);
        std::fflush(stdout);
        // The engine's own "the experience has stopped" signal, and the
        // only one that fires on the way back to the home screen.
        //
        // This flag used to be cleared by onAppReady alone, which fires
        // while the app shell is starting and NOT again on a return from a
        // game, so after one game it stayed set for the rest of the
        // session. Everything keyed on it then behaved as though a game
        // were still running: the Discord presence kept showing the game
        // that had been left, and the server-region check kept sampling
        // sockets after the game's own had closed, reporting whatever
        // unrelated peer was left as the "server region".
        // The callback runs BEFORE the flag is cleared, deliberately: it
        // is how a listener tells "returned from a real experience" apart
        // from "the app shell is starting", and the other caller
        // (onLuaAppDidReturn) does not clear anything. Clearing first
        // would make this path indistinguishable from startup.
        if (on_returned_to_app) on_returned_to_app();
        experience_loaded_storage().store(false);
        place_id_storage().store(0);
    }
    void gameActivity_onFlagsFailed() {
        std::printf("stud: NativeHelper.gameActivity_onFlagsFailed()\n");
    }
    void gameActivity_onFlagsLoaded(std::shared_ptr<jnivm::ByteBuffer> /*flagsBuffer*/) {
        std::printf("stud: NativeHelper.gameActivity_onFlagsLoaded()\n");
    }
    // Whether an experience is loaded, as opposed to the app shell.
    //
    // Input needs this: the in-game camera script pins the mouse while a
    // rotation drag is held (Enum.MouseBehavior.LockCurrentPosition) and
    // the app shell never does, but the engine reports neither; its one
    // exported predicate answers for LockCenter only, and that stays false
    // through an ordinary rotation. So the platform has to hold the
    // pointer still for a drag in an experience and leave it free on the
    // home screen, and this is how it tells them apart.
    static bool experience_is_loaded() { return experience_loaded_storage().load(); }

    void gameActivity_onGameLoaded(jlong placeId) {
        std::printf("stud: NativeHelper.gameActivity_onGameLoaded: placeId=%lld\n",
                    static_cast<long long>(placeId));
        std::fflush(stdout);
        // The engine names the experience it is loading here. Stud's
        // acknowledgement of a start-game request has to say WHICH
        // experience, and with no deep link it had nothing but 0 to send,
        // which the server rejects as "not authorized to join this
        // experience". This is the engine's own answer to that question,
        // so remember it; 0 is the engine's own "no experience" value and
        // is not worth remembering over a real one.
        // Only a real experience counts. The engine calls this with 0 on
        // the home screen at startup, and setting the flag there made the
        // app shell take the in-experience input path from the moment Stud
        // opened, which is the avatar editor jumping before any game had
        // been played at all.
        if (placeId > 0) {
            set_last_place_id(static_cast<long long>(placeId));
            experience_loaded_storage().store(true);
        }
    }

    // Written from the engine's own callback thread and read from the
    // input thread, hence atomic.
    static std::atomic<bool>& experience_loaded_storage() {
        static std::atomic<bool> loaded{false};
        return loaded;
    }

    // The last real experience the engine named, or 0 if it has not named
    // one yet. Written from the engine's own callback thread and read from
    // the acknowledgement thread, hence atomic.
    static long long last_place_id() { return place_id_storage().load(); }
    static void set_last_place_id(long long id) { place_id_storage().store(id); }

    // The rest of the launch request the Lua app published, kept exactly
    // as the real client keeps it: the app's own launch-request parser reads all of these out of the
    // same JSON and the app's own app-shell helper feeds them to StartGameParams. Sending only
    // the place id is what a "not authorized to join this experience"
    // answer looks like from the other side.
    struct LaunchRequest {
        long long place_id = 0;
        long long referred_by_player_id = 0;
        std::string join_attempt_id;
        std::string join_attempt_origin;
        std::string game_join_context;
        std::string launch_data;
        std::string event_id;
        std::string access_code;
        std::string link_code;
        std::string game_instance_id;
    };
    // Defined after the struct they hold, and private only by convention
    // the accessors below are the API.
    static std::mutex& launch_request_mutex() {
        static std::mutex m;
        return m;
    }
    static LaunchRequest& launch_request_storage() {
        static LaunchRequest r;
        return r;
    }

    static LaunchRequest last_launch_request() {
        std::lock_guard<std::mutex> lock(launch_request_mutex());
        return launch_request_storage();
    }
    static void set_last_launch_request(LaunchRequest request) {
        {
            std::lock_guard<std::mutex> lock(launch_request_mutex());
            launch_request_storage() = std::move(request);
        }
        set_last_place_id(launch_request_storage().place_id);
    }
    void gameActivity_onGameStreamingStatusChanged(std::shared_ptr<FakeJni::JString> data) {
        std::printf("stud: NativeHelper.gameActivity_onGameStreamingStatusChanged: data=%s\n",
                    data ? data->asStdString().c_str() : "");
    }
    void gameActivity_onLuaAppDidReturn() {
        std::printf("stud: NativeHelper.gameActivity_onLuaAppDidReturn()\n");
        std::fflush(stdout);
        if (on_returned_to_app) on_returned_to_app();
    }

    // Fired whenever the Lua app comes back to the foreground, leaving a
    // game, or an experience ending. Stud re-asserts the engine's task
    // scheduler to foreground there, because the engine drops to its low
    // background frequency and nothing else tells it otherwise. Set once
    // during bring-up (runtime/src/main.cpp).
    static inline std::function<void()> on_returned_to_app;

    // Fired whenever the signed-in account changes: a login, a logout,
    // or a switch between accounts. Stud uses it to look at the engine's
    // own cookie jar at that moment: a web-view panel opened afterwards
    // has to carry the account the user is NOW on, and a newly issued
    // session has to be persisted or the account is gone next launch.
    // The argument is which of the three happened, for the log only.
    static inline std::function<void(const char*)> on_account_changed;
    void gameActivity_onLuaTextBoxChanged(std::shared_ptr<FakeJni::JString> /*value*/) {
        std::printf("stud: NativeHelper.gameActivity_onLuaTextBoxChanged()\n");
    }
    void gameActivity_onLuaTextBoxPropertyChanged() {
        // Once per property change on a focused TextBox, so typing a
        // sentence prints a line per keystroke, 126 of them in one
        // session, paired with the callback below. Behind the same
        // switch as the rest of the input tracing.
        if (text_input_trace_enabled()) {
            std::printf("stud: NativeHelper.gameActivity_onLuaTextBoxPropertyChanged()\n");
        }
    }
    void gameActivity_onMotionEventListening(std::shared_ptr<FakeJni::JString> /*motionType*/) {
        std::printf("stud: NativeHelper.gameActivity_onMotionEventListening()\n");
    }
    void gameActivity_onRestartLuaApp() {
        std::printf("stud: NativeHelper.gameActivity_onRestartLuaApp()\n");
    }
    void gameActivity_onScanQrCode() {
        std::printf("stud: NativeHelper.gameActivity_onScanQrCode()\n");
    }
    void gameActivity_onScreenOrientationChanged(FakeJni::JInt orientation,
                                                  FakeJni::JBoolean inExperience) {
        std::printf("stud: NativeHelper.gameActivity_onScreenOrientationChanged: orientation=%d "
                    "inExperience=%d\n",
                    static_cast<int>(orientation), static_cast<int>(inExperience));
    }
    // A screenshot or a screen recording, finished and in the engine's own
    // storage. Android's Java copies it into the gallery and then reports
    // back with nativeImageSavedToAlbumFinished; the engine waits for that
    // report. See album_bridge.h. Set during bring-up.
    static inline std::function<void(const std::string&)> on_capture_ready;
    void gameActivity_onScreenshotReady(std::shared_ptr<FakeJni::JString> path) {
        const std::string file = path ? path->asStdString() : std::string();
        std::printf("stud: NativeHelper.gameActivity_onScreenshotReady: path=%s\n",
                    file.c_str());
        std::fflush(stdout);
        if (on_capture_ready && !file.empty()) on_capture_ready(file);
    }
    void gameActivity_setAppUpgradeStatus(FakeJni::JInt result, FakeJni::JInt upgradeStatusCode,
                                           std::shared_ptr<FakeJni::JString> /*apkUrl*/,
                                           std::shared_ptr<FakeJni::JString> /*apkCheckSum*/) {
        std::printf("stud: NativeHelper.gameActivity_setAppUpgradeStatus: result=%d "
                    "upgradeStatusCode=%d\n",
                    static_cast<int>(result), static_cast<int>(upgradeStatusCode));
    }
    void gameActivity_showKeyboard(jlong /*textBox*/, FakeJni::JBoolean /*showNativeInput*/,
                                    std::shared_ptr<FakeJni::JByteArray> /*byteArrayFromTextBox*/,
                                    std::shared_ptr<NativeTextBoxInfoStub> /*textBoxInfo*/) {
        std::printf("stud: NativeHelper.gameActivity_showKeyboard()\n");
    }
};

// Real authenticated user identity, threaded in from main() (via
// LaunchPayload::authenticated_user_id/username/display_name); see
// NativeUserJavaInterfaceStub::getUserId()'s own doc comment.
// `user_id == 0` (the default) means "no real identity available",
// matching every other honest-placeholder default in this file.
void set_native_user_identity(long long user_id, std::string username, std::string display_name);

// Real accessors onto the same identity set_native_user_identity()
// stores, for build_desktop_start_app_params() (start_app_params.cpp)
// to use, since UserController::didLogin() itself
// (confirmed in the engine this session) reads username/userId/etc. directly
// off the StartAppParams object it's handed, not via a fresh
// NativeUserJavaInterface JNI call.
long long native_user_id();
std::string native_username();
std::string native_display_name();
std::string native_user_interface_files_dir();

// Confirmed-embedded class-name string in libroblox.so itself
// (an embedded-string scan of the real binary, not a @Keep guess;
// see this file's own doc history for how this sweep technique found
// NativeHelperStub etc.): `com.roblox.engine.jni.reporter.
// SessionReporterJavaInterface` is a real class real native code does
// at minimum `FindClass()` on, plausibly to read real session-identity
// getters during boot/session-report bring-up. Confirmed against the app's own code
// method surface (`getAppVersion`/`getFilesDir`/`getLastLoggedInUser`/
// `getLastLoggedInUserId`/`sendSessionReport`/
// `setEventTrackingGoogleAnalytics`), honest answers reusing the same
// real identity/files-dir state already threaded through
// NativeUserJavaInterfaceStub, not fabricated values. `sendSessionReport`/
// `setEventTrackingGoogleAnalytics` are honest no-ops (log only); Stud
// has no real analytics backend to forward to.
class SessionReporterJavaInterfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/reporter/SessionReporterJavaInterface")
    static std::shared_ptr<FakeJni::JString> getAppVersion() {
        return std::make_shared<FakeJni::JString>(real_app_version());
    }
    static std::shared_ptr<FakeJni::JString> getFilesDir() {
        return std::make_shared<FakeJni::JString>(native_user_interface_files_dir());
    }
    static std::shared_ptr<FakeJni::JString> getLastLoggedInUser() {
        return std::make_shared<FakeJni::JString>(native_username());
    }
    static std::shared_ptr<FakeJni::JString> getLastLoggedInUserId() {
        return std::make_shared<FakeJni::JString>(std::to_string(native_user_id()));
    }
    static void sendSessionReport(std::shared_ptr<FakeJni::JString> str,
                                   std::shared_ptr<FakeJni::JString> /*str2*/) {
        std::printf("stud: SessionReporterJavaInterface.sendSessionReport: %s\n",
                    str ? str->asStdString().c_str() : "");
    }
    static void setEventTrackingGoogleAnalytics(std::shared_ptr<FakeJni::JString> /*str*/,
                                                 std::shared_ptr<FakeJni::JString> /*str2*/,
                                                 std::shared_ptr<FakeJni::JString> /*str3*/,
                                                 jlong /*j10*/) {
        std::printf("stud: SessionReporterJavaInterface.setEventTrackingGoogleAnalytics() (no-op)\n");
    }
};

class NativeLocaleJavaInterfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/locale/NativeLocaleJavaInterface")

    // All three used to be the literal "en_us", which is why the app came
    // up in English whatever language the desktop was running in. The
    // real implementations, read from the app's own code:
    //
    //   getLocale()       Configuration.getLocales().get(0).toString()
    //                     the system locale, "pt_BR".
    //   getRobloxLocale() the app's own locale id for it, "pt_br", from a
    //                     fixed supported set that falls back to English.
    //   getGameLocale()   a stored per-experience (UGC) locale, falling
    //                     back to getRobloxLocale() when there is none.
    //
    // Stud runs no DEX, so nothing ever stores a UGC locale and the real
    // fallback is the honest answer for the third.
    static std::shared_ptr<FakeJni::JString> getLocale() {
        return std::make_shared<FakeJni::JString>(
            stud::android_glue::system_locale().java_tag.c_str());
    }
    static std::shared_ptr<FakeJni::JString> getRobloxLocale() {
        return std::make_shared<FakeJni::JString>(
            stud::android_glue::system_locale().roblox.c_str());
    }
    static std::shared_ptr<FakeJni::JString> getGameLocale() {
        return std::make_shared<FakeJni::JString>(
            stud::android_glue::system_locale().roblox.c_str());
    }
};

// com.roblox.engine.jni.NativeGLInterface, the V2 app-bridge API's own
// declaring class (Java_com_roblox_engine_jni_NativeGLInterface_*, see
// engine_v2_bridge.cpp). Distinct from NativeGLJavaInterface above (a V1
// class). Confirmed-live crash this fixes: unlike most static
// native methods (which never touch their own jclass argument),
// nativeAppBridgeV2StartAppWithParams's compiled implementation
// dereferences its jclass parameter within its first few instructions
// (a real, reproducible null-pointer SIGSEGV under a debugger when nullptr was
// passed for it, matching this project's own established pattern for
// this bug class; see patch_libjnivm.cmake's GetFieldID null-class
// guard for the previously-fixed sibling case). Real JNI always supplies
// a genuine, non-null jclass for a static native method; registering
// this stub and passing its real jclass (env.FindClass()) instead of
// nullptr matches that.
class NativeGLInterfaceStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/engine/jni/NativeGLInterface")
};

// org.webrtc.voiceengine.BuildInfo; real, confirmed missing-class
// FindClass failure inside Roblox's bundled WebRTC voice-chat code
// (its own fatal-error formatter names it directly: "Fatal error in
// .../webrtc-new/modules/utility/source/helpers_android.cc, line 75 ...
// Check failed: c \n org/webrtc/voiceengine/BuildInfo"). No known
// fields/methods yet, grown against real evidence if a future crash
// shows one is actually called.
class WebRtcBuildInfoStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/webrtc/voiceengine/BuildInfo")
};

// org.webrtc.voiceengine.WebRtcAudioManager, same WebRTC helpers_android.cc
// FindClass sequence as WebRtcBuildInfoStub above, next class in line.
class WebRtcAudioManagerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/webrtc/voiceengine/WebRtcAudioManager")
};

// org.webrtc.voiceengine.WebRtcAudioRecord / WebRtcAudioTrack, same
// FindClass sequence as the two stubs above; real, standard upstream
// WebRTC helpers_android.cc looks up exactly these four classes
// (BuildInfo, WebRtcAudioManager, WebRtcAudioRecord, WebRtcAudioTrack)
// in this file's own JNI init path, confirmed one at a time this
// session via the real crash message naming each class in turn, adding
// the last two together since the pattern is now a known, established
// sequence rather than a guess.
class WebRtcAudioRecordStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/webrtc/voiceengine/WebRtcAudioRecord")
};

class WebRtcAudioTrackStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("org/webrtc/voiceengine/WebRtcAudioTrack")
};

// android.view.MotionEvent / android.view.KeyEvent; real, confirmed
// FindClass targets: Roblox's own input-handling init code looks these
// up and calls GetMethodID(getDeviceId, "()I") /
// GetMethodID(getHistoricalAxisValue, "(III)F") on the result
// unconditionally, without a null check, confirmed via jnivm's own
// FindClass tracing (JNIVM_ENABLE_TRACE=ON), which showed the exact
// class names right before the crash (jnivm's default "class is null"
// log doesn't include the class name itself, only the method/signature,
// so this needed the extra trace pass to pin down). No real touch/mouse/
// key event data flows through these yet (Stud has no input-forwarding
// path built). This looks like Roblox caching jmethodIDs up front for
// later use, not processing a live event, so real getter BEHAVIOR isn't
// needed yet, only that these two specific methods RESOLVE (an empty
// class, unlike the WebRTC stubs above, is not enough here, jnivm's
// GetMethodID needs an actual matching method declared, or it returns
// null too and the same crash recurs one level later). getDeviceId()
// returns 0 (real Android's own convention for "no specific input
// device," matching e.g. KeyCharacterMap.VIRTUAL_KEYBOARD's use of 0 as
// a similar sentinel); getHistoricalAxisValue() returns 0.0f (no
// historical samples buffered). Grow further only against real evidence
// (another crash), matching this file's own established discipline.
// MotionEventStub / KeyEventStub used to live here as near-empty
// placeholders (getDeviceId + getHistoricalAxisValue only). They now have
// real, fully-populated implementations in android_framework_stubs.h,
// AGDK's own GameActivity hands these objects to onTouchEventNative/
// onKeyDownNative and the engine reads a dozen more methods off each.

// com.roblox.client.flags.NativeFlagsInitResult; real class, real
// field/method layout confirmed against the app's own code (), not
// guessed. `Java_..._nativeInitializeNativeFlags` constructs one of
// these via `new NativeFlagsInitResult(int)` then calls
// `addBoolean(String,boolean,boolean)` on it repeatedly before handing
// it back to native code as the native method's own return value,
// live-confirmed real, previously-unregistered "class is null" failure
// (both the constructor and addBoolean) in this exact real call chain,
// checked at what might be needed. Real semantics for the `found`
// parameter (per the real method body): when false, real code
// resolves the value from a live flag-provider lookup instead of
// trusting the passed-in `value`. Stud has no such provider, so the
// honest fallback is `false` (same as the real code's own
// "not found, not NativeBoolean" default-false branch), not fabricating
// a lookup result.
class NativeFlagsInitResultStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/client/flags/NativeFlagsInitResult")

    NativeFlagsInitResultStub() = default;
    explicit NativeFlagsInitResultStub(FakeJni::JInt native_flag_provider_id)
        : native_flag_provider_id_(native_flag_provider_id) {}

    void addBoolean(std::shared_ptr<FakeJni::JString> key, FakeJni::JBoolean value,
                     FakeJni::JBoolean found) {
        if (!key) return;
        if (!found) value = false;
        boolean_cached_map_[static_cast<std::string>(*key)] = static_cast<bool>(value);
    }

    FakeJni::JInt getNativeFlagProviderId() { return native_flag_provider_id_; }

private:
    FakeJni::JInt native_flag_provider_id_ = 0;
    std::unordered_map<std::string, bool> boolean_cached_map_;
};

// com.roblox.universalapp.cookie.CookieProtocol$OnSetCookieHandlerImpl.
// Real, confirmed against the app's own code class (). Real mechanism: a
// real instance native method, `JNICookieProtocol.updateOnSetCookieHandler
// (OnSetCookieHandler)` (confirmed exported against the library's exported symbols:
// `Java_com_roblox_universalapp_cookie_JNICookieProtocol_
// updateOnSetCookieHandler`), registers a real Java object native code
// calls back into (`onSetCookie(String[] cookies, String url)`) whenever
// the engine's own HTTP layer sees a real Set-Cookie header it wants a
// real WebView's cookie jar to also know about, same real "native
// exports a registration entry point, real DEX would normally call it
// once, Stud calls it itself instead" shape as the Djinni
// setPlatformImpl protocols (protocol_platform_stubs.h) and
// `NativeGLJavaInterface.setAppBridgeNotificationListener`. Stud has no
// real WebView to actually sync cookies into yet, honest no-op body,
// just logs, matching this file's own "satisfy the real call shape now,
// grow the real behavior against real evidence later" convention.
class CookieOnSetHandlerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/cookie/CookieProtocol$OnSetCookieHandlerImpl")
    void onSetCookie(std::shared_ptr<FakeJni::JArray<FakeJni::JString>> cookies,
                      std::shared_ptr<FakeJni::JString> url) {
        std::size_t count = cookies ? static_cast<std::size_t>(cookies->getSize()) : 0;
        std::string url_str = url ? static_cast<std::string>(*url) : "";
        // A newly issued session cookie appears here and nowhere else,
        // so this is where a fresh login gets persisted. Nothing is kept
        // for the web view: that reads the engine's own jar live when a
        // panel opens, so switching accounts is not defeated by a stale
        // copy taken at login. Never logged by value; see
        // webview_cookies.h.
        std::vector<std::string> headers;
        headers.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            auto entry = (*cookies)[static_cast<FakeJni::JInt>(i)];
            if (entry) headers.push_back(static_cast<std::string>(*entry));
        }
        note_engine_cookies(url_str, headers);
        std::printf("stud: CookieProtocol.onSetCookie: %zu cookie(s) for url=%s (names: %s)\n",
                    count, url_str.c_str(), cookie_names(headers).c_str());
    }
};

// com.roblox.universalapp.cookie.JNICookieProtocol, the real class
// `updateOnSetCookieHandler` is an instance method on (real, the app's own code-
// confirmed singleton accessed via `JNICookieProtocol`). Any real,
// non-null jobject works as `thiz` here, the real native
// implementation stores the passed handler, doesn't dereference `thiz`
// itself (same "identity doesn't matter, only non-null-ness does"
// pattern as run_asset_manager_setup_bridge's own dummy AssetManager
// object).
class JniCookieProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/cookie/JNICookieProtocol")
};

// com.roblox.universalapp.cookie.CookieProtocol, a DIFFERENT class from
// JNICookieProtocol above, and the one the engine actually calls back into.
// Live-caught as `GetMethodID MISS class=com/roblox/universalapp/cookie/
// CookieProtocol static method=setCookie sig=(Ljava/lang/String;Ljava/lang/
// String;)V` in an ordinary run: the engine hands the platform a cookie to
// persist, and every one of those calls was being dropped.
//
// The value is a real credential, so it is never logged, name and byte count
// only, exactly as the existing cookie read-back diagnostic does.
class CookieProtocolStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/universalapp/cookie/CookieProtocol")
    static void setCookie(std::shared_ptr<FakeJni::JString> name,
                          std::shared_ptr<FakeJni::JString> value);
};

struct CookieProtocolBootstrapResult {
    bool called = false;
    bool trapped_abort = false;
};

// Calls the real `updateOnSetCookieHandler` directly, the way real DEX
// code (CookieProtocol's own constructor) would normally do once at
// real startup. Symbol missing from this specific libroblox.so build
// degrades gracefully (called stays false), matching every other
// bridge in this directory.
CookieProtocolBootstrapResult run_cookie_protocol_bootstrap(FakeJni::Jvm& jvm,
                                                              const stud::linker::LoadedLibrary& lib);

// Registers every stub class above onto `jvm`, call once, before
// load_library()'s after_constructors hook runs (GameActivity_register()
// needs these classes to already exist when it FindClass's them during
// DT_INIT_ARRAY execution).
void register_game_activity_stubs(FakeJni::Jvm& jvm);

}  // namespace stud::jni_bridge
