#include "stud/game_activity_stubs.h"

#include <functional>

#include "stud/android_framework_stubs.h"
#include "stud/trap_recovery.h"

#include <nlohmann/json.hpp>

#include <chrono>
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


BEGIN_NATIVE_DESCRIPTOR(InsetsStub)
{ FakeJni::Field<&InsetsStub::left>{}, "left" },
{ FakeJni::Field<&InsetsStub::top>{}, "top" },
{ FakeJni::Field<&InsetsStub::right>{}, "right" },
{ FakeJni::Field<&InsetsStub::bottom>{}, "bottom" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocaleStub)
{ FakeJni::Function<&LocaleStub::getLanguage>{}, "getLanguage" },
{ FakeJni::Function<&LocaleStub::getScript>{}, "getScript" },
{ FakeJni::Function<&LocaleStub::getCountry>{}, "getCountry" },
{ FakeJni::Function<&LocaleStub::getVariant>{}, "getVariant" },
// Locale.toString() is what the real getLocale() calls; without it that
// lookup misses and the engine gets no system locale at all.
{ FakeJni::Function<&LocaleStub::toString>{}, "toString" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LocaleListStub)
{ FakeJni::Function<&LocaleListStub::size>{}, "size" },
{ FakeJni::Function<&LocaleListStub::get>{}, "get" },
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

