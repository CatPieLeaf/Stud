#include "stud/app_java_classes.h"
#include "stud/system_theme_bridge.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include <functional>

#include "stud/android_framework.h"
#include "stud/trap_recovery.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <algorithm>
#include <deque>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <sys/statvfs.h>

namespace stud::jni_bridge {

namespace {
std::string& app_version_storage() {
    static std::string version;
    return version;
}
}  // namespace

const std::string& real_app_version() { return app_version_storage(); }

bool text_input_trace_enabled() {
    static const bool enabled = std::getenv("STUD_INPUT_TRACE") != nullptr;
    return enabled;
}

void set_real_app_version(const std::string& version) { app_version_storage() = version; }


BEGIN_NATIVE_DESCRIPTOR(InsetsJava)
{ FakeJni::Field<&InsetsJava::left>{}, "left" },
{ FakeJni::Field<&InsetsJava::top>{}, "top" },
{ FakeJni::Field<&InsetsJava::right>{}, "right" },
{ FakeJni::Field<&InsetsJava::bottom>{}, "bottom" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocaleJava)
{ FakeJni::Function<&LocaleJava::getLanguage>{}, "getLanguage" },
{ FakeJni::Function<&LocaleJava::getScript>{}, "getScript" },
{ FakeJni::Function<&LocaleJava::getCountry>{}, "getCountry" },
{ FakeJni::Function<&LocaleJava::getVariant>{}, "getVariant" },
// Locale.toString() is what the real getLocale() calls; without it that
// lookup misses and the engine gets no system locale at all.
{ FakeJni::Function<&LocaleJava::toString>{}, "toString" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocaleListJava)
{ FakeJni::Function<&LocaleListJava::size>{}, "size" },
{ FakeJni::Function<&LocaleListJava::get>{}, "get" },
END_NATIVE_DESCRIPTOR

// A plain (non-member) function pointer registers as an INSTANCE method by
// default in jnivm/fake-jni (its Function<> trait marks free-function
// pointers FunctionType::None, and the default Descriptor flags for that
// case are just PUBLIC, the STATIC bit has to be requested explicitly,
// confirmed empirically: registering these without it produced "Unable to
// find STATIC method captionBar").
namespace {
constexpr int kStaticPublic = FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC;
}

BEGIN_NATIVE_DESCRIPTOR(WindowInsetsCompatTypeJava)
{ FakeJni::Function<&WindowInsetsCompatTypeJava::captionBar>{}, "captionBar", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::displayCutout>{}, "displayCutout", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::ime>{}, "ime", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::mandatorySystemGestures>{}, "mandatorySystemGestures", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::navigationBars>{}, "navigationBars", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::statusBars>{}, "statusBars", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::systemBars>{}, "systemBars", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::systemGestures>{}, "systemGestures", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeJava::tappableElement>{}, "tappableElement", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ConfigurationJava)
{ FakeJni::Function<&ConfigurationJava::getLocales>{}, "getLocales" },
{ FakeJni::Field<&ConfigurationJava::fontScale>{}, "fontScale" },
{ FakeJni::Field<&ConfigurationJava::mcc>{}, "mcc" },
{ FakeJni::Field<&ConfigurationJava::mnc>{}, "mnc" },
{ FakeJni::Field<&ConfigurationJava::userSetLocale>{}, "userSetLocale" },
{ FakeJni::Field<&ConfigurationJava::colorMode>{}, "colorMode" },
{ FakeJni::Field<&ConfigurationJava::screenLayout>{}, "screenLayout" },
{ FakeJni::Field<&ConfigurationJava::fontWeightAdjustment>{}, "fontWeightAdjustment" },
{ FakeJni::Field<&ConfigurationJava::touchscreen>{}, "touchscreen" },
{ FakeJni::Field<&ConfigurationJava::keyboard>{}, "keyboard" },
{ FakeJni::Field<&ConfigurationJava::keyboardHidden>{}, "keyboardHidden" },
{ FakeJni::Field<&ConfigurationJava::hardKeyboardHidden>{}, "hardKeyboardHidden" },
{ FakeJni::Field<&ConfigurationJava::navigation>{}, "navigation" },
{ FakeJni::Field<&ConfigurationJava::navigationHidden>{}, "navigationHidden" },
{ FakeJni::Field<&ConfigurationJava::orientation>{}, "orientation" },
{ FakeJni::Field<&ConfigurationJava::uiMode>{}, "uiMode" },
{ FakeJni::Field<&ConfigurationJava::screenWidthDp>{}, "screenWidthDp" },
{ FakeJni::Field<&ConfigurationJava::screenHeightDp>{}, "screenHeightDp" },
{ FakeJni::Field<&ConfigurationJava::smallestScreenWidthDp>{}, "smallestScreenWidthDp" },
{ FakeJni::Field<&ConfigurationJava::densityDpi>{}, "densityDpi" },
{ FakeJni::Field<&ConfigurationJava::compatScreenWidthDp>{}, "compatScreenWidthDp" },
{ FakeJni::Field<&ConfigurationJava::compatScreenHeightDp>{}, "compatScreenHeightDp" },
{ FakeJni::Field<&ConfigurationJava::compatSmallestScreenWidthDp>{}, "compatSmallestScreenWidthDp" },
{ FakeJni::Field<&ConfigurationJava::assetsSeq>{}, "assetsSeq" },
{ FakeJni::Field<&ConfigurationJava::seq>{}, "seq" },
{ FakeJni::Field<&ConfigurationJava::locale>{}, "locale" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AssetManagerJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(GameActivityJava)
{ FakeJni::Function<&GameActivityJava::finish>{}, "finish" },
{ FakeJni::Function<&GameActivityJava::setWindowFlags>{}, "setWindowFlags" },
{ FakeJni::Function<&GameActivityJava::setWindowFormat>{}, "setWindowFormat" },
{ FakeJni::Function<&GameActivityJava::getWindowInsets>{}, "getWindowInsets" },
{ FakeJni::Function<&GameActivityJava::getWaterfallInsets>{}, "getWaterfallInsets" },
{ FakeJni::Function<&GameActivityJava::setImeEditorInfo>{}, "setImeEditorInfo" },
{ FakeJni::Function<&GameActivityJava::setImeEditorInfoFields>{}, "setImeEditorInfoFields" },
{ FakeJni::Function<&GameActivityJava::getAssets>{}, "getAssets" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MainGameActivityJava)
{ FakeJni::Function<&MainGameActivityJava::bootstrapTheApp>{}, "bootstrapTheApp" },
{ FakeJni::Field<&MainGameActivityJava::nativeHelper>{}, "nativeHelper" },
{ FakeJni::Function<&MainGameActivityJava::getNativeHelper>{}, "getNativeHelper" },
{ FakeJni::Function<&MainGameActivityJava::getAppUpgradeKey>{}, "getAppUpgradeKey", kStaticPublic },
{ FakeJni::Function<&MainGameActivityJava::syncCookiesFromEngine>{}, "syncCookiesFromEngine" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<FakeJni::JString> MainGameActivityJava::getAppUpgradeKey() {
    return std::make_shared<FakeJni::JString>("AppAndroidV");
}

void MainGameActivityJava::syncCookiesFromEngine() {
    std::printf("stud: MainGameActivity.syncCookiesFromEngine() (Stud seeds the engine's cookie "
                "jar at bring-up and has no separate store to sync back to)\n");
    std::fflush(stdout);
}

MainGameActivityJava::MainGameActivityJava() : nativeHelper(std::make_shared<NativeHelperJava>()) {}

BEGIN_NATIVE_DESCRIPTOR(JavaUtilListJava)
{ FakeJni::Function<&JavaUtilListJava::size>{}, "size" },
{ FakeJni::Function<&JavaUtilListJava::isEmpty>{}, "isEmpty" },
{ FakeJni::Function<&JavaUtilListJava::get>{}, "get" },
END_NATIVE_DESCRIPTOR



namespace {
constexpr int kStaticPublicField = FakeJni::JFieldID::PUBLIC | FakeJni::JFieldID::STATIC;
}  // namespace

std::shared_ptr<PlatformSystemDialogHandlerJava> PlatformSystemDialogHandlerJava::instance() {
    if (!INSTANCE) INSTANCE = std::make_shared<PlatformSystemDialogHandlerJava>();
    return INSTANCE;
}

FakeJni::JLong IPlatformSystemDialogHandlerJava::open(
    std::shared_ptr<SystemDialogRequestJava> /*request*/,
    std::shared_ptr<ISystemDialogCallbackJava> /*callback*/) {
    std::printf("stud: SystemDialogHandler.open(). No Android dialog UI, nothing shown\n");
    std::fflush(stdout);
    return 0;
}

void IPlatformSystemDialogHandlerJava::dismiss(FakeJni::JLong /*id*/) {}
void IPlatformSystemDialogHandlerJava::dismissAll() {}

BEGIN_NATIVE_DESCRIPTOR(SystemDialogRequestJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ISystemDialogCallbackJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(IPlatformSystemDialogHandlerJava)
{ FakeJni::Function<&IPlatformSystemDialogHandlerJava::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&IPlatformSystemDialogHandlerJava::open>{}, "open" },
{ FakeJni::Function<&IPlatformSystemDialogHandlerJava::dismiss>{}, "dismiss" },
{ FakeJni::Function<&IPlatformSystemDialogHandlerJava::dismissAll>{}, "dismissAll" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PlatformSystemDialogHandlerJava)
{ FakeJni::Field<&PlatformSystemDialogHandlerJava::INSTANCE>{}, "INSTANCE", kStaticPublicField },
{ FakeJni::Function<&PlatformSystemDialogHandlerJava::isAvailable>{}, "isAvailable" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<FacialAgeEstimationProtocolJava> FacialAgeEstimationProtocolJava::instance() {
    if (!INSTANCE) INSTANCE = std::make_shared<FacialAgeEstimationProtocolJava>();
    return INSTANCE;
}

void FacialAgeEstimationProtocolJava::setListener(FakeJni::JLong native_listener_ptr) {
    std::printf("stud: FacialAgeEstimationProtocol.setListener(0x%llx)\n",
                static_cast<unsigned long long>(native_listener_ptr));
    std::fflush(stdout);
}

void FacialAgeEstimationProtocolJava::startInquiry(std::shared_ptr<FakeJni::JString> inquiry_id,
                                                    std::shared_ptr<FakeJni::JString> /*token*/) {
    // The session token is deliberately not logged; it is a real
    // credential, same rule as the .ROBLOSECURITY cookie.
    std::printf("stud: FacialAgeEstimationProtocol.startInquiry(id=%s); no face-scan SDK, "
                "nothing started\n",
                inquiry_id ? inquiry_id->asStdString().c_str() : "(null)");
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(FacialAgeEstimationProtocolJava)
{ FakeJni::Field<&FacialAgeEstimationProtocolJava::INSTANCE>{}, "INSTANCE", kStaticPublicField },
{ FakeJni::Function<&FacialAgeEstimationProtocolJava::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&FacialAgeEstimationProtocolJava::setListener>{}, "setListener" },
{ FakeJni::Function<&FacialAgeEstimationProtocolJava::startInquiry>{}, "startInquiry" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(CookieProtocolJava)
{ FakeJni::Function<&CookieProtocolJava::setCookie>{}, "setCookie", kStaticPublic },
END_NATIVE_DESCRIPTOR

void CookieProtocolJava::setCookie(std::shared_ptr<FakeJni::JString> name,
                                    std::shared_ptr<FakeJni::JString> value) {
    // Name and size only, never the value.
    std::printf("stud: CookieProtocol.setCookie: %s (%zu bytes)\n",
                name ? name->asStdString().c_str() : "", value ? value->asStdString().size() : 0u);
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(JNIBaseUrlSetterJava)
{ FakeJni::Function<&JNIBaseUrlSetterJava::setBaseUrl>{}, "setBaseUrl", kStaticPublic },
END_NATIVE_DESCRIPTOR

void JNIBaseUrlSetterJava::setBaseUrl(std::shared_ptr<FakeJni::JString> url) {
    std::printf("stud: JNIBaseUrlSetter.setBaseUrl(\"%s\")\n",
                url ? url->asStdString().c_str() : "(null)");
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(JNIExperienceProtocolJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JNILinkingProtocolJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JNIAppRestarterJava)
{ FakeJni::Function<&JNIAppRestarterJava::restartApp>{}, "restartApp", kStaticPublic },
END_NATIVE_DESCRIPTOR

void JNIAppRestarterJava::restartApp(std::shared_ptr<ContextJava> /*context*/,
                                      std::shared_ptr<FakeJni::JString> url) {
    // Deliberately does NOT exit; see the class's own doc comment.
    //
    // The URL is real though. On a device this fires ACTION_VIEW before
    // exiting, and the engine uses it for links that belong to another
    // application. `Roblox-studio:` chief among them. Handing it to the
    // desktop is the honest half of what a device does; the exit is the
    // half Stud cannot do.
    const std::string target = url ? url->asStdString() : std::string();
    if (!target.empty() && on_open_external_url) on_open_external_url(target);
    std::printf("stud: JNIAppRestarter.restartApp, engine asked to relaunch via a URL; handed "
                "it to the desktop instead of exiting (no activity manager to relaunch us)\n");
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(SurfaceJava)
END_NATIVE_DESCRIPTOR



BEGIN_NATIVE_DESCRIPTOR(NativeHelperJava)
{ FakeJni::Function<&NativeHelperJava::gameActivity_hideKeyboard>{}, "gameActivity_hideKeyboard" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onAppReady>{}, "gameActivity_onAppReady" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onDidLogInReceived>{}, "gameActivity_onDidLogInReceived" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onDidLogOutReceived>{}, "gameActivity_onDidLogOutReceived" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onDidSignUp>{}, "gameActivity_onDidSignUp" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onDidSwitchAccountReceived>{}, "gameActivity_onDidSwitchAccountReceived" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onEngineInitialized>{}, "gameActivity_onEngineInitialized" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onExperienceStart>{}, "gameActivity_onExperienceStart" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onExperienceStop>{}, "gameActivity_onExperienceStop" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onFlagsFailed>{}, "gameActivity_onFlagsFailed" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onFlagsLoaded>{}, "gameActivity_onFlagsLoaded" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onGameLoaded>{}, "gameActivity_onGameLoaded" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onGameStreamingStatusChanged>{}, "gameActivity_onGameStreamingStatusChanged" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onLuaAppDidReturn>{}, "gameActivity_onLuaAppDidReturn" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onLuaTextBoxChanged>{}, "gameActivity_onLuaTextBoxChanged" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onLuaTextBoxPropertyChanged>{}, "gameActivity_onLuaTextBoxPropertyChanged" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onMotionEventListening>{}, "gameActivity_onMotionEventListening" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onRestartLuaApp>{}, "gameActivity_onRestartLuaApp" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onScanQrCode>{}, "gameActivity_onScanQrCode" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onScreenOrientationChanged>{}, "gameActivity_onScreenOrientationChanged" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_onScreenshotReady>{}, "gameActivity_onScreenshotReady" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_setAppUpgradeStatus>{}, "gameActivity_setAppUpgradeStatus" },
{ FakeJni::Function<&NativeHelperJava::gameActivity_showKeyboard>{}, "gameActivity_showKeyboard" },
END_NATIVE_DESCRIPTOR

FakeJni::JBoolean ActivityJava::runOnUiThread(std::shared_ptr<FakeJni::JObject> runnable) {
    if (!runnable) return false;
    LooperJava::getMainLooper()->post(std::move(runnable));
    return true;
}

namespace {
std::string g_activity_files_dir;
std::string g_activity_cache_dir;
}  // namespace

void set_activity_context_directories(std::string files_dir, std::string cache_dir) {
    g_activity_files_dir = std::move(files_dir);
    g_activity_cache_dir = std::move(cache_dir);
}

std::shared_ptr<JavaIoFileJava> ContextJava::getFilesDir() {
    return std::make_shared<JavaIoFileJava>(g_activity_files_dir);
}

std::shared_ptr<JavaIoFileJava> ContextJava::getCacheDir() {
    return std::make_shared<JavaIoFileJava>(g_activity_cache_dir);
}

BEGIN_NATIVE_DESCRIPTOR(JavaIoFileJava)
{ FakeJni::Function<&JavaIoFileJava::getAbsolutePath>{}, "getAbsolutePath" },
{ FakeJni::Function<&JavaIoFileJava::getPath>{}, "getPath" },
{ FakeJni::Function<&JavaIoFileJava::toStringJ>{}, "toString" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ContextJava)
{ FakeJni::Function<&ContextJava::getPackageName>{}, "getPackageName" },
{ FakeJni::Function<&ContextJava::getFilesDir>{}, "getFilesDir" },
{ FakeJni::Function<&ContextJava::getCacheDir>{}, "getCacheDir" },
{ FakeJni::Function<&ContextJava::getSharedPreferences>{}, "getSharedPreferences" },
{ FakeJni::Function<&ContextJava::getResources>{}, "getResources" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ActivityJava)
{ FakeJni::Function<&ActivityJava::runOnUiThread>{}, "runOnUiThread" },
{ FakeJni::Function<&ActivityJava::getApplicationContext>{}, "getApplicationContext" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<ApplicationJava> ApplicationJava::singleton() {
    static auto instance = std::make_shared<ApplicationJava>();
    return instance;
}

BEGIN_NATIVE_DESCRIPTOR(ApplicationJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ActivityThreadJava)
{ FakeJni::Function<&ActivityThreadJava::getApplication>{}, "getApplication" },
{ FakeJni::Function<&ActivityThreadJava::currentActivityThread>{}, "currentActivityThread", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(FMODJava)
{ FakeJni::Function<&FMODJava::init>{}, "init", kStaticPublic },
{ FakeJni::Function<&FMODJava::checkInit>{}, "checkInit", kStaticPublic },
{ FakeJni::Function<&FMODJava::close>{}, "close", kStaticPublic },
{ FakeJni::Function<&FMODJava::getAssetManager>{}, "getAssetManager", kStaticPublic },
{ FakeJni::Function<&FMODJava::getOutputBlockSize>{}, "getOutputBlockSize", kStaticPublic },
{ FakeJni::Function<&FMODJava::getOutputSampleRate>{}, "getOutputSampleRate", kStaticPublic },
{ FakeJni::Function<&FMODJava::isBluetoothOn>{}, "isBluetoothOn", kStaticPublic },
{ FakeJni::Function<&FMODJava::lowLatencyFlag>{}, "lowLatencyFlag", kStaticPublic },
{ FakeJni::Function<&FMODJava::proAudioFlag>{}, "proAudioFlag", kStaticPublic },
{ FakeJni::Function<&FMODJava::supportsAAudio>{}, "supportsAAudio", kStaticPublic },
{ FakeJni::Function<&FMODJava::supportsLowLatency>{}, "supportsLowLatency", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AudioDeviceJava)
{ FakeJni::Constructor<AudioDeviceJava>{} },
{ FakeJni::Function<&AudioDeviceJava::init>{}, "init" },
{ FakeJni::Function<&AudioDeviceJava::close>{}, "close" },
{ FakeJni::Function<&AudioDeviceJava::write>{}, "write" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(EmptyArrayListJava)
{ FakeJni::Function<&EmptyArrayListJava::size>{}, "size" },
{ FakeJni::Function<&EmptyArrayListJava::isEmpty>{}, "isEmpty" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(GameTextInputStateJava)
{ FakeJni::Constructor<GameTextInputStateJava>{} },
{ FakeJni::Constructor<GameTextInputStateJava, std::shared_ptr<FakeJni::JString>, FakeJni::JInt,
                        FakeJni::JInt, FakeJni::JInt, FakeJni::JInt>{} },
{ FakeJni::Field<&GameTextInputStateJava::text>{}, "text" },
{ FakeJni::Field<&GameTextInputStateJava::selectionStart>{}, "selectionStart" },
{ FakeJni::Field<&GameTextInputStateJava::selectionEnd>{}, "selectionEnd" },
{ FakeJni::Field<&GameTextInputStateJava::composingRegionStart>{}, "composingRegionStart" },
{ FakeJni::Field<&GameTextInputStateJava::composingRegionEnd>{}, "composingRegionEnd" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ChannelRecordJava)
{ FakeJni::Constructor<ChannelRecordJava>{} },
{ FakeJni::Constructor<ChannelRecordJava, std::shared_ptr<FakeJni::JString>, jlong>{} },
{ FakeJni::Field<&ChannelRecordJava::name>{}, "name" },
{ FakeJni::Field<&ChannelRecordJava::id>{}, "id" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ApplicationExitInfoCppJava)
{ FakeJni::Constructor<ApplicationExitInfoCppJava>{} },
{ FakeJni::Constructor<ApplicationExitInfoCppJava, FakeJni::JInt, jlong,
                        std::shared_ptr<FakeJni::JString>>{} },
{ FakeJni::Constructor<ApplicationExitInfoCppJava, FakeJni::JInt, FakeJni::JInt, jlong,
                        std::shared_ptr<FakeJni::JString>>{} },
{ FakeJni::Constructor<ApplicationExitInfoCppJava, FakeJni::JInt, FakeJni::JInt, jlong,
                        std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>,
                        std::shared_ptr<FakeJni::JString>, jlong, jlong, FakeJni::JInt>{} },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mPid>{}, "mPid" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mSignal>{}, "mSignal" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mTimestamp>{}, "mTimestamp" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mExitReason>{}, "mExitReason" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mExitSubreason>{}, "mExitSubreason" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mDescription>{}, "mDescription" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mPss>{}, "mPss" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mRss>{}, "mRss" },
{ FakeJni::Field<&ApplicationExitInfoCppJava::mImportance>{}, "mImportance" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JNIAchievementJava)
{ FakeJni::Function<&JNIAchievementJava::grantAchievementForNativeAsync>{}, "grantAchievementForNativeAsync", kStaticPublic },
{ FakeJni::Function<&JNIAchievementJava::hasAchievedForNativeAsync>{}, "hasAchievedForNativeAsync", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(InputConnectionJava)
{ FakeJni::Function<&InputConnectionJava::setState>{}, "setState" },
{ FakeJni::Function<&InputConnectionJava::setSoftKeyboardActive>{}, "setSoftKeyboardActive" },
{ FakeJni::Function<&InputConnectionJava::restartInput>{}, "restartInput" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeGLInterfaceJava)
END_NATIVE_DESCRIPTOR

namespace {
std::string g_native_user_interface_files_dir;
long long g_native_user_id = 0;
std::string g_native_username = "StudPlayer";
std::string g_native_display_name = "StudPlayer";
}  // namespace

void set_native_user_interface_files_dir(std::string path) {
    g_native_user_interface_files_dir = std::move(path);
}

void set_native_user_identity(long long user_id, std::string username, std::string display_name) {
    g_native_user_id = user_id;
    if (!username.empty()) g_native_username = std::move(username);
    if (!display_name.empty()) g_native_display_name = std::move(display_name);
}

long long native_user_id() { return g_native_user_id; }
std::string native_username() { return g_native_username; }
std::string native_display_name() { return g_native_display_name; }
std::string native_user_interface_files_dir() { return g_native_user_interface_files_dir; }

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceJava::getFilesDir() {
    return std::make_shared<FakeJni::JString>(g_native_user_interface_files_dir);
}

FakeJni::JLong NativeUserJavaInterfaceJava::getUserId() { return g_native_user_id; }

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceJava::getUsername() {
    return std::make_shared<FakeJni::JString>(g_native_username);
}

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceJava::getDisplayName() {
    return std::make_shared<FakeJni::JString>(g_native_display_name);
}

BEGIN_NATIVE_DESCRIPTOR(NativeUserJavaInterfaceJava)
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getUserId>{}, "getUserId", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getIsUnder13>{}, "getIsUnder13", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getUsername>{}, "getUsername", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getDisplayName>{}, "getDisplayName", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getAlternateName>{}, "getAlternateName", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getPlatformName>{}, "getPlatformName", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getMembershipType>{}, "getMembershipType", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getHasRobloxSubscription>{}, "getHasRobloxSubscription", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getTheme>{}, "getTheme", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getLastLoggedInUser>{}, "getLastLoggedInUser", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getLastLoggedInUserId>{}, "getLastLoggedInUserId", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getFilesDir>{}, "getFilesDir", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::getAppVersion>{}, "getAppVersion", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::isDebuggerConnected>{}, "isDebuggerConnected", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceJava::setEventTrackingGoogleAnalytics>{}, "setEventTrackingGoogleAnalytics", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NetworkUtilsJava)
{ FakeJni::Function<&NetworkUtilsJava::getPublicIPv4Addresseses>{}, "getPublicIPv4Addresseses", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeQuoteInterfaceJava)
{ FakeJni::Function<&NativeQuoteInterfaceJava::requestResponse>{}, "requestResponse", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ExperienceSessionJava)
{ FakeJni::Function<&ExperienceSessionJava::shouldDisableExperienceIdleTimer>{}, "shouldDisableExperienceIdleTimer", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppRatingPromptHandlerJava)
{ FakeJni::Function<&AppRatingPromptHandlerJava::isAppRatingPromptAvailable>{}, "isAppRatingPromptAvailable", kStaticPublic },
{ FakeJni::Function<&AppRatingPromptHandlerJava::showAppRatingPrompt>{}, "showAppRatingPrompt", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(IAPPurchaseManagerJava)
{ FakeJni::Function<&IAPPurchaseManagerJava::getPlatformPaymentMethod>{}, "getPlatformPaymentMethod", kStaticPublic },
{ FakeJni::Function<&IAPPurchaseManagerJava::getPlatformPaymentProviderType>{}, "getPlatformPaymentProviderType", kStaticPublic },
{ FakeJni::Function<&IAPPurchaseManagerJava::invokeStore>{}, "invokeStore", kStaticPublic },
{ FakeJni::Function<&IAPPurchaseManagerJava::invokeStoreV2>{}, "invokeStoreV2", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AssertDialogUtilJava)
{ FakeJni::Function<&AssertDialogUtilJava::showAssertionPopup>{}, "showAssertionPopup", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SystemThemeProtocolJava)
{ FakeJni::Function<&SystemThemeProtocolJava::getSystemTheme>{}, "getSystemTheme", kStaticPublic },
{ FakeJni::Function<&SystemThemeProtocolJava::isSystemThemeAvailable>{}, "isSystemThemeAvailable", kStaticPublic },
END_NATIVE_DESCRIPTOR

// Exists to be found, not to be called: every method on the real class is
// a native whose body is libroblox's own. See the header.
BEGIN_NATIVE_DESCRIPTOR(JNISystemThemeProtocolJava)
END_NATIVE_DESCRIPTOR

jlong LocalStorageManagerJava::getAllocatableBytes() {
    struct statvfs st{};
    const char* path = std::getenv("HOME");
    std::string cache_root = (path ? std::string(path) : ".") + "/.cache/stud";
    if (statvfs(cache_root.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<jlong>(st.f_bavail) * static_cast<jlong>(st.f_frsize);
}

BEGIN_NATIVE_DESCRIPTOR(LocalStorageManagerJava)
{ FakeJni::Function<&LocalStorageManagerJava::getAllocatableBytes>{}, "getAllocatableBytes" },
END_NATIVE_DESCRIPTOR

void NativeGLJavaInterfaceJava::onAppBridgeNotification(std::shared_ptr<FakeJni::JString> type,
                                                          std::shared_ptr<FakeJni::JString> data) {
    std::string type_str = type ? type->asStdString() : "";
    std::string data_str = data ? data->asStdString() : "";
    std::printf("stud: onAppBridgeNotification: type=\"%s\" data=%s\n", type_str.c_str(),
                data_str.c_str());
    // Confirmed shape for the one payload this project knows
    // about (InitHelper's own inner callback, the AppBridge-BrowserTracker flow;
    // see this class's own header doc comment). Parsed and logged, not
    // yet acted on; see that comment for why acting on it is deferred
    // until a real payload has actually been observed.
    try {
        nlohmann::json doc = nlohmann::json::parse(data_str);
        if (doc.contains("btid")) {
            std::printf("stud: onAppBridgeNotification: real browserTrackerId=%s result=%s\n",
                        doc.value("btid", nlohmann::json()).dump().c_str(),
                        doc.value("result", nlohmann::json()).dump().c_str());
        }
    } catch (const nlohmann::json::parse_error&) {
        // Not every real notification is this JSON shape, honest no-op,
        // not an error worth surfacing.
    }
}

std::shared_ptr<DeviceStaticParamsJava> NativeGLJavaInterfaceJava::s_device_static_params;

namespace {
// Focused-TextBox state, shared with the input bridge. Guarded because
// the engine sets it from its own thread and the input poll thread reads
// it from another.
std::mutex g_text_box_mutex;
long g_active_text_box = 0;
std::string g_active_text_box_text;
NativeGLJavaInterfaceJava::TextBoxStyle g_active_text_box_style;
// What Stud has sent the focused box and the engine has not yet echoed
// back, oldest first. The engine reports its TextBox's text after every
// change, Stud's own included; an echo of an earlier keystroke arriving
// after a later one must not rewind what is being typed. Anything that is
// not one of these is a change the engine made itself.
std::deque<std::string> g_text_sent_unechoed;
}  // namespace

void NativeGLJavaInterfaceJava::showKeyboard(FakeJni::JLong text_box,
                                              FakeJni::JBoolean show_native_input,
                                              std::shared_ptr<FakeJni::JByteArray> initial_text,
                                              std::shared_ptr<NativeTextBoxInfoJava> info) {
    std::string text;
    if (initial_text) {
        // The real argument is the TextBox's current contents as UTF-8
        // bytes, so typing continues from what is already there rather
        // than replacing it.
        for (FakeJni::JInt i = 0; i < initial_text->getSize(); ++i) {
            char c = static_cast<char>((*initial_text)[i]);
            if (c == '\0') break;
            text.push_back(c);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_text_box_mutex);
        g_active_text_box = static_cast<long>(text_box);
        g_active_text_box_text = text;
        g_text_sent_unechoed.clear();
        // A focus almost always arrives twice: once for the real box and
        // once for a second one with a degenerate rectangle (live: a
        // 358x36 box followed immediately by a 42x0 one). A box with no
        // height has nothing to draw in, so the last real rectangle is
        // kept rather than being overwritten by it.
        const bool drawable =
            info && static_cast<float>(info->width) > 0.0f && static_cast<float>(info->height) > 0.0f;
        if (drawable) g_active_text_box_style = {};
        if (drawable) {
            g_active_text_box_style.x = info->x;
            g_active_text_box_style.y = info->y;
            g_active_text_box_style.width = info->width;
            g_active_text_box_style.height = info->height;
            g_active_text_box_style.font_size = info->fontSize;
            g_active_text_box_style.font = static_cast<int>(info->font);
            g_active_text_box_style.color = static_cast<unsigned>(info->textColor);
            g_active_text_box_style.x_alignment = static_cast<int>(info->xAlignment);
            g_active_text_box_style.y_alignment = static_cast<int>(info->yAlignment);
            // Roblox's own TextInputType: 5 and 9 are the password kinds
            // (the real app maps exactly those onto Android's password
            // input types in RbxKeyboard.l).
            const int input_type = static_cast<int>(info->textInputType);
            g_active_text_box_style.password = input_type == 5 || input_type == 9;
        }
    }
    // Never log the contents, a real TextBox can be a password field.
    // The flags and geometry are not secret, and they are the evidence
    // for HOW the engine expects typing to be shown: `showNativeInput`
    // arrives TRUE, and the NativeTextBoxInfo carries the box's real
    // rectangle, font size and colour. That combination only makes sense
    // if a native input widget is meant to sit over the box and draw the
    // in-progress text; see the text-input entry in the engineering notes.
    std::printf("stud: showKeyboard: text box focused (%zu chars of existing text) "
                "showNativeInput=%d editable=%d multiline=%d inputType=%d returnKey=%d "
                "box=[%.0f,%.0f %.0fx%.0f] fontSize=%.1f textColor=%08x\n",
                text.size(), static_cast<int>(show_native_input),
                info ? static_cast<int>(info->editable) : -1,
                info ? static_cast<int>(info->multiline) : -1,
                info ? static_cast<int>(info->textInputType) : -1,
                info ? static_cast<int>(info->returnKeyType) : -1,
                info ? static_cast<double>(info->x) : 0.0,
                info ? static_cast<double>(info->y) : 0.0,
                info ? static_cast<double>(info->width) : 0.0,
                info ? static_cast<double>(info->height) : 0.0,
                info ? static_cast<double>(info->fontSize) : 0.0,
                info ? static_cast<unsigned>(info->textColor) : 0u);
    std::fflush(stdout);
}

namespace {
void log_engine_call(const char* what) {
    std::printf("stud: NativeGLJavaInterface.%s\n", what);
    std::fflush(stdout);
}
}  // namespace

// Real methods libroblox looks up on this class. Each was a live
// `GetMethodID MISS` before this; see the header for why that is never
// cosmetic. Honest bodies. Stud reports what it was told and does nothing
// where it genuinely has nothing to do.
void NativeGLJavaInterfaceJava::gameLoadedCallback(FakeJni::JLong placeId) {
    std::printf("stud: NativeGLJavaInterface.gameLoadedCallback: placeId=%lld\n",
                static_cast<long long>(placeId));
    std::fflush(stdout);
}
void NativeGLJavaInterfaceJava::gameDidLeave() { log_engine_call("gameDidLeave()"); }
void NativeGLJavaInterfaceJava::exitGameWithError(FakeJni::JInt error) {
    std::printf("stud: NativeGLJavaInterface.exitGameWithError: %d\n", static_cast<int>(error));
    std::fflush(stdout);
}
void NativeGLJavaInterfaceJava::onAppShellReloadNeeded() {
    log_engine_call("onAppShellReloadNeeded()");
}
void NativeGLJavaInterfaceJava::onDataModelNotificationCallback(
    std::shared_ptr<FakeJni::JString> type, std::shared_ptr<FakeJni::JString> data) {
    // Type only, the payload can carry account data.
    const std::string kind = type ? type->asStdString() : std::string();
    std::printf("stud: NativeGLJavaInterface.onDataModelNotificationCallback: type=%s\n",
                kind.c_str());
    std::fflush(stdout);
    if (kind == "NATIVE_EXIT" && on_native_exit) on_native_exit();
    // The account report the app's SessionManager keeps; see
    // record_account_info().
    if (kind == "DID_LOG_IN" && data) record_account_info(data->asStdString());
}
// The focused TextBox's text, changed by the engine: a chat command such
// as /team turns into a [Team] tag and takes itself out of the text. The
// app's own keyboard adopts it, and so does Stud's text box, which
// follows active_text_box_text(); without this it kept showing "/team"
// in front of what was typed next, and deleting that "/team" undid the
// command.
void NativeGLJavaInterfaceJava::onLuaTextBoxChangedCallback(std::shared_ptr<FakeJni::JString> value) {
    const std::string text = value ? value->asStdString() : std::string();
    bool adopted = false;
    {
        std::lock_guard<std::mutex> lock(g_text_box_mutex);
        if (g_active_text_box == 0) return;
        const auto echo =
            std::find(g_text_sent_unechoed.begin(), g_text_sent_unechoed.end(), text);
        if (echo != g_text_sent_unechoed.end()) {
            // Echoes come back in order, so everything sent before this
            // one has been echoed or never will be.
            g_text_sent_unechoed.erase(g_text_sent_unechoed.begin(), echo + 1);
        } else if (text != g_active_text_box_text) {
            g_active_text_box_text = text;
            g_text_sent_unechoed.clear();
            adopted = true;
        }
    }
    if (adopted && text_input_trace_enabled()) {
        // Length only, never the content: a real TextBox can be a password field.
        std::printf("stud: onLuaTextBoxChangedCallback: the engine changed the text (%zu chars)\n",
                    text.size());
        std::fflush(stdout);
    }
}
void NativeHelperJava::gameActivity_onLuaTextBoxChanged(std::shared_ptr<FakeJni::JString> value) {
    NativeGLJavaInterfaceJava::onLuaTextBoxChangedCallback(std::move(value));
}
void NativeGLJavaInterfaceJava::onLuaTextBoxPropertyChangedCallback() {
    if (text_input_trace_enabled()) log_engine_call("onLuaTextBoxPropertyChangedCallback()");
}
void NativeGLJavaInterfaceJava::onVrSessionStateUpdate(FakeJni::JInt) {
    // Stud has no VR session.
}
void NativeGLJavaInterfaceJava::onExtendedAnalyticsRecvCallback(
    std::shared_ptr<FakeJni::JByteArray>, FakeJni::JInt) {
    // Analytics payload Stud has nowhere to forward to.
}
void NativeGLJavaInterfaceJava::screenOrientationChanged(FakeJni::JInt orientation) {
    std::printf("stud: NativeGLJavaInterface.screenOrientationChanged: %d\n",
                static_cast<int>(orientation));
    std::fflush(stdout);
}
void NativeGLJavaInterfaceJava::listenToMotionEvents(std::shared_ptr<FakeJni::JString> motionType) {
    // Stud has no accelerometer/gyroscope to listen to.
    std::printf("stud: NativeGLJavaInterface.listenToMotionEvents: %s (no motion sensors)\n",
                motionType ? motionType->asStdString().c_str() : "");
    std::fflush(stdout);
}
void NativeGLJavaInterfaceJava::openNativeOverlay(std::shared_ptr<FakeJni::JString> a,
                                                   std::shared_ptr<FakeJni::JString>) {
    std::printf("stud: NativeGLJavaInterface.openNativeOverlay: %s (no native overlay)\n",
                a ? a->asStdString().c_str() : "");
    std::fflush(stdout);
}
// The app's own implementation is the same routine gameActivity_onScreenshotReady
// runs: copy the file into the gallery, then report through
// nativeImageSavedToAlbumFinished. See album_bridge.h.
void NativeGLJavaInterfaceJava::saveImageToAlbum(std::shared_ptr<FakeJni::JString> path) {
    const std::string file = path ? path->asStdString() : std::string();
    std::printf("stud: NativeGLJavaInterface.saveImageToAlbum: %s\n", file.c_str());
    std::fflush(stdout);
    if (!file.empty() && NativeHelperJava::on_capture_ready) {
        NativeHelperJava::on_capture_ready(file);
    }
}
namespace {
// Set once at bring-up, by the process that can reach the engine's own
// setter. Kept as a callback because this class is a plain static with no
// library handle of its own.
std::function<void()>& webview_user_agent_reporter() {
    static std::function<void()> reporter;
    return reporter;
}
}  // namespace

void set_webview_user_agent_reporter(std::function<void()> reporter) {
    webview_user_agent_reporter() = std::move(reporter);
}

void NativeGLJavaInterfaceJava::getWebViewUserAgent() {
    // Returns void: the real implementation asks its WebView
    // asynchronously and hands the answer back through the engine's own
    // setWebviewUserAgent. Stud does the same, with the string its viewer
    // really sends (stud/webview_user_agent.h).
    //
    // Answering matters: the engine reports this to Roblox when it
    // creates a login challenge, and the page that answers the challenge
    // runs in the viewer. Saying nothing meant the challenge was created
    // against an empty client and the page failed with "something went
    // wrong", live-reported, with the viewer left open because the
    // challenge never completed.
    log_engine_call("getWebViewUserAgent()");
    if (auto& reporter = webview_user_agent_reporter(); reporter) reporter();
}
void NativeGLJavaInterfaceJava::getMobileAdvertisingId() {
    // Same shape. Stud has no advertising id, and inventing one would be a
    // fabricated identifier, not an honest placeholder.
    log_engine_call("getMobileAdvertisingId() (none on this platform)");
}
namespace {
std::function<void(long long, const std::string&)>& purchase_unavailable_reporter() {
    static std::function<void(long long, const std::string&)> reporter;
    return reporter;
}
// Every native purchase entry point ends here. The app, with no billing
// (no Play Store, which a desktop never has), reports the purchase as not
// completed; so does this.
void purchase_unavailable(const char* which, FakeJni::JLong user_id,
                          const std::shared_ptr<FakeJni::JString>& product_id) {
    log_engine_call(which);
    if (auto& reporter = purchase_unavailable_reporter(); reporter) {
        reporter(user_id, product_id ? product_id->asStdString() : std::string());
    }
}
}  // namespace

void set_purchase_unavailable_reporter(
    std::function<void(long long, const std::string&)> reporter) {
    purchase_unavailable_reporter() = std::move(reporter);
}

void NativeGLJavaInterfaceJava::promptNativePurchase(FakeJni::JLong user_id,
                                                      std::shared_ptr<FakeJni::JString> product) {
    purchase_unavailable("promptNativePurchase() (no billing)", user_id, product);
}
void NativeGLJavaInterfaceJava::promptNativePurchase2(FakeJni::JLong user_id,
                                                       std::shared_ptr<FakeJni::JString> product,
                                                       std::shared_ptr<FakeJni::JString>) {
    purchase_unavailable("promptNativePurchase() (no billing)", user_id, product);
}
void NativeGLJavaInterfaceJava::promptNativePurchaseWithPayload(
    FakeJni::JLong user_id, std::shared_ptr<FakeJni::JString> product,
    std::shared_ptr<FakeJni::JString>) {
    purchase_unavailable("promptNativePurchaseWithPayload() (no billing)", user_id, product);
}
void NativeGLJavaInterfaceJava::promptNativePurchaseWithPaymentSessionId(
    FakeJni::JLong user_id, std::shared_ptr<FakeJni::JString> product,
    std::shared_ptr<FakeJni::JString>) {
    purchase_unavailable("promptNativePurchaseWithPaymentSessionId() (no billing)", user_id,
                         product);
}
void NativeGLJavaInterfaceJava::promptNativePurchaseWithPaymentSessionId3(
    FakeJni::JLong user_id, std::shared_ptr<FakeJni::JString> product,
    std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>) {
    purchase_unavailable("promptNativePurchaseWithPaymentSessionId() (no billing)", user_id,
                         product);
}

void NativeGLJavaInterfaceJava::hideKeyboard() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    g_active_text_box = 0;
    g_active_text_box_text.clear();
    g_active_text_box_style = {};
    g_text_sent_unechoed.clear();
    std::printf("stud: hideKeyboard: text box focus released\n");
}

long NativeGLJavaInterfaceJava::active_text_box() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    return g_active_text_box;
}

void NativeGLJavaInterfaceJava::set_active_text_box_style(const TextBoxStyle& style) {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    g_active_text_box_style = style;
}

NativeGLJavaInterfaceJava::TextBoxStyle NativeGLJavaInterfaceJava::active_text_box_style() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    return g_active_text_box_style;
}

std::string NativeGLJavaInterfaceJava::active_text_box_text() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    return g_active_text_box_text;
}

void NativeGLJavaInterfaceJava::set_active_text_box_text(std::string text) {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    g_text_sent_unechoed.push_back(text);
    g_active_text_box_text = std::move(text);
}

BEGIN_NATIVE_DESCRIPTOR(NativeTextBoxInfoJava)
{ FakeJni::Constructor<NativeTextBoxInfoJava>{} },
// The real (FFFFFZIIIIIIZZZ) constructor libroblox actually calls.
{ FakeJni::Constructor<NativeTextBoxInfoJava, FakeJni::JFloat, FakeJni::JFloat, FakeJni::JFloat,
                       FakeJni::JFloat, FakeJni::JFloat, FakeJni::JBoolean, FakeJni::JInt,
                       FakeJni::JInt, FakeJni::JInt, FakeJni::JInt, FakeJni::JInt, FakeJni::JInt,
                       FakeJni::JBoolean, FakeJni::JBoolean, FakeJni::JBoolean>{} },
{ FakeJni::Field<&NativeTextBoxInfoJava::font>{}, "font" },
{ FakeJni::Field<&NativeTextBoxInfoJava::fontSize>{}, "fontSize" },
{ FakeJni::Field<&NativeTextBoxInfoJava::height>{}, "height" },
{ FakeJni::Field<&NativeTextBoxInfoJava::manualFocusRelease>{}, "manualFocusRelease" },
{ FakeJni::Field<&NativeTextBoxInfoJava::multiline>{}, "multiline" },
{ FakeJni::Field<&NativeTextBoxInfoJava::returnKeyType>{}, "returnKeyType" },
{ FakeJni::Field<&NativeTextBoxInfoJava::textColor>{}, "textColor" },
{ FakeJni::Field<&NativeTextBoxInfoJava::textInputType>{}, "textInputType" },
{ FakeJni::Field<&NativeTextBoxInfoJava::textWrapped>{}, "textWrapped" },
{ FakeJni::Field<&NativeTextBoxInfoJava::width>{}, "width" },
{ FakeJni::Field<&NativeTextBoxInfoJava::x>{}, "x" },
{ FakeJni::Field<&NativeTextBoxInfoJava::xAlignment>{}, "xAlignment" },
{ FakeJni::Field<&NativeTextBoxInfoJava::y>{}, "y" },
{ FakeJni::Field<&NativeTextBoxInfoJava::yAlignment>{}, "yAlignment" },
{ FakeJni::Field<&NativeTextBoxInfoJava::editable>{}, "editable" },
END_NATIVE_DESCRIPTOR

namespace {
// Real process start, captured the first time anything asks. Stud's own
// process start is the honest answer to what real Android records via
// Process.getStartElapsedRealtime().
std::chrono::steady_clock::time_point process_start_time() {
    static const auto t0 = std::chrono::steady_clock::now();
    return t0;
}
const bool g_process_start_primed = (process_start_time(), true);
}  // namespace

FakeJni::JLong LoggingProtocolJava::getProcessTimestamp() {
    (void)g_process_start_primed;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - process_start_time())
                  .count();
    return static_cast<FakeJni::JLong>(ms);
}

BEGIN_NATIVE_DESCRIPTOR(VideoCodecCapabilityJava)
{ FakeJni::Constructor<VideoCodecCapabilityJava>{} },
{ FakeJni::Field<&VideoCodecCapabilityJava::codec>{}, "codec" },
{ FakeJni::Field<&VideoCodecCapabilityJava::name>{}, "name" },
{ FakeJni::Field<&VideoCodecCapabilityJava::isEncoder>{}, "isEncoder" },
{ FakeJni::Field<&VideoCodecCapabilityJava::isHardware>{}, "isHardware" },
{ FakeJni::Field<&VideoCodecCapabilityJava::maxBitrate>{}, "maxBitrate" },
{ FakeJni::Field<&VideoCodecCapabilityJava::minBitrate>{}, "minBitrate" },
{ FakeJni::Field<&VideoCodecCapabilityJava::maxFps>{}, "maxFps" },
{ FakeJni::Field<&VideoCodecCapabilityJava::minFps>{}, "minFps" },
{ FakeJni::Field<&VideoCodecCapabilityJava::maxWidth>{}, "maxWidth" },
{ FakeJni::Field<&VideoCodecCapabilityJava::minWidth>{}, "minWidth" },
{ FakeJni::Field<&VideoCodecCapabilityJava::maxHeight>{}, "maxHeight" },
{ FakeJni::Field<&VideoCodecCapabilityJava::minHeight>{}, "minHeight" },
{ FakeJni::Field<&VideoCodecCapabilityJava::maxInstances>{}, "maxInstances" },
{ FakeJni::Field<&VideoCodecCapabilityJava::profiles>{}, "profiles" },
{ FakeJni::Field<&VideoCodecCapabilityJava::levels>{}, "levels" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MediaCodecInfoUtilsJava)
{ FakeJni::Function<&MediaCodecInfoUtilsJava::getVideoCodecs>{}, "getVideoCodecs", kStaticPublic },
{ FakeJni::Function<&MediaCodecInfoUtilsJava::hevcHardwareEncodingSupported>{}, "hevcHardwareEncodingSupported", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LoggingProtocolJava)
{ FakeJni::Function<&LoggingProtocolJava::getProcessTimestamp>{}, "getProcessTimestamp", kStaticPublic },
END_NATIVE_DESCRIPTOR

FakeJni::JInt AppRtcDeviceWrapperJava::getSelectedAudioDeviceAsInt() {
    return 1;  // WIRED_HEADSET, an ordinary desktop output
}
std::shared_ptr<FakeJni::JString> AppRtcDeviceWrapperJava::getSelectedAudioDeviceName() {
    return std::make_shared<FakeJni::JString>("Desktop Audio");
}
FakeJni::JBoolean AppRtcDeviceWrapperJava::isValid() { return true; }
void AppRtcDeviceWrapperJava::wrapSetCommunicationMute(FakeJni::JBoolean muted) {
    // Reported, not acted on: Stud has no system-level microphone mute,
    // and the engine stops reading the stream when it mutes anyway.
    std::printf("stud: audio: engine set communication mute = %d\n", muted ? 1 : 0);
    std::fflush(stdout);
}
void AppRtcDeviceWrapperJava::wrapStartCommunication() {
    std::printf("stud: audio: engine started voice communication\n");
    std::fflush(stdout);
}
void AppRtcDeviceWrapperJava::wrapStopCommunication() {
    std::printf("stud: audio: engine stopped voice communication\n");
    std::fflush(stdout);
}

// Every answer is the refusal: no hardware codec, so nothing to describe
// and nothing to read. FMOD checks init() first and takes its software
// path when it fails.
FakeJni::JBoolean FmodMediaCodecJava::init(FakeJni::JLong) { return false; }
FakeJni::JInt FmodMediaCodecJava::getChannelCount() { return 0; }
FakeJni::JInt FmodMediaCodecJava::getSampleRate() { return 0; }
FakeJni::JLong FmodMediaCodecJava::getLength() { return 0; }
FakeJni::JInt FmodMediaCodecJava::read(std::shared_ptr<FakeJni::JByteArray>, FakeJni::JInt) {
    return 0;
}
void FmodMediaCodecJava::release() {}

BEGIN_NATIVE_DESCRIPTOR(FmodMediaCodecJava)
{ FakeJni::Constructor<FmodMediaCodecJava>{} },
{ FakeJni::Function<&FmodMediaCodecJava::init>{}, "init" },
{ FakeJni::Function<&FmodMediaCodecJava::getChannelCount>{}, "getChannelCount" },
{ FakeJni::Function<&FmodMediaCodecJava::getSampleRate>{}, "getSampleRate" },
{ FakeJni::Function<&FmodMediaCodecJava::getLength>{}, "getLength" },
{ FakeJni::Function<&FmodMediaCodecJava::read>{}, "read" },
{ FakeJni::Function<&FmodMediaCodecJava::release>{}, "release" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppRtcDeviceWrapperJava)
{ FakeJni::Constructor<AppRtcDeviceWrapperJava>{} },
{ FakeJni::Constructor<AppRtcDeviceWrapperJava, FakeJni::JLong>{} },
{ FakeJni::Function<&AppRtcDeviceWrapperJava::getSelectedAudioDeviceAsInt>{}, "getSelectedAudioDeviceAsInt" },
{ FakeJni::Function<&AppRtcDeviceWrapperJava::getSelectedAudioDeviceName>{}, "getSelectedAudioDeviceName" },
{ FakeJni::Function<&AppRtcDeviceWrapperJava::isValid>{}, "isValid" },
{ FakeJni::Function<&AppRtcDeviceWrapperJava::wrapSetCommunicationMute>{}, "wrapSetCommunicationMute" },
{ FakeJni::Function<&AppRtcDeviceWrapperJava::wrapStartCommunication>{}, "wrapStartCommunication" },
{ FakeJni::Function<&AppRtcDeviceWrapperJava::wrapStopCommunication>{}, "wrapStopCommunication" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MessageBusConnectionJava)
{ FakeJni::Constructor<MessageBusConnectionJava>{} },
{ FakeJni::Constructor<MessageBusConnectionJava, FakeJni::JLong>{} },
END_NATIVE_DESCRIPTOR

void MessageBusRawCallbackJava::run(std::shared_ptr<FakeJni::JString> json) {
    const std::string payload = json ? json->asStdString() : std::string();
    if (handler) {
        handler(payload);
        return;
    }
    if (on_message) on_message(payload);
}

std::shared_ptr<FakeJni::JString> MessageBusRequestHandlerRawJava::run(
    std::shared_ptr<FakeJni::JString> json) {
    const std::string payload = json ? json->asStdString() : std::string();
    const std::string response = handler ? handler(payload) : std::string("{}");
    return std::make_shared<FakeJni::JString>(response);
}

BEGIN_NATIVE_DESCRIPTOR(MessageBusJava)
{ FakeJni::Constructor<MessageBusJava>{} },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MessageBusRawCallbackJava)
{ FakeJni::Constructor<MessageBusRawCallbackJava>{} },
{ FakeJni::Function<&MessageBusRawCallbackJava::run>{}, "run" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebViewProtocolJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MessageBusRequestHandlerRawJava)
{ FakeJni::Constructor<MessageBusRequestHandlerRawJava>{} },
{ FakeJni::Function<&MessageBusRequestHandlerRawJava::run>{}, "run" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MemStorageConnectionJava)
{ FakeJni::Constructor<MemStorageConnectionJava>{} },
{ FakeJni::Constructor<MemStorageConnectionJava, FakeJni::JLong>{} },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeGLJavaInterfaceJava)
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onAppBridgeNotification>{}, "onAppBridgeNotification", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::getDeviceStaticParams>{}, "getDeviceStaticParams", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::setDeviceStaticParams>{}, "setDeviceStaticParams", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::showKeyboard>{}, "showKeyboard", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::hideKeyboard>{}, "hideKeyboard", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::gameLoadedCallback>{}, "gameLoadedCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::gameDidLeave>{}, "gameDidLeave", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::exitGameWithError>{}, "exitGameWithError", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onAppShellReloadNeeded>{}, "onAppShellReloadNeeded", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onDataModelNotificationCallback>{}, "onDataModelNotificationCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onLuaTextBoxChangedCallback>{}, "onLuaTextBoxChangedCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onLuaTextBoxPropertyChangedCallback>{}, "onLuaTextBoxPropertyChangedCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onVrSessionStateUpdate>{}, "onVrSessionStateUpdate", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::onExtendedAnalyticsRecvCallback>{}, "onExtendedAnalyticsRecvCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::screenOrientationChanged>{}, "screenOrientationChanged", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::listenToMotionEvents>{}, "listenToMotionEvents", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::openNativeOverlay>{}, "openNativeOverlay", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::saveImageToAlbum>{}, "saveImageToAlbum", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::getWebViewUserAgent>{}, "getWebViewUserAgent", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::getMobileAdvertisingId>{}, "getMobileAdvertisingId", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::promptNativePurchase>{}, "promptNativePurchase", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::promptNativePurchase2>{}, "promptNativePurchase", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::promptNativePurchaseWithPayload>{}, "promptNativePurchaseWithPayload", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::promptNativePurchaseWithPaymentSessionId>{}, "promptNativePurchaseWithPaymentSessionId", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceJava::promptNativePurchaseWithPaymentSessionId3>{}, "promptNativePurchaseWithPaymentSessionId", kStaticPublic },
END_NATIVE_DESCRIPTOR

void NativeObjectManagerJava::nativeObjectRegister(std::shared_ptr<FakeJni::JObject> obj,
                                                     jlong native_ptr) {
    (void)obj;
    (void)native_ptr;
}

void NativeObjectManagerJava::nativeObjectStop() {}

BEGIN_NATIVE_DESCRIPTOR(NativeObjectManagerJava)
{ FakeJni::Function<&NativeObjectManagerJava::nativeObjectRegister>{}, "register", kStaticPublic },
{ FakeJni::Function<&NativeObjectManagerJava::nativeObjectStop>{}, "stop", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeLocaleJavaInterfaceJava)
{ FakeJni::Function<&NativeLocaleJavaInterfaceJava::getLocale>{}, "getLocale", kStaticPublic },
{ FakeJni::Function<&NativeLocaleJavaInterfaceJava::getRobloxLocale>{}, "getRobloxLocale", kStaticPublic },
{ FakeJni::Function<&NativeLocaleJavaInterfaceJava::getGameLocale>{}, "getGameLocale", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SessionReporterJavaInterfaceJava)
{ FakeJni::Function<&SessionReporterJavaInterfaceJava::getAppVersion>{}, "getAppVersion", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceJava::getFilesDir>{}, "getFilesDir", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceJava::getLastLoggedInUser>{}, "getLastLoggedInUser", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceJava::getLastLoggedInUserId>{}, "getLastLoggedInUserId", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceJava::sendSessionReport>{}, "sendSessionReport", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceJava::setEventTrackingGoogleAnalytics>{}, "setEventTrackingGoogleAnalytics", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcBuildInfoJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcAudioManagerJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcAudioRecordJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcAudioTrackJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeFlagsInitResultJava)
{ FakeJni::Constructor<NativeFlagsInitResultJava>{} },
{ FakeJni::Constructor<NativeFlagsInitResultJava, FakeJni::JInt>{} },
{ FakeJni::Function<&NativeFlagsInitResultJava::addBoolean>{}, "addBoolean" },
{ FakeJni::Function<&NativeFlagsInitResultJava::getNativeFlagProviderId>{}, "getNativeFlagProviderId" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(CookieOnSetHandlerJava)
{ FakeJni::Function<&CookieOnSetHandlerJava::onSetCookie>{}, "onSetCookie" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JniCookieProtocolJava)
END_NATIVE_DESCRIPTOR

using UpdateOnSetCookieHandlerFn = void (*)(JNIEnv*, jobject, jobject);

CookieProtocolBootstrapResult run_cookie_protocol_bootstrap(FakeJni::Jvm& jvm,
                                                              const stud::linker::LoadedLibrary& lib) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    CookieProtocolBootstrapResult result;
    void* addr = lib.find_symbol(
        "Java_com_roblox_universalapp_cookie_JNICookieProtocol_updateOnSetCookieHandler");
    if (addr == nullptr) {
        std::fprintf(stderr,
                      "stud: cookie_protocol: JNICookieProtocol.updateOnSetCookieHandler not "
                      "found, skipping\n");
        return result;
    }
    auto* fn = reinterpret_cast<UpdateOnSetCookieHandlerFn>(addr);
    jobject protocol_ref = env.createLocalReference(std::make_shared<JniCookieProtocolJava>());
    jobject handler_ref = env.createLocalReference(std::make_shared<CookieOnSetHandlerJava>());
    result.called = true;
    result.trapped_abort = !call_trapping_abort(fn, jni_env, protocol_ref, handler_ref);
    clear_pending_jni_exception(jni_env, "JNICookieProtocol.updateOnSetCookieHandler");
    return result;
}

void register_app_java_classes(FakeJni::Jvm& jvm) {
    jvm.registerClass<AssetManagerJava>();
    jvm.registerClass<InsetsJava>();
    jvm.registerClass<LocaleJava>();
    jvm.registerClass<LocaleListJava>();
    jvm.registerClass<WindowInsetsCompatTypeJava>();
    jvm.registerClass<ConfigurationJava>();
    jvm.registerClass<GameActivityJava>();
    jvm.registerClass<NativeTextBoxInfoJava>();
    jvm.registerClass<NativeHelperJava>();
    jvm.registerClass<MainGameActivityJava>();
    jvm.registerClass<SurfaceJava>();
    jvm.registerClass<JavaIoFileJava>();
    jvm.registerClass<ContextJava>();
    jvm.registerClass<ActivityJava>();
    jvm.registerClass<ApplicationJava>();
    jvm.registerClass<ActivityThreadJava>();
    jvm.registerClass<FMODJava>();
    jvm.registerClass<AudioDeviceJava>();
    jvm.registerClass<EmptyArrayListJava>();
    jvm.registerClass<GameTextInputStateJava>();
    jvm.registerClass<ChannelRecordJava>();
    jvm.registerClass<ApplicationExitInfoCppJava>();
    jvm.registerClass<JNIAchievementJava>();
    jvm.registerClass<InputConnectionJava>();
    // Must be registered before NativeGLJavaInterfaceJava below; that
    // class's real getDeviceStaticParams()/setDeviceStaticParams()
    // descriptor entries use this type in their own JNI signatures, and
    // FakeJni resolves a class-typed signature against the registered
    // class table.
    jvm.registerClass<DeviceStaticParamsJava>();
    jvm.registerClass<JNIBaseUrlSetterJava>();
    jvm.registerClass<JNIAppRestarterJava>();
    jvm.registerClass<JNIExperienceProtocolJava>();
    jvm.registerClass<JNILinkingProtocolJava>();
    jvm.registerClass<VideoCodecCapabilityJava>();
    jvm.registerClass<MediaCodecInfoUtilsJava>();
    jvm.registerClass<LoggingProtocolJava>();
    jvm.registerClass<AppRtcDeviceWrapperJava>();
    jvm.registerClass<MessageBusConnectionJava>();
    jvm.registerClass<MessageBusJava>();
    jvm.registerClass<MessageBusRawCallbackJava>();
    jvm.registerClass<MessageBusRequestHandlerRawJava>();
    jvm.registerClass<WebViewProtocolJava>();
    jvm.registerClass<MemStorageConnectionJava>();
    jvm.registerClass<NativeGLJavaInterfaceJava>();
    jvm.registerClass<NativeObjectManagerJava>();
    jvm.registerClass<NativeGLInterfaceJava>();
    jvm.registerClass<NativeUserJavaInterfaceJava>();
    jvm.registerClass<NetworkUtilsJava>();
    jvm.registerClass<NativeQuoteInterfaceJava>();
    jvm.registerClass<LocalStorageManagerJava>();
    jvm.registerClass<ExperienceSessionJava>();
    jvm.registerClass<AppRatingPromptHandlerJava>();
    jvm.registerClass<IAPPurchaseManagerJava>();
    jvm.registerClass<AssertDialogUtilJava>();
    jvm.registerClass<SystemThemeProtocolJava>();
    jvm.registerClass<JNISystemThemeProtocolJava>();
    jvm.registerClass<NativeLocaleJavaInterfaceJava>();
    jvm.registerClass<SessionReporterJavaInterfaceJava>();
    jvm.registerClass<WebRtcBuildInfoJava>();
    jvm.registerClass<WebRtcAudioManagerJava>();
    jvm.registerClass<WebRtcAudioRecordJava>();
    jvm.registerClass<WebRtcAudioTrackJava>();
    jvm.registerClass<CookieOnSetHandlerJava>();
    jvm.registerClass<JniCookieProtocolJava>();
    jvm.registerClass<CookieProtocolJava>();
    jvm.registerClass<JavaUtilListJava>();
    jvm.registerClass<FmodMediaCodecJava>();
    jvm.registerClass<SystemDialogRequestJava>();
    jvm.registerClass<ISystemDialogCallbackJava>();
    jvm.registerClass<IPlatformSystemDialogHandlerJava>();
    jvm.registerClass<PlatformSystemDialogHandlerJava>();
    jvm.registerClass<FacialAgeEstimationProtocolJava>();
    // Real Kotlin `object` semantics: INSTANCE exists from class-init
    // onward, so populate it at registration rather than lazily, native
    // code reads the field directly and never calls a factory.
    PlatformSystemDialogHandlerJava::instance();
    FacialAgeEstimationProtocolJava::instance();
    jvm.registerClass<NativeFlagsInitResultJava>();
}

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceJava::getTheme() {
    return std::make_shared<FakeJni::JString>(current_theme_name() == "dark" ? "Dark" : "Light");
}

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

std::shared_ptr<FakeJni::JString> NetworkUtilsJava::getPublicIPv4Addresseses() {
    std::string out;
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0) return std::make_shared<FakeJni::JString>("");
    for (const ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) continue;
        const auto* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        if ((ntohl(in->sin_addr.s_addr) >> 24) == 127) continue;  // loopback
        char text[INET_ADDRSTRLEN] = {};
        if (::inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) == nullptr) continue;
        out += text;
        out += " : ";
    }
    ::freeifaddrs(list);
    return std::make_shared<FakeJni::JString>(out);
}

}  // namespace stud::jni_bridge

namespace stud::jni_bridge {

namespace {
struct AccountInfo {
    bool under_13 = false;
    int membership_type = 0;
    bool has_subscription = false;
};
std::mutex& account_mutex() {
    static std::mutex m;
    return m;
}
AccountInfo& account_info() {
    static AccountInfo a;
    return a;
}
}  // namespace

void record_account_info(const std::string& json) {
    const auto doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object()) {
        std::printf("stud: account report: not JSON (%zu bytes)\n", json.size());
        std::fflush(stdout);
        return;
    }
    AccountInfo info;
    if (doc.contains("isUnder13") && doc["isUnder13"].is_boolean()) {
        info.under_13 = doc["isUnder13"].get<bool>();
    }
    if (doc.contains("membershipType") && doc["membershipType"].is_number_integer()) {
        info.membership_type = doc["membershipType"].get<int>();
    }
    if (doc.contains("hasRobloxSubscription") && doc["hasRobloxSubscription"].is_boolean()) {
        info.has_subscription = doc["hasRobloxSubscription"].get<bool>();
    }
    {
        std::lock_guard<std::mutex> lock(account_mutex());
        account_info() = info;
    }
    std::printf("stud: account report: under13=%s membershipType=%d subscription=%s\n",
                info.under_13 ? "yes" : "no", info.membership_type,
                info.has_subscription ? "yes" : "no");
    std::fflush(stdout);
}

FakeJni::JBoolean NativeUserJavaInterfaceJava::getIsUnder13() {
    std::lock_guard<std::mutex> lock(account_mutex());
    return account_info().under_13;
}

FakeJni::JInt NativeUserJavaInterfaceJava::getMembershipType() {
    std::lock_guard<std::mutex> lock(account_mutex());
    return account_info().membership_type;
}

FakeJni::JBoolean NativeUserJavaInterfaceJava::getHasRobloxSubscription() {
    std::lock_guard<std::mutex> lock(account_mutex());
    return account_info().has_subscription;
}

}  // namespace stud::jni_bridge