BEGIN_NATIVE_DESCRIPTOR(WindowInsetsCompatTypeStub)
{ FakeJni::Function<&WindowInsetsCompatTypeStub::captionBar>{}, "captionBar", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::displayCutout>{}, "displayCutout", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::ime>{}, "ime", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::mandatorySystemGestures>{}, "mandatorySystemGestures", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::navigationBars>{}, "navigationBars", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::statusBars>{}, "statusBars", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::systemBars>{}, "systemBars", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::systemGestures>{}, "systemGestures", kStaticPublic },
{ FakeJni::Function<&WindowInsetsCompatTypeStub::tappableElement>{}, "tappableElement", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ConfigurationStub)
{ FakeJni::Function<&ConfigurationStub::getLocales>{}, "getLocales" },
{ FakeJni::Field<&ConfigurationStub::fontScale>{}, "fontScale" },
{ FakeJni::Field<&ConfigurationStub::mcc>{}, "mcc" },
{ FakeJni::Field<&ConfigurationStub::mnc>{}, "mnc" },
{ FakeJni::Field<&ConfigurationStub::userSetLocale>{}, "userSetLocale" },
{ FakeJni::Field<&ConfigurationStub::colorMode>{}, "colorMode" },
{ FakeJni::Field<&ConfigurationStub::screenLayout>{}, "screenLayout" },
{ FakeJni::Field<&ConfigurationStub::fontWeightAdjustment>{}, "fontWeightAdjustment" },
{ FakeJni::Field<&ConfigurationStub::touchscreen>{}, "touchscreen" },
{ FakeJni::Field<&ConfigurationStub::keyboard>{}, "keyboard" },
{ FakeJni::Field<&ConfigurationStub::keyboardHidden>{}, "keyboardHidden" },
{ FakeJni::Field<&ConfigurationStub::hardKeyboardHidden>{}, "hardKeyboardHidden" },
{ FakeJni::Field<&ConfigurationStub::navigation>{}, "navigation" },
{ FakeJni::Field<&ConfigurationStub::navigationHidden>{}, "navigationHidden" },
{ FakeJni::Field<&ConfigurationStub::orientation>{}, "orientation" },
{ FakeJni::Field<&ConfigurationStub::uiMode>{}, "uiMode" },
{ FakeJni::Field<&ConfigurationStub::screenWidthDp>{}, "screenWidthDp" },
{ FakeJni::Field<&ConfigurationStub::screenHeightDp>{}, "screenHeightDp" },
{ FakeJni::Field<&ConfigurationStub::smallestScreenWidthDp>{}, "smallestScreenWidthDp" },
{ FakeJni::Field<&ConfigurationStub::densityDpi>{}, "densityDpi" },
{ FakeJni::Field<&ConfigurationStub::compatScreenWidthDp>{}, "compatScreenWidthDp" },
{ FakeJni::Field<&ConfigurationStub::compatScreenHeightDp>{}, "compatScreenHeightDp" },
{ FakeJni::Field<&ConfigurationStub::compatSmallestScreenWidthDp>{}, "compatSmallestScreenWidthDp" },
{ FakeJni::Field<&ConfigurationStub::assetsSeq>{}, "assetsSeq" },
{ FakeJni::Field<&ConfigurationStub::seq>{}, "seq" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AssetManagerStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(GameActivityStub)
{ FakeJni::Function<&GameActivityStub::finish>{}, "finish" },
{ FakeJni::Function<&GameActivityStub::setWindowFlags>{}, "setWindowFlags" },
{ FakeJni::Function<&GameActivityStub::setWindowFormat>{}, "setWindowFormat" },
{ FakeJni::Function<&GameActivityStub::getWindowInsets>{}, "getWindowInsets" },
{ FakeJni::Function<&GameActivityStub::getWaterfallInsets>{}, "getWaterfallInsets" },
{ FakeJni::Function<&GameActivityStub::setImeEditorInfo>{}, "setImeEditorInfo" },
{ FakeJni::Function<&GameActivityStub::setImeEditorInfoFields>{}, "setImeEditorInfoFields" },
{ FakeJni::Function<&GameActivityStub::getAssets>{}, "getAssets" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MainGameActivityStub)
{ FakeJni::Function<&MainGameActivityStub::bootstrapTheApp>{}, "bootstrapTheApp" },
{ FakeJni::Field<&MainGameActivityStub::nativeHelper>{}, "nativeHelper" },
{ FakeJni::Function<&MainGameActivityStub::getNativeHelper>{}, "getNativeHelper" },
{ FakeJni::Function<&MainGameActivityStub::getAppUpgradeKey>{}, "getAppUpgradeKey", kStaticPublic },
{ FakeJni::Function<&MainGameActivityStub::syncCookiesFromEngine>{}, "syncCookiesFromEngine" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<FakeJni::JString> MainGameActivityStub::getAppUpgradeKey() {
    return std::make_shared<FakeJni::JString>("AppAndroidV");
}

void MainGameActivityStub::syncCookiesFromEngine() {
    std::printf("stud: MainGameActivity.syncCookiesFromEngine() (Stud seeds the engine's cookie "
                "jar at bring-up and has no separate store to sync back to)\n");
    std::fflush(stdout);
}

MainGameActivityStub::MainGameActivityStub() : nativeHelper(std::make_shared<NativeHelperStub>()) {}

BEGIN_NATIVE_DESCRIPTOR(JavaUtilListStub)
{ FakeJni::Function<&JavaUtilListStub::size>{}, "size" },
{ FakeJni::Function<&JavaUtilListStub::isEmpty>{}, "isEmpty" },
{ FakeJni::Function<&JavaUtilListStub::get>{}, "get" },
END_NATIVE_DESCRIPTOR



namespace {
constexpr int kStaticPublicField = FakeJni::JFieldID::PUBLIC | FakeJni::JFieldID::STATIC;
}  // namespace

std::shared_ptr<PlatformSystemDialogHandlerStub> PlatformSystemDialogHandlerStub::instance() {
    if (!INSTANCE) INSTANCE = std::make_shared<PlatformSystemDialogHandlerStub>();
    return INSTANCE;
}

FakeJni::JLong IPlatformSystemDialogHandlerStub::open(
    std::shared_ptr<SystemDialogRequestStub> /*request*/,
    std::shared_ptr<ISystemDialogCallbackStub> /*callback*/) {
    std::printf("stud: SystemDialogHandler.open(). No Android dialog UI, nothing shown\n");
    std::fflush(stdout);
    return 0;
}

void IPlatformSystemDialogHandlerStub::dismiss(FakeJni::JLong /*id*/) {}
void IPlatformSystemDialogHandlerStub::dismissAll() {}

BEGIN_NATIVE_DESCRIPTOR(SystemDialogRequestStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ISystemDialogCallbackStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(IPlatformSystemDialogHandlerStub)
{ FakeJni::Function<&IPlatformSystemDialogHandlerStub::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&IPlatformSystemDialogHandlerStub::open>{}, "open" },
{ FakeJni::Function<&IPlatformSystemDialogHandlerStub::dismiss>{}, "dismiss" },
{ FakeJni::Function<&IPlatformSystemDialogHandlerStub::dismissAll>{}, "dismissAll" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PlatformSystemDialogHandlerStub)
{ FakeJni::Field<&PlatformSystemDialogHandlerStub::INSTANCE>{}, "INSTANCE", kStaticPublicField },
{ FakeJni::Function<&PlatformSystemDialogHandlerStub::isAvailable>{}, "isAvailable" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<FacialAgeEstimationProtocolStub> FacialAgeEstimationProtocolStub::instance() {
    if (!INSTANCE) INSTANCE = std::make_shared<FacialAgeEstimationProtocolStub>();
    return INSTANCE;
}

void FacialAgeEstimationProtocolStub::setListener(FakeJni::JLong native_listener_ptr) {
    std::printf("stud: FacialAgeEstimationProtocol.setListener(0x%llx)\n",
                static_cast<unsigned long long>(native_listener_ptr));
    std::fflush(stdout);
}

void FacialAgeEstimationProtocolStub::startInquiry(std::shared_ptr<FakeJni::JString> inquiry_id,
                                                    std::shared_ptr<FakeJni::JString> /*token*/) {
    // The session token is deliberately not logged; it is a real
    // credential, same rule as the .ROBLOSECURITY cookie.
    std::printf("stud: FacialAgeEstimationProtocol.startInquiry(id=%s); no face-scan SDK, "
                "nothing started\n",
                inquiry_id ? inquiry_id->asStdString().c_str() : "(null)");
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(FacialAgeEstimationProtocolStub)
{ FakeJni::Field<&FacialAgeEstimationProtocolStub::INSTANCE>{}, "INSTANCE", kStaticPublicField },
{ FakeJni::Function<&FacialAgeEstimationProtocolStub::isAvailable>{}, "isAvailable" },
{ FakeJni::Function<&FacialAgeEstimationProtocolStub::setListener>{}, "setListener" },
{ FakeJni::Function<&FacialAgeEstimationProtocolStub::startInquiry>{}, "startInquiry" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(CookieProtocolStub)
{ FakeJni::Function<&CookieProtocolStub::setCookie>{}, "setCookie", kStaticPublic },
END_NATIVE_DESCRIPTOR

void CookieProtocolStub::setCookie(std::shared_ptr<FakeJni::JString> name,
                                    std::shared_ptr<FakeJni::JString> value) {
    // Name and size only, never the value.
    std::printf("stud: CookieProtocol.setCookie: %s (%zu bytes)\n",
                name ? name->asStdString().c_str() : "", value ? value->asStdString().size() : 0u);
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(JNIBaseUrlSetterStub)
{ FakeJni::Function<&JNIBaseUrlSetterStub::setBaseUrl>{}, "setBaseUrl", kStaticPublic },
END_NATIVE_DESCRIPTOR

void JNIBaseUrlSetterStub::setBaseUrl(std::shared_ptr<FakeJni::JString> url) {
    std::printf("stud: JNIBaseUrlSetter.setBaseUrl(\"%s\")\n",
                url ? url->asStdString().c_str() : "(null)");
    std::fflush(stdout);
}

BEGIN_NATIVE_DESCRIPTOR(JNIExperienceProtocolStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JNILinkingProtocolStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JNIAppRestarterStub)
{ FakeJni::Function<&JNIAppRestarterStub::restartApp>{}, "restartApp", kStaticPublic },
END_NATIVE_DESCRIPTOR

void JNIAppRestarterStub::restartApp(std::shared_ptr<ContextStub> /*context*/,
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

BEGIN_NATIVE_DESCRIPTOR(SurfaceStub)
END_NATIVE_DESCRIPTOR



BEGIN_NATIVE_DESCRIPTOR(NativeHelperStub)
{ FakeJni::Function<&NativeHelperStub::gameActivity_hideKeyboard>{}, "gameActivity_hideKeyboard" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onAppReady>{}, "gameActivity_onAppReady" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onDidLogInReceived>{}, "gameActivity_onDidLogInReceived" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onDidLogOutReceived>{}, "gameActivity_onDidLogOutReceived" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onDidSignUp>{}, "gameActivity_onDidSignUp" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onDidSwitchAccountReceived>{}, "gameActivity_onDidSwitchAccountReceived" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onEngineInitialized>{}, "gameActivity_onEngineInitialized" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onExperienceStart>{}, "gameActivity_onExperienceStart" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onExperienceStop>{}, "gameActivity_onExperienceStop" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onFlagsFailed>{}, "gameActivity_onFlagsFailed" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onFlagsLoaded>{}, "gameActivity_onFlagsLoaded" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onGameLoaded>{}, "gameActivity_onGameLoaded" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onGameStreamingStatusChanged>{}, "gameActivity_onGameStreamingStatusChanged" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onLuaAppDidReturn>{}, "gameActivity_onLuaAppDidReturn" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onLuaTextBoxChanged>{}, "gameActivity_onLuaTextBoxChanged" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onLuaTextBoxPropertyChanged>{}, "gameActivity_onLuaTextBoxPropertyChanged" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onMotionEventListening>{}, "gameActivity_onMotionEventListening" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onRestartLuaApp>{}, "gameActivity_onRestartLuaApp" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onScanQrCode>{}, "gameActivity_onScanQrCode" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onScreenOrientationChanged>{}, "gameActivity_onScreenOrientationChanged" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_onScreenshotReady>{}, "gameActivity_onScreenshotReady" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_setAppUpgradeStatus>{}, "gameActivity_setAppUpgradeStatus" },
{ FakeJni::Function<&NativeHelperStub::gameActivity_showKeyboard>{}, "gameActivity_showKeyboard" },
END_NATIVE_DESCRIPTOR

FakeJni::JBoolean ActivityStub::runOnUiThread(std::shared_ptr<FakeJni::JObject> runnable) {
    if (!runnable) return false;
    LooperStub::getMainLooper()->post(std::move(runnable));
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

std::shared_ptr<JavaIoFileStub> ContextStub::getFilesDir() {
    return std::make_shared<JavaIoFileStub>(g_activity_files_dir);
}

std::shared_ptr<JavaIoFileStub> ContextStub::getCacheDir() {
    return std::make_shared<JavaIoFileStub>(g_activity_cache_dir);
}

BEGIN_NATIVE_DESCRIPTOR(JavaIoFileStub)
{ FakeJni::Function<&JavaIoFileStub::getAbsolutePath>{}, "getAbsolutePath" },
{ FakeJni::Function<&JavaIoFileStub::getPath>{}, "getPath" },
{ FakeJni::Function<&JavaIoFileStub::toStringJ>{}, "toString" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ContextStub)
{ FakeJni::Function<&ContextStub::getPackageName>{}, "getPackageName" },
{ FakeJni::Function<&ContextStub::getFilesDir>{}, "getFilesDir" },
{ FakeJni::Function<&ContextStub::getCacheDir>{}, "getCacheDir" },
{ FakeJni::Function<&ContextStub::getSharedPreferences>{}, "getSharedPreferences" },
{ FakeJni::Function<&ContextStub::getResources>{}, "getResources" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ActivityStub)
{ FakeJni::Function<&ActivityStub::runOnUiThread>{}, "runOnUiThread" },
{ FakeJni::Function<&ActivityStub::getApplicationContext>{}, "getApplicationContext" },
END_NATIVE_DESCRIPTOR

std::shared_ptr<ApplicationStub> ApplicationStub::singleton() {
    static auto instance = std::make_shared<ApplicationStub>();
    return instance;
}

BEGIN_NATIVE_DESCRIPTOR(ApplicationStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ActivityThreadStub)
{ FakeJni::Function<&ActivityThreadStub::getApplication>{}, "getApplication" },
{ FakeJni::Function<&ActivityThreadStub::currentActivityThread>{}, "currentActivityThread", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(FMODStub)
{ FakeJni::Function<&FMODStub::init>{}, "init", kStaticPublic },
{ FakeJni::Function<&FMODStub::checkInit>{}, "checkInit", kStaticPublic },
{ FakeJni::Function<&FMODStub::close>{}, "close", kStaticPublic },
{ FakeJni::Function<&FMODStub::getAssetManager>{}, "getAssetManager", kStaticPublic },
{ FakeJni::Function<&FMODStub::getOutputBlockSize>{}, "getOutputBlockSize", kStaticPublic },
{ FakeJni::Function<&FMODStub::getOutputSampleRate>{}, "getOutputSampleRate", kStaticPublic },
{ FakeJni::Function<&FMODStub::isBluetoothOn>{}, "isBluetoothOn", kStaticPublic },
{ FakeJni::Function<&FMODStub::lowLatencyFlag>{}, "lowLatencyFlag", kStaticPublic },
{ FakeJni::Function<&FMODStub::proAudioFlag>{}, "proAudioFlag", kStaticPublic },
{ FakeJni::Function<&FMODStub::supportsAAudio>{}, "supportsAAudio", kStaticPublic },
{ FakeJni::Function<&FMODStub::supportsLowLatency>{}, "supportsLowLatency", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AudioDeviceStub)
{ FakeJni::Constructor<AudioDeviceStub>{} },
{ FakeJni::Function<&AudioDeviceStub::init>{}, "init" },
{ FakeJni::Function<&AudioDeviceStub::close>{}, "close" },
{ FakeJni::Function<&AudioDeviceStub::write>{}, "write" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(EmptyArrayListStub)
{ FakeJni::Function<&EmptyArrayListStub::size>{}, "size" },
{ FakeJni::Function<&EmptyArrayListStub::isEmpty>{}, "isEmpty" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(GameTextInputStateStub)
{ FakeJni::Constructor<GameTextInputStateStub>{} },
{ FakeJni::Constructor<GameTextInputStateStub, std::shared_ptr<FakeJni::JString>, FakeJni::JInt,
                        FakeJni::JInt, FakeJni::JInt, FakeJni::JInt>{} },
{ FakeJni::Field<&GameTextInputStateStub::text>{}, "text" },
{ FakeJni::Field<&GameTextInputStateStub::selectionStart>{}, "selectionStart" },
{ FakeJni::Field<&GameTextInputStateStub::selectionEnd>{}, "selectionEnd" },
{ FakeJni::Field<&GameTextInputStateStub::composingRegionStart>{}, "composingRegionStart" },
{ FakeJni::Field<&GameTextInputStateStub::composingRegionEnd>{}, "composingRegionEnd" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ChannelRecordStub)
{ FakeJni::Constructor<ChannelRecordStub>{} },
{ FakeJni::Constructor<ChannelRecordStub, std::shared_ptr<FakeJni::JString>, jlong>{} },
{ FakeJni::Field<&ChannelRecordStub::name>{}, "name" },
{ FakeJni::Field<&ChannelRecordStub::id>{}, "id" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ApplicationExitInfoCppStub)
{ FakeJni::Constructor<ApplicationExitInfoCppStub>{} },
{ FakeJni::Constructor<ApplicationExitInfoCppStub, FakeJni::JInt, jlong,
                        std::shared_ptr<FakeJni::JString>>{} },
{ FakeJni::Constructor<ApplicationExitInfoCppStub, FakeJni::JInt, FakeJni::JInt, jlong,
                        std::shared_ptr<FakeJni::JString>>{} },
{ FakeJni::Constructor<ApplicationExitInfoCppStub, FakeJni::JInt, FakeJni::JInt, jlong,
                        std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>,
                        std::shared_ptr<FakeJni::JString>, jlong, jlong, FakeJni::JInt>{} },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mPid>{}, "mPid" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mSignal>{}, "mSignal" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mTimestamp>{}, "mTimestamp" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mExitReason>{}, "mExitReason" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mExitSubreason>{}, "mExitSubreason" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mDescription>{}, "mDescription" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mPss>{}, "mPss" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mRss>{}, "mRss" },
{ FakeJni::Field<&ApplicationExitInfoCppStub::mImportance>{}, "mImportance" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JNIAchievementStub)
{ FakeJni::Function<&JNIAchievementStub::grantAchievementForNativeAsync>{}, "grantAchievementForNativeAsync", kStaticPublic },
{ FakeJni::Function<&JNIAchievementStub::hasAchievedForNativeAsync>{}, "hasAchievedForNativeAsync", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(InputConnectionStub)
{ FakeJni::Function<&InputConnectionStub::setState>{}, "setState" },
{ FakeJni::Function<&InputConnectionStub::setSoftKeyboardActive>{}, "setSoftKeyboardActive" },
{ FakeJni::Function<&InputConnectionStub::restartInput>{}, "restartInput" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeGLInterfaceStub)
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

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceStub::getFilesDir() {
    return std::make_shared<FakeJni::JString>(g_native_user_interface_files_dir);
}

FakeJni::JLong NativeUserJavaInterfaceStub::getUserId() { return g_native_user_id; }

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceStub::getUsername() {
    return std::make_shared<FakeJni::JString>(g_native_username);
}

std::shared_ptr<FakeJni::JString> NativeUserJavaInterfaceStub::getDisplayName() {
    return std::make_shared<FakeJni::JString>(g_native_display_name);
}

BEGIN_NATIVE_DESCRIPTOR(NativeUserJavaInterfaceStub)
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getUserId>{}, "getUserId", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getIsUnder13>{}, "getIsUnder13", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getUsername>{}, "getUsername", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getDisplayName>{}, "getDisplayName", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getAlternateName>{}, "getAlternateName", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getPlatformName>{}, "getPlatformName", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getMembershipType>{}, "getMembershipType", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getHasRobloxSubscription>{}, "getHasRobloxSubscription", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getTheme>{}, "getTheme", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getLastLoggedInUser>{}, "getLastLoggedInUser", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getLastLoggedInUserId>{}, "getLastLoggedInUserId", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getFilesDir>{}, "getFilesDir", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::getAppVersion>{}, "getAppVersion", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::isDebuggerConnected>{}, "isDebuggerConnected", kStaticPublic },
{ FakeJni::Function<&NativeUserJavaInterfaceStub::setEventTrackingGoogleAnalytics>{}, "setEventTrackingGoogleAnalytics", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NetworkUtilsStub)
{ FakeJni::Function<&NetworkUtilsStub::getPublicIPv4Addresseses>{}, "getPublicIPv4Addresseses", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeQuoteInterfaceStub)
{ FakeJni::Function<&NativeQuoteInterfaceStub::requestResponse>{}, "requestResponse", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ExperienceSessionStub)
{ FakeJni::Function<&ExperienceSessionStub::shouldDisableExperienceIdleTimer>{}, "shouldDisableExperienceIdleTimer", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppRatingPromptHandlerStub)
{ FakeJni::Function<&AppRatingPromptHandlerStub::isAppRatingPromptAvailable>{}, "isAppRatingPromptAvailable", kStaticPublic },
{ FakeJni::Function<&AppRatingPromptHandlerStub::showAppRatingPrompt>{}, "showAppRatingPrompt", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(IAPPurchaseManagerStub)
{ FakeJni::Function<&IAPPurchaseManagerStub::getPlatformPaymentMethod>{}, "getPlatformPaymentMethod", kStaticPublic },
{ FakeJni::Function<&IAPPurchaseManagerStub::getPlatformPaymentProviderType>{}, "getPlatformPaymentProviderType", kStaticPublic },
{ FakeJni::Function<&IAPPurchaseManagerStub::invokeStore>{}, "invokeStore", kStaticPublic },
{ FakeJni::Function<&IAPPurchaseManagerStub::invokeStoreV2>{}, "invokeStoreV2", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AssertDialogUtilStub)
{ FakeJni::Function<&AssertDialogUtilStub::showAssertionPopup>{}, "showAssertionPopup", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SystemThemeProtocolStub)
{ FakeJni::Function<&SystemThemeProtocolStub::getSystemTheme>{}, "getSystemTheme", kStaticPublic },
{ FakeJni::Function<&SystemThemeProtocolStub::isSystemThemeAvailable>{}, "isSystemThemeAvailable", kStaticPublic },
END_NATIVE_DESCRIPTOR

// Exists to be found, not to be called: every method on the real class is
// a native whose body is libroblox's own. See the header.
BEGIN_NATIVE_DESCRIPTOR(JNISystemThemeProtocolStub)
END_NATIVE_DESCRIPTOR

jlong LocalStorageManagerStub::getAllocatableBytes() {
    struct statvfs st{};
    const char* path = std::getenv("HOME");
    std::string cache_root = (path ? std::string(path) : ".") + "/.cache/stud";
    if (statvfs(cache_root.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<jlong>(st.f_bavail) * static_cast<jlong>(st.f_frsize);
}

BEGIN_NATIVE_DESCRIPTOR(LocalStorageManagerStub)
{ FakeJni::Function<&LocalStorageManagerStub::getAllocatableBytes>{}, "getAllocatableBytes" },
END_NATIVE_DESCRIPTOR

void NativeGLJavaInterfaceStub::onAppBridgeNotification(std::shared_ptr<FakeJni::JString> type,
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

std::shared_ptr<DeviceStaticParamsStub> NativeGLJavaInterfaceStub::s_device_static_params;

namespace {
// Focused-TextBox state, shared with the input bridge. Guarded because
// the engine sets it from its own thread and the input poll thread reads
// it from another.
std::mutex g_text_box_mutex;
long g_active_text_box = 0;
std::string g_active_text_box_text;
NativeGLJavaInterfaceStub::TextBoxStyle g_active_text_box_style;
}  // namespace

void NativeGLJavaInterfaceStub::showKeyboard(FakeJni::JLong text_box,
                                              FakeJni::JBoolean show_native_input,
                                              std::shared_ptr<FakeJni::JByteArray> initial_text,
                                              std::shared_ptr<NativeTextBoxInfoStub> info) {
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
void NativeGLJavaInterfaceStub::gameLoadedCallback(FakeJni::JLong placeId) {
    std::printf("stud: NativeGLJavaInterface.gameLoadedCallback: placeId=%lld\n",
                static_cast<long long>(placeId));
    std::fflush(stdout);
}
void NativeGLJavaInterfaceStub::gameDidLeave() { log_engine_call("gameDidLeave()"); }
void NativeGLJavaInterfaceStub::exitGameWithError(FakeJni::JInt error) {
    std::printf("stud: NativeGLJavaInterface.exitGameWithError: %d\n", static_cast<int>(error));
    std::fflush(stdout);
}
void NativeGLJavaInterfaceStub::onAppShellReloadNeeded() {
    log_engine_call("onAppShellReloadNeeded()");
}
void NativeGLJavaInterfaceStub::onDataModelNotificationCallback(
    std::shared_ptr<FakeJni::JString> type, std::shared_ptr<FakeJni::JString>) {
    // Type only, the payload can carry account data.
    const std::string kind = type ? type->asStdString() : std::string();
    std::printf("stud: NativeGLJavaInterface.onDataModelNotificationCallback: type=%s\n",
                kind.c_str());
    std::fflush(stdout);
    if (kind == "NATIVE_EXIT" && on_native_exit) on_native_exit();
}
void NativeGLJavaInterfaceStub::onLuaTextBoxChangedCallback(std::shared_ptr<FakeJni::JString> value) {
    // Length only, never the content: a real TextBox can be a password field.
    std::printf("stud: NativeGLJavaInterface.onLuaTextBoxChangedCallback: %zu chars\n",
                value ? value->asStdString().size() : 0u);
    std::fflush(stdout);
}
void NativeGLJavaInterfaceStub::onLuaTextBoxPropertyChangedCallback() {
    if (text_input_trace_enabled()) log_engine_call("onLuaTextBoxPropertyChangedCallback()");
}
void NativeGLJavaInterfaceStub::onVrSessionStateUpdate(FakeJni::JInt) {
    // Stud has no VR session.
}
void NativeGLJavaInterfaceStub::onExtendedAnalyticsRecvCallback(
    std::shared_ptr<FakeJni::JByteArray>, FakeJni::JInt) {
    // Analytics payload Stud has nowhere to forward to.
}
void NativeGLJavaInterfaceStub::screenOrientationChanged(FakeJni::JInt orientation) {
    std::printf("stud: NativeGLJavaInterface.screenOrientationChanged: %d\n",
                static_cast<int>(orientation));
    std::fflush(stdout);
}
void NativeGLJavaInterfaceStub::listenToMotionEvents(std::shared_ptr<FakeJni::JString> motionType) {
    // Stud has no accelerometer/gyroscope to listen to.
    std::printf("stud: NativeGLJavaInterface.listenToMotionEvents: %s (no motion sensors)\n",
                motionType ? motionType->asStdString().c_str() : "");
    std::fflush(stdout);
}
void NativeGLJavaInterfaceStub::openNativeOverlay(std::shared_ptr<FakeJni::JString> a,
                                                   std::shared_ptr<FakeJni::JString>) {
    std::printf("stud: NativeGLJavaInterface.openNativeOverlay: %s (no native overlay)\n",
                a ? a->asStdString().c_str() : "");
    std::fflush(stdout);
}
void NativeGLJavaInterfaceStub::saveImageToAlbum(std::shared_ptr<FakeJni::JString>) {
    log_engine_call("saveImageToAlbum() (no photo album on this platform)");
}
namespace {
// Set once at bring-up, by the process that can reach the engine's own
// setter. Kept as a callback because this stub is a plain static with no
// library handle of its own.
std::function<void()>& webview_user_agent_reporter() {
    static std::function<void()> reporter;
    return reporter;
}
}  // namespace

void set_webview_user_agent_reporter(std::function<void()> reporter) {
    webview_user_agent_reporter() = std::move(reporter);
}

void NativeGLJavaInterfaceStub::getWebViewUserAgent() {
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
void NativeGLJavaInterfaceStub::getMobileAdvertisingId() {
    // Same shape. Stud has no advertising id, and inventing one would be a
    // fabricated identifier, not an honest placeholder.
    log_engine_call("getMobileAdvertisingId() (none on this platform)");
}
void NativeGLJavaInterfaceStub::promptNativePurchase(FakeJni::JLong,
                                                      std::shared_ptr<FakeJni::JString>) {
    log_engine_call("promptNativePurchase() (no billing)");
}
void NativeGLJavaInterfaceStub::promptNativePurchase2(FakeJni::JLong,
                                                       std::shared_ptr<FakeJni::JString>,
                                                       std::shared_ptr<FakeJni::JString>) {
    log_engine_call("promptNativePurchase() (no billing)");
}
void NativeGLJavaInterfaceStub::promptNativePurchaseWithPayload(
    FakeJni::JLong, std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>) {
    log_engine_call("promptNativePurchaseWithPayload() (no billing)");
}
void NativeGLJavaInterfaceStub::promptNativePurchaseWithPaymentSessionId(
    FakeJni::JLong, std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>) {
    log_engine_call("promptNativePurchaseWithPaymentSessionId() (no billing)");
}
void NativeGLJavaInterfaceStub::promptNativePurchaseWithPaymentSessionId3(
    FakeJni::JLong, std::shared_ptr<FakeJni::JString>, std::shared_ptr<FakeJni::JString>,
    std::shared_ptr<FakeJni::JString>) {
    log_engine_call("promptNativePurchaseWithPaymentSessionId() (no billing)");
}

void NativeGLJavaInterfaceStub::hideKeyboard() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    g_active_text_box = 0;
    g_active_text_box_text.clear();
    g_active_text_box_style = {};
    std::printf("stud: hideKeyboard: text box focus released\n");
}

long NativeGLJavaInterfaceStub::active_text_box() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    return g_active_text_box;
}

void NativeGLJavaInterfaceStub::set_active_text_box_style(const TextBoxStyle& style) {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    g_active_text_box_style = style;
}

NativeGLJavaInterfaceStub::TextBoxStyle NativeGLJavaInterfaceStub::active_text_box_style() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    return g_active_text_box_style;
}

std::string NativeGLJavaInterfaceStub::active_text_box_text() {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    return g_active_text_box_text;
}

void NativeGLJavaInterfaceStub::set_active_text_box_text(std::string text) {
    std::lock_guard<std::mutex> lock(g_text_box_mutex);
    g_active_text_box_text = std::move(text);
}

BEGIN_NATIVE_DESCRIPTOR(NativeTextBoxInfoStub)
{ FakeJni::Constructor<NativeTextBoxInfoStub>{} },
// The real (FFFFFZIIIIIIZZZ) constructor libroblox actually calls.
{ FakeJni::Constructor<NativeTextBoxInfoStub, FakeJni::JFloat, FakeJni::JFloat, FakeJni::JFloat,
                       FakeJni::JFloat, FakeJni::JFloat, FakeJni::JBoolean, FakeJni::JInt,
                       FakeJni::JInt, FakeJni::JInt, FakeJni::JInt, FakeJni::JInt, FakeJni::JInt,
                       FakeJni::JBoolean, FakeJni::JBoolean, FakeJni::JBoolean>{} },
{ FakeJni::Field<&NativeTextBoxInfoStub::font>{}, "font" },
{ FakeJni::Field<&NativeTextBoxInfoStub::fontSize>{}, "fontSize" },
{ FakeJni::Field<&NativeTextBoxInfoStub::height>{}, "height" },
{ FakeJni::Field<&NativeTextBoxInfoStub::manualFocusRelease>{}, "manualFocusRelease" },
{ FakeJni::Field<&NativeTextBoxInfoStub::multiline>{}, "multiline" },
{ FakeJni::Field<&NativeTextBoxInfoStub::returnKeyType>{}, "returnKeyType" },
{ FakeJni::Field<&NativeTextBoxInfoStub::textColor>{}, "textColor" },
{ FakeJni::Field<&NativeTextBoxInfoStub::textInputType>{}, "textInputType" },
{ FakeJni::Field<&NativeTextBoxInfoStub::textWrapped>{}, "textWrapped" },
{ FakeJni::Field<&NativeTextBoxInfoStub::width>{}, "width" },
{ FakeJni::Field<&NativeTextBoxInfoStub::x>{}, "x" },
{ FakeJni::Field<&NativeTextBoxInfoStub::xAlignment>{}, "xAlignment" },
{ FakeJni::Field<&NativeTextBoxInfoStub::y>{}, "y" },
{ FakeJni::Field<&NativeTextBoxInfoStub::yAlignment>{}, "yAlignment" },
{ FakeJni::Field<&NativeTextBoxInfoStub::editable>{}, "editable" },
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

FakeJni::JLong LoggingProtocolStub::getProcessTimestamp() {
    (void)g_process_start_primed;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - process_start_time())
                  .count();
    return static_cast<FakeJni::JLong>(ms);
}

BEGIN_NATIVE_DESCRIPTOR(VideoCodecCapabilityStub)
{ FakeJni::Constructor<VideoCodecCapabilityStub>{} },
{ FakeJni::Field<&VideoCodecCapabilityStub::codec>{}, "codec" },
{ FakeJni::Field<&VideoCodecCapabilityStub::name>{}, "name" },
{ FakeJni::Field<&VideoCodecCapabilityStub::isEncoder>{}, "isEncoder" },
{ FakeJni::Field<&VideoCodecCapabilityStub::isHardware>{}, "isHardware" },
{ FakeJni::Field<&VideoCodecCapabilityStub::maxBitrate>{}, "maxBitrate" },
{ FakeJni::Field<&VideoCodecCapabilityStub::minBitrate>{}, "minBitrate" },
{ FakeJni::Field<&VideoCodecCapabilityStub::maxFps>{}, "maxFps" },
{ FakeJni::Field<&VideoCodecCapabilityStub::minFps>{}, "minFps" },
{ FakeJni::Field<&VideoCodecCapabilityStub::maxWidth>{}, "maxWidth" },
{ FakeJni::Field<&VideoCodecCapabilityStub::minWidth>{}, "minWidth" },
{ FakeJni::Field<&VideoCodecCapabilityStub::maxHeight>{}, "maxHeight" },
{ FakeJni::Field<&VideoCodecCapabilityStub::minHeight>{}, "minHeight" },
{ FakeJni::Field<&VideoCodecCapabilityStub::maxInstances>{}, "maxInstances" },
{ FakeJni::Field<&VideoCodecCapabilityStub::profiles>{}, "profiles" },
{ FakeJni::Field<&VideoCodecCapabilityStub::levels>{}, "levels" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MediaCodecInfoUtilsStub)
{ FakeJni::Function<&MediaCodecInfoUtilsStub::getVideoCodecs>{}, "getVideoCodecs", kStaticPublic },
{ FakeJni::Function<&MediaCodecInfoUtilsStub::hevcHardwareEncodingSupported>{}, "hevcHardwareEncodingSupported", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(LoggingProtocolStub)
{ FakeJni::Function<&LoggingProtocolStub::getProcessTimestamp>{}, "getProcessTimestamp", kStaticPublic },
END_NATIVE_DESCRIPTOR

FakeJni::JInt AppRtcDeviceWrapperStub::getSelectedAudioDeviceAsInt() {
    return 1;  // WIRED_HEADSET, an ordinary desktop output
}
std::shared_ptr<FakeJni::JString> AppRtcDeviceWrapperStub::getSelectedAudioDeviceName() {
    return std::make_shared<FakeJni::JString>("Desktop Audio");
}
FakeJni::JBoolean AppRtcDeviceWrapperStub::isValid() { return true; }
void AppRtcDeviceWrapperStub::wrapSetCommunicationMute(FakeJni::JBoolean muted) {
    // Reported, not acted on: Stud has no system-level microphone mute,
    // and the engine stops reading the stream when it mutes anyway.
    std::printf("stud: audio: engine set communication mute = %d\n", muted ? 1 : 0);
    std::fflush(stdout);
}
void AppRtcDeviceWrapperStub::wrapStartCommunication() {
    std::printf("stud: audio: engine started voice communication\n");
    std::fflush(stdout);
}
void AppRtcDeviceWrapperStub::wrapStopCommunication() {
    std::printf("stud: audio: engine stopped voice communication\n");
    std::fflush(stdout);
}

// Every answer is the refusal: no hardware codec, so nothing to describe
// and nothing to read. FMOD checks init() first and takes its software
// path when it fails.
FakeJni::JBoolean FmodMediaCodecStub::init(FakeJni::JLong) { return false; }
FakeJni::JInt FmodMediaCodecStub::getChannelCount() { return 0; }
FakeJni::JInt FmodMediaCodecStub::getSampleRate() { return 0; }
FakeJni::JLong FmodMediaCodecStub::getLength() { return 0; }
FakeJni::JInt FmodMediaCodecStub::read(std::shared_ptr<FakeJni::JByteArray>, FakeJni::JInt) {
    return 0;
}
void FmodMediaCodecStub::release() {}

BEGIN_NATIVE_DESCRIPTOR(FmodMediaCodecStub)
{ FakeJni::Constructor<FmodMediaCodecStub>{} },
{ FakeJni::Function<&FmodMediaCodecStub::init>{}, "init" },
{ FakeJni::Function<&FmodMediaCodecStub::getChannelCount>{}, "getChannelCount" },
{ FakeJni::Function<&FmodMediaCodecStub::getSampleRate>{}, "getSampleRate" },
{ FakeJni::Function<&FmodMediaCodecStub::getLength>{}, "getLength" },
{ FakeJni::Function<&FmodMediaCodecStub::read>{}, "read" },
{ FakeJni::Function<&FmodMediaCodecStub::release>{}, "release" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(AppRtcDeviceWrapperStub)
{ FakeJni::Constructor<AppRtcDeviceWrapperStub>{} },
{ FakeJni::Constructor<AppRtcDeviceWrapperStub, FakeJni::JLong>{} },
{ FakeJni::Function<&AppRtcDeviceWrapperStub::getSelectedAudioDeviceAsInt>{}, "getSelectedAudioDeviceAsInt" },
{ FakeJni::Function<&AppRtcDeviceWrapperStub::getSelectedAudioDeviceName>{}, "getSelectedAudioDeviceName" },
{ FakeJni::Function<&AppRtcDeviceWrapperStub::isValid>{}, "isValid" },
{ FakeJni::Function<&AppRtcDeviceWrapperStub::wrapSetCommunicationMute>{}, "wrapSetCommunicationMute" },
{ FakeJni::Function<&AppRtcDeviceWrapperStub::wrapStartCommunication>{}, "wrapStartCommunication" },
{ FakeJni::Function<&AppRtcDeviceWrapperStub::wrapStopCommunication>{}, "wrapStopCommunication" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MessageBusConnectionStub)
{ FakeJni::Constructor<MessageBusConnectionStub>{} },
{ FakeJni::Constructor<MessageBusConnectionStub, FakeJni::JLong>{} },
END_NATIVE_DESCRIPTOR

void MessageBusRawCallbackStub::run(std::shared_ptr<FakeJni::JString> json) {
    const std::string payload = json ? json->asStdString() : std::string();
    if (handler) {
        handler(payload);
        return;
    }
    if (on_message) on_message(payload);
}

std::shared_ptr<FakeJni::JString> MessageBusRequestHandlerRawStub::run(
    std::shared_ptr<FakeJni::JString> json) {
    const std::string payload = json ? json->asStdString() : std::string();
    const std::string response = handler ? handler(payload) : std::string("{}");
    return std::make_shared<FakeJni::JString>(response);
}

BEGIN_NATIVE_DESCRIPTOR(MessageBusStub)
{ FakeJni::Constructor<MessageBusStub>{} },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MessageBusRawCallbackStub)
{ FakeJni::Constructor<MessageBusRawCallbackStub>{} },
{ FakeJni::Function<&MessageBusRawCallbackStub::run>{}, "run" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebViewProtocolStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MessageBusRequestHandlerRawStub)
{ FakeJni::Constructor<MessageBusRequestHandlerRawStub>{} },
{ FakeJni::Function<&MessageBusRequestHandlerRawStub::run>{}, "run" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MemStorageConnectionStub)
{ FakeJni::Constructor<MemStorageConnectionStub>{} },
{ FakeJni::Constructor<MemStorageConnectionStub, FakeJni::JLong>{} },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeGLJavaInterfaceStub)
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onAppBridgeNotification>{}, "onAppBridgeNotification", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::getDeviceStaticParams>{}, "getDeviceStaticParams", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::setDeviceStaticParams>{}, "setDeviceStaticParams", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::showKeyboard>{}, "showKeyboard", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::hideKeyboard>{}, "hideKeyboard", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::gameLoadedCallback>{}, "gameLoadedCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::gameDidLeave>{}, "gameDidLeave", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::exitGameWithError>{}, "exitGameWithError", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onAppShellReloadNeeded>{}, "onAppShellReloadNeeded", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onDataModelNotificationCallback>{}, "onDataModelNotificationCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onLuaTextBoxChangedCallback>{}, "onLuaTextBoxChangedCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onLuaTextBoxPropertyChangedCallback>{}, "onLuaTextBoxPropertyChangedCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onVrSessionStateUpdate>{}, "onVrSessionStateUpdate", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::onExtendedAnalyticsRecvCallback>{}, "onExtendedAnalyticsRecvCallback", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::screenOrientationChanged>{}, "screenOrientationChanged", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::listenToMotionEvents>{}, "listenToMotionEvents", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::openNativeOverlay>{}, "openNativeOverlay", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::saveImageToAlbum>{}, "saveImageToAlbum", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::getWebViewUserAgent>{}, "getWebViewUserAgent", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::getMobileAdvertisingId>{}, "getMobileAdvertisingId", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::promptNativePurchase>{}, "promptNativePurchase", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::promptNativePurchase2>{}, "promptNativePurchase", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::promptNativePurchaseWithPayload>{}, "promptNativePurchaseWithPayload", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::promptNativePurchaseWithPaymentSessionId>{}, "promptNativePurchaseWithPaymentSessionId", kStaticPublic },
{ FakeJni::Function<&NativeGLJavaInterfaceStub::promptNativePurchaseWithPaymentSessionId3>{}, "promptNativePurchaseWithPaymentSessionId", kStaticPublic },
END_NATIVE_DESCRIPTOR

void NativeObjectManagerStub::nativeObjectRegister(std::shared_ptr<FakeJni::JObject> obj,
                                                     jlong native_ptr) {
    (void)obj;
    (void)native_ptr;
}

void NativeObjectManagerStub::nativeObjectStop() {}

BEGIN_NATIVE_DESCRIPTOR(NativeObjectManagerStub)
{ FakeJni::Function<&NativeObjectManagerStub::nativeObjectRegister>{}, "register", kStaticPublic },
{ FakeJni::Function<&NativeObjectManagerStub::nativeObjectStop>{}, "stop", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeLocaleJavaInterfaceStub)
{ FakeJni::Function<&NativeLocaleJavaInterfaceStub::getLocale>{}, "getLocale", kStaticPublic },
{ FakeJni::Function<&NativeLocaleJavaInterfaceStub::getRobloxLocale>{}, "getRobloxLocale", kStaticPublic },
{ FakeJni::Function<&NativeLocaleJavaInterfaceStub::getGameLocale>{}, "getGameLocale", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SessionReporterJavaInterfaceStub)
{ FakeJni::Function<&SessionReporterJavaInterfaceStub::getAppVersion>{}, "getAppVersion", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceStub::getFilesDir>{}, "getFilesDir", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceStub::getLastLoggedInUser>{}, "getLastLoggedInUser", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceStub::getLastLoggedInUserId>{}, "getLastLoggedInUserId", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceStub::sendSessionReport>{}, "sendSessionReport", kStaticPublic },
{ FakeJni::Function<&SessionReporterJavaInterfaceStub::setEventTrackingGoogleAnalytics>{}, "setEventTrackingGoogleAnalytics", kStaticPublic },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcBuildInfoStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcAudioManagerStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcAudioRecordStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(WebRtcAudioTrackStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(NativeFlagsInitResultStub)
{ FakeJni::Constructor<NativeFlagsInitResultStub>{} },
{ FakeJni::Constructor<NativeFlagsInitResultStub, FakeJni::JInt>{} },
{ FakeJni::Function<&NativeFlagsInitResultStub::addBoolean>{}, "addBoolean" },
{ FakeJni::Function<&NativeFlagsInitResultStub::getNativeFlagProviderId>{}, "getNativeFlagProviderId" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(CookieOnSetHandlerStub)
{ FakeJni::Function<&CookieOnSetHandlerStub::onSetCookie>{}, "onSetCookie" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JniCookieProtocolStub)
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
    jobject protocol_ref = env.createLocalReference(std::make_shared<JniCookieProtocolStub>());
    jobject handler_ref = env.createLocalReference(std::make_shared<CookieOnSetHandlerStub>());
    result.called = true;
    result.trapped_abort = !call_trapping_abort(fn, jni_env, protocol_ref, handler_ref);
    clear_pending_jni_exception(jni_env, "JNICookieProtocol.updateOnSetCookieHandler");
    return result;
}

void register_game_activity_stubs(FakeJni::Jvm& jvm) {
    jvm.registerClass<AssetManagerStub>();
    jvm.registerClass<InsetsStub>();
    jvm.registerClass<LocaleStub>();
    jvm.registerClass<LocaleListStub>();
    jvm.registerClass<WindowInsetsCompatTypeStub>();
    jvm.registerClass<ConfigurationStub>();
    jvm.registerClass<GameActivityStub>();
    jvm.registerClass<NativeTextBoxInfoStub>();
    jvm.registerClass<NativeHelperStub>();
    jvm.registerClass<MainGameActivityStub>();
    jvm.registerClass<SurfaceStub>();
    jvm.registerClass<JavaIoFileStub>();
    jvm.registerClass<ContextStub>();
    jvm.registerClass<ActivityStub>();
    jvm.registerClass<ApplicationStub>();
    jvm.registerClass<ActivityThreadStub>();
    jvm.registerClass<FMODStub>();
    jvm.registerClass<AudioDeviceStub>();
    jvm.registerClass<EmptyArrayListStub>();
    jvm.registerClass<GameTextInputStateStub>();
    jvm.registerClass<ChannelRecordStub>();
    jvm.registerClass<ApplicationExitInfoCppStub>();
    jvm.registerClass<JNIAchievementStub>();
    jvm.registerClass<InputConnectionStub>();
    // Must be registered before NativeGLJavaInterfaceStub below; that
    // class's real getDeviceStaticParams()/setDeviceStaticParams()
    // descriptor entries use this type in their own JNI signatures, and
    // FakeJni resolves a class-typed signature against the registered
    // class table.
    jvm.registerClass<DeviceStaticParamsStub>();
    jvm.registerClass<JNIBaseUrlSetterStub>();
    jvm.registerClass<JNIAppRestarterStub>();
    jvm.registerClass<JNIExperienceProtocolStub>();
    jvm.registerClass<JNILinkingProtocolStub>();
    jvm.registerClass<VideoCodecCapabilityStub>();
    jvm.registerClass<MediaCodecInfoUtilsStub>();
    jvm.registerClass<LoggingProtocolStub>();
    jvm.registerClass<AppRtcDeviceWrapperStub>();
    jvm.registerClass<MessageBusConnectionStub>();
    jvm.registerClass<MessageBusStub>();
    jvm.registerClass<MessageBusRawCallbackStub>();
    jvm.registerClass<MessageBusRequestHandlerRawStub>();
    jvm.registerClass<WebViewProtocolStub>();
    jvm.registerClass<MemStorageConnectionStub>();
    jvm.registerClass<NativeGLJavaInterfaceStub>();
    jvm.registerClass<NativeObjectManagerStub>();
    jvm.registerClass<NativeGLInterfaceStub>();
    jvm.registerClass<NativeUserJavaInterfaceStub>();
    jvm.registerClass<NetworkUtilsStub>();
    jvm.registerClass<NativeQuoteInterfaceStub>();
    jvm.registerClass<LocalStorageManagerStub>();
    jvm.registerClass<ExperienceSessionStub>();
    jvm.registerClass<AppRatingPromptHandlerStub>();
    jvm.registerClass<IAPPurchaseManagerStub>();
    jvm.registerClass<AssertDialogUtilStub>();
    jvm.registerClass<SystemThemeProtocolStub>();
    jvm.registerClass<JNISystemThemeProtocolStub>();
    jvm.registerClass<NativeLocaleJavaInterfaceStub>();
    jvm.registerClass<SessionReporterJavaInterfaceStub>();
    jvm.registerClass<WebRtcBuildInfoStub>();
    jvm.registerClass<WebRtcAudioManagerStub>();
    jvm.registerClass<WebRtcAudioRecordStub>();
    jvm.registerClass<WebRtcAudioTrackStub>();
    jvm.registerClass<CookieOnSetHandlerStub>();
    jvm.registerClass<JniCookieProtocolStub>();
    jvm.registerClass<CookieProtocolStub>();
    jvm.registerClass<JavaUtilListStub>();
    jvm.registerClass<FmodMediaCodecStub>();
    jvm.registerClass<SystemDialogRequestStub>();
    jvm.registerClass<ISystemDialogCallbackStub>();
    jvm.registerClass<IPlatformSystemDialogHandlerStub>();
    jvm.registerClass<PlatformSystemDialogHandlerStub>();
    jvm.registerClass<FacialAgeEstimationProtocolStub>();
    // Real Kotlin `object` semantics: INSTANCE exists from class-init
    // onward, so populate it at registration rather than lazily, native
    // code reads the field directly and never calls a factory.
    PlatformSystemDialogHandlerStub::instance();
    FacialAgeEstimationProtocolStub::instance();
    jvm.registerClass<NativeFlagsInitResultStub>();
}

}  // namespace stud::jni_bridge
