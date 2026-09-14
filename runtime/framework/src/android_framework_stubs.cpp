#include "stud/android_framework_stubs.h"

#include <unistd.h>
#include <strings.h>

#include <algorithm>
#include <fstream>
#include <cctype>

#include "stud/bionic_jvm.h"
// For ContextStub: DeviceUtilsStub::getScreenPhysicalSizeInMillimeters takes a
// real android.content.Context, and FakeJni computes the JNI signature from
// the C++ type, so the complete type is needed here (the header can only
// forward-declare it, game_activity_stubs.h includes that header).
#include "stud/game_activity_stubs.h"

#include <jnivm/class.h>
#include <jnivm/env.h>
#include <jnivm/internal/findclass.h>
#include <jnivm/vm.h>
#include <jnivm/array.h>
#include <jnivm/string.h>
#include <jnivm/weak.h>

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace stud::jni_bridge {

BEGIN_NATIVE_DESCRIPTOR(LooperStub)
{ FakeJni::Function<&LooperStub::getMainLooper>{}, "getMainLooper" },
{ FakeJni::Function<&LooperStub::myLooper>{}, "myLooper" },
{ FakeJni::Function<&LooperStub::prepare>{}, "prepare" },
{ FakeJni::Function<&LooperStub::loop>{}, "loop" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(HandlerStub)
{ FakeJni::Constructor<HandlerStub>{} },
{ FakeJni::Constructor<HandlerStub, std::shared_ptr<LooperStub>>{} },
{ FakeJni::Function<&HandlerStub::post>{}, "post" },
{ FakeJni::Function<&HandlerStub::postDelayed>{}, "postDelayed" },
{ FakeJni::Function<&HandlerStub::getLooper>{}, "getLooper" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(HandlerThreadStub)
{ FakeJni::Constructor<HandlerThreadStub>{} },
{ FakeJni::Constructor<HandlerThreadStub, std::shared_ptr<FakeJni::JString>>{} },
{ FakeJni::Function<&HandlerThreadStub::start>{}, "start" },
{ FakeJni::Function<&HandlerThreadStub::getLooper>{}, "getLooper" },
{ FakeJni::Function<&HandlerThreadStub::quit>{}, "quit" },
{ FakeJni::Function<&HandlerThreadStub::quitSafely>{}, "quitSafely" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ClassLoaderStub)
{ FakeJni::Function<&ClassLoaderStub::loadClass>{}, "loadClass" },
{ FakeJni::Function<&ClassLoaderStub::findClass>{}, "findClass" },
END_NATIVE_DESCRIPTOR

namespace {
constexpr int kStaticPublicField = FakeJni::JFieldID::PUBLIC | FakeJni::JFieldID::STATIC;
constexpr int kStaticPublicMethod = FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC;
}  // namespace

// android.os.Build's fields are all `public static final`. Registering them
// without STATIC meant every real lookup missed, live-caught during a game
// launch as `GetFieldID MISS class=android/os/Build static field=BOARD` (and
// BOOTLOADER, BRAND, DEVICE, FINGERPRINT, HARDWARE, MANUFACTURER, MODEL,
// PRODUCT, TAGS, USER), each followed by jnivm's own `GetField field is null`.
BEGIN_NATIVE_DESCRIPTOR(BuildStub)
{ FakeJni::Field<&BuildStub::MANUFACTURER>{}, "MANUFACTURER", kStaticPublicField },
{ FakeJni::Field<&BuildStub::BRAND>{}, "BRAND", kStaticPublicField },
{ FakeJni::Field<&BuildStub::MODEL>{}, "MODEL", kStaticPublicField },
{ FakeJni::Field<&BuildStub::DEVICE>{}, "DEVICE", kStaticPublicField },
{ FakeJni::Field<&BuildStub::PRODUCT>{}, "PRODUCT", kStaticPublicField },
{ FakeJni::Field<&BuildStub::HARDWARE>{}, "HARDWARE", kStaticPublicField },
{ FakeJni::Field<&BuildStub::BOARD>{}, "BOARD", kStaticPublicField },
{ FakeJni::Field<&BuildStub::FINGERPRINT>{}, "FINGERPRINT", kStaticPublicField },
{ FakeJni::Field<&BuildStub::ID>{}, "ID", kStaticPublicField },
{ FakeJni::Field<&BuildStub::TAGS>{}, "TAGS", kStaticPublicField },
{ FakeJni::Field<&BuildStub::TYPE>{}, "TYPE", kStaticPublicField },
{ FakeJni::Field<&BuildStub::BOOTLOADER>{}, "BOOTLOADER", kStaticPublicField },
{ FakeJni::Field<&BuildStub::USER>{}, "USER", kStaticPublicField },
END_NATIVE_DESCRIPTOR

// android.os.Build$VERSION's fields are `public static final` too, the
// same fix as BuildStub above, which was applied there and missed here.
// Live-caught during a game launch as `GetFieldID MISS
// class=android/os/Build$VERSION static field=SDK_INT sig=I`, repeatedly.
BEGIN_NATIVE_DESCRIPTOR(BuildVersionStub)
{ FakeJni::Field<&BuildVersionStub::SDK_INT>{}, "SDK_INT", kStaticPublicField },
{ FakeJni::Field<&BuildVersionStub::RELEASE>{}, "RELEASE", kStaticPublicField },
{ FakeJni::Field<&BuildVersionStub::SECURITY_PATCH>{}, "SECURITY_PATCH", kStaticPublicField },
{ FakeJni::Field<&BuildVersionStub::INCREMENTAL>{}, "INCREMENTAL", kStaticPublicField },
{ FakeJni::Field<&BuildVersionStub::CODENAME>{}, "CODENAME", kStaticPublicField },
END_NATIVE_DESCRIPTOR

// Also `public static`, same live-caught miss during a game launch.
BEGIN_NATIVE_DESCRIPTOR(DebugStub)
{ FakeJni::Function<&DebugStub::isDebuggerConnected>{}, "isDebuggerConnected", kStaticPublicMethod },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PointStub)
{ FakeJni::Constructor<PointStub>{} },
{ FakeJni::Constructor<PointStub, FakeJni::JInt, FakeJni::JInt>{} },
{ FakeJni::Field<&PointStub::x>{}, "x" },
{ FakeJni::Field<&PointStub::y>{}, "y" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceUtilsStub)
{ FakeJni::Function<&DeviceUtilsStub::getScreenPhysicalSizeInMillimeters>{},
  "getScreenPhysicalSizeInMillimeters", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
END_NATIVE_DESCRIPTOR

std::shared_ptr<PointStub> DeviceUtilsStub::getScreenPhysicalSizeInMillimeters(
    std::shared_ptr<ContextStub> /*context*/) {
    auto metrics = ResourcesStub().getDisplayMetrics();
    auto to_mm = [](FakeJni::JInt pixels, FakeJni::JFloat dpi) -> FakeJni::JInt {
        if (dpi <= 0.0f) return 0;
        return static_cast<FakeJni::JInt>((static_cast<float>(pixels) / dpi) * 25.4f);
    };
    auto point = std::make_shared<PointStub>(to_mm(metrics->widthPixels, metrics->xdpi),
                                              to_mm(metrics->heightPixels, metrics->ydpi));
    // The panel does not change size. Said once, rather than once per
    // call, the engine asks on every renderer rebuild.
    static bool announced_screen_mm = false;
    if (!announced_screen_mm) {
        announced_screen_mm = true;
        std::printf("stud: DeviceUtils.getScreenPhysicalSizeInMillimeters() -> %dx%d mm\n",
        static_cast<int>(point->x), static_cast<int>(point->y));
        std::fflush(stdout);
    }
    return point;
}

BEGIN_NATIVE_DESCRIPTOR(JavaLangSystemStub)
{ FakeJni::Function<&JavaLangSystemStub::identityHashCode>{}, "identityHashCode", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
{ FakeJni::Function<&JavaLangSystemStub::currentTimeMillis>{}, "currentTimeMillis", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
{ FakeJni::Function<&JavaLangSystemStub::nanoTime>{}, "nanoTime", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
END_NATIVE_DESCRIPTOR

FakeJni::JInt JavaLangSystemStub::identityHashCode(std::shared_ptr<FakeJni::JObject> obj) {
    auto value = reinterpret_cast<std::uintptr_t>(obj.get());
    return static_cast<FakeJni::JInt>((value >> 4) & 0x7fffffff);
}

FakeJni::JLong JavaLangSystemStub::currentTimeMillis() {
    return static_cast<FakeJni::JLong>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

FakeJni::JLong JavaLangSystemStub::nanoTime() {
    return static_cast<FakeJni::JLong>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

BEGIN_NATIVE_DESCRIPTOR(JavaLangErrorStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangExceptionStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangRuntimeExceptionStub)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangLongStub)
{ FakeJni::Constructor<JavaLangLongStub>{} },
{ FakeJni::Constructor<JavaLangLongStub, FakeJni::JLong>{} },
{ FakeJni::Function<&JavaLangLongStub::longValue>{}, "longValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangIntegerStub)
{ FakeJni::Constructor<JavaLangIntegerStub>{} },
{ FakeJni::Constructor<JavaLangIntegerStub, FakeJni::JInt>{} },
{ FakeJni::Function<&JavaLangIntegerStub::intValue>{}, "intValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangBooleanStub)
{ FakeJni::Constructor<JavaLangBooleanStub>{} },
{ FakeJni::Constructor<JavaLangBooleanStub, FakeJni::JBoolean>{} },
{ FakeJni::Function<&JavaLangBooleanStub::booleanValue>{}, "booleanValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangDoubleStub)
{ FakeJni::Constructor<JavaLangDoubleStub>{} },
{ FakeJni::Constructor<JavaLangDoubleStub, FakeJni::JDouble>{} },
{ FakeJni::Function<&JavaLangDoubleStub::doubleValue>{}, "doubleValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaUtilIteratorStub)
{ FakeJni::Function<&JavaUtilIteratorStub::hasNext>{}, "hasNext" },
{ FakeJni::Function<&JavaUtilIteratorStub::next>{}, "next" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaUtilHashSetStub)
{ FakeJni::Constructor<JavaUtilHashSetStub>{} },
{ FakeJni::Function<&JavaUtilHashSetStub::add>{}, "add" },
{ FakeJni::Function<&JavaUtilHashSetStub::contains>{}, "contains" },
{ FakeJni::Function<&JavaUtilHashSetStub::size>{}, "size" },
{ FakeJni::Function<&JavaUtilHashSetStub::isEmpty>{}, "isEmpty" },
{ FakeJni::Function<&JavaUtilHashSetStub::iterator>{}, "iterator" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaUtilHashMapStub)
{ FakeJni::Constructor<JavaUtilHashMapStub>{} },
{ FakeJni::Function<&JavaUtilHashMapStub::put>{}, "put" },
{ FakeJni::Function<&JavaUtilHashMapStub::get>{}, "get" },
{ FakeJni::Function<&JavaUtilHashMapStub::containsKey>{}, "containsKey" },
{ FakeJni::Function<&JavaUtilHashMapStub::size>{}, "size" },
{ FakeJni::Function<&JavaUtilHashMapStub::isEmpty>{}, "isEmpty" },
END_NATIVE_DESCRIPTOR

void LooperStub::start(FakeJni::Jvm& jvm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable() || quit_) return;
    thread_ = std::thread([this, &jvm] { pump(jvm); });
}

void LooperStub::post(std::shared_ptr<FakeJni::JObject> runnable) {
    if (!runnable) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(runnable));
    }
    cv_.notify_one();
}

void LooperStub::quit() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
}

void LooperStub::stop() {
    quit();
    if (thread_.joinable()) thread_.join();
}

void LooperStub::dispatch_one(FakeJni::Jvm& jvm, std::shared_ptr<FakeJni::JObject> runnable) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);
    jobject obj = env.createLocalReference(std::move(runnable));
    jclass cls = jni_env->GetObjectClass(obj);
    if (!cls) {
        std::fprintf(stderr, "stud: Looper(%s): posted Runnable has no resolvable class, skipping\n",
                      debug_name_.c_str());
        std::fflush(stderr);
        return;
    }
    jmethodID run_id = jni_env->GetMethodID(cls, "run", "()V");
    if (!run_id) {
        std::fprintf(stderr, "stud: Looper(%s): posted Runnable has no run()V, skipping\n",
                      debug_name_.c_str());
        std::fflush(stderr);
        return;
    }
    jni_env->CallVoidMethod(obj, run_id);
}

void LooperStub::pump(FakeJni::Jvm& jvm) {
    static_cast<BionicAwareJvm&>(jvm).ensure_env_for_current_thread();
    for (;;) {
        std::shared_ptr<FakeJni::JObject> runnable;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return quit_ || !queue_.empty(); });
            if (queue_.empty() && quit_) return;
            runnable = std::move(queue_.front());
            queue_.pop_front();
        }
        dispatch_one(jvm, std::move(runnable));
    }
}

void LooperStub::drain_pending(FakeJni::Jvm& jvm) {
    for (;;) {
        std::shared_ptr<FakeJni::JObject> runnable;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) return;
            runnable = std::move(queue_.front());
            queue_.pop_front();
        }
        dispatch_one(jvm, std::move(runnable));
    }
}

std::shared_ptr<LooperStub> LooperStub::get_or_create_main_looper(FakeJni::Jvm& jvm) {
    static std::mutex init_mutex;
    static std::shared_ptr<LooperStub> instance;
    std::lock_guard<std::mutex> lock(init_mutex);
    if (!instance) {
        // Real Android semantics: the main Looper runs ON the real
        // process main thread, never a spawned worker (see this
        // class's own header doc comment), deliberately does NOT
        // call start() here. The real main thread drains it directly
        // via drain_pending() (see activity_thread.h).
        instance = std::make_shared<LooperStub>("main");
    }
    return instance;
}

std::shared_ptr<LooperStub> LooperStub::getMainLooper() {
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    return get_or_create_main_looper(env.getVM());
}

std::shared_ptr<LooperStub> LooperStub::myLooper() {
    // Real semantics: null unless the calling thread has its own
    // prepared Looper. No real caller of this codebase's own bring-up
    // was found relying on that distinction (see class-level comment on
    // prepare()/loop()), returning the main looper is a safe,
    // documented simplification, not a silent lie about a case that
    // matters here.
    return getMainLooper();
}

HandlerStub::HandlerStub() {
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    looper_ = LooperStub::get_or_create_main_looper(env.getVM());
}

HandlerStub::HandlerStub(std::shared_ptr<LooperStub> looper) : looper_(std::move(looper)) {
    if (!looper_) {
        auto& env = FakeJni::JniEnvContext().getJniEnv();
        looper_ = LooperStub::get_or_create_main_looper(env.getVM());
    }
}

FakeJni::JBoolean HandlerStub::post(std::shared_ptr<FakeJni::JObject> runnable) {
    if (!runnable || !looper_) return false;
    looper_->post(std::move(runnable));
    return true;
}

FakeJni::JBoolean HandlerStub::postDelayed(std::shared_ptr<FakeJni::JObject> runnable,
                                            FakeJni::JLong delay_millis) {
    if (!runnable || !looper_) return false;
    auto looper = looper_;
    std::thread([looper, runnable, delay_millis]() mutable {
        if (delay_millis > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_millis));
        }
        looper->post(std::move(runnable));
    }).detach();
    return true;
}

HandlerThreadStub::HandlerThreadStub() : looper_(std::make_shared<LooperStub>("HandlerThread")) {}

HandlerThreadStub::HandlerThreadStub(std::shared_ptr<FakeJni::JString> name)
    : looper_(std::make_shared<LooperStub>(name ? static_cast<std::string>(*name) : "HandlerThread")) {}

void HandlerThreadStub::start() {
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    looper_->start(env.getVM());
}

std::shared_ptr<FakeJni::JClass> ClassLoaderStub::loadClass(std::shared_ptr<FakeJni::JString> name) {
    return findClass(std::move(name));
}

std::shared_ptr<FakeJni::JClass> ClassLoaderStub::findClass(std::shared_ptr<FakeJni::JString> name) {
    if (!name) return nullptr;
    std::string dotted = *name;
    for (auto& c : dotted) {
        if (c == '.') c = '/';
    }
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    jclass raw = env.FindClass(dotted.c_str());
    if (!raw) return nullptr;
    return std::dynamic_pointer_cast<FakeJni::JClass>(env.resolveReference(raw));
}

std::shared_ptr<ClassLoaderStub> shared_class_loader() {
    static auto singleton = std::make_shared<ClassLoaderStub>();
    return singleton;
}

// Real java.lang.String methods the engine actually calls, attached to
// jnivm's own canonical String class rather than a competing stub of the
// same name (that collision is exactly what killed the engine's own
// designated thread twice before; see ByteBuffer/ClassMeta).
//
// Live-caught on the typing path, once per keystroke:
//   STUD_DIAG GetMethodID MISS class=`java/lang/String`
//       method=`getBytes` sig=`(Ljava/lang/String;)[B`
// The engine converts the text it is handed into bytes with
// String.getBytes(charsetName), and jnivm ships no getBytes at all, so
// the lookup returned null and the text could never be read. Text
// delivery was reaching the engine the whole time; this is where it was
// being dropped.
void register_java_lang_string_methods(FakeJni::Jvm& jvm) {
    auto* vm = jnivm::VM::FromJavaVM(&jvm);
    if (vm == nullptr) return;
    auto* env = vm->GetEnv().get();
    if (env == nullptr) return;
    auto java_lang_string = jnivm::InternalFindClass(env, "java/lang/String");
    if (!java_lang_string) return;

    // The charset name is accepted and ignored on purpose: jnivm's String
    // holds a std::string that is already UTF-8, which is what every
    // charset Roblox asks for here (UTF-8, and the ASCII subsets) yields
    // for the same content. Converting to a genuinely different encoding
    // would need a real charset table Stud does not have, and inventing
    // one would corrupt text rather than fail visibly.
    auto to_bytes = [](jnivm::ENV* e, jnivm::Object* self) -> std::shared_ptr<FakeJni::JByteArray> {
        auto* str = dynamic_cast<jnivm::String*>(self);
        const std::string value = str != nullptr ? static_cast<const std::string&>(*str) : "";
        auto out = std::make_shared<FakeJni::JByteArray>(static_cast<FakeJni::JInt>(value.size()));
        for (size_t i = 0; i < value.size(); ++i) {
            (*out)[static_cast<FakeJni::JInt>(i)] = static_cast<FakeJni::JByte>(value[i]);
        }
        (void)e;
        return out;
    };

    java_lang_string->HookInstanceFunction(
        env, "getBytes",
        [to_bytes](jnivm::ENV* e, jnivm::Object* self,
                   std::shared_ptr<FakeJni::JString>) -> std::shared_ptr<FakeJni::JByteArray> {
            return to_bytes(e, self);
        });
    // The no-argument overload means "the platform default charset",
    // which on Android is always UTF-8, the same answer.
    java_lang_string->HookInstanceFunction(
        env, "getBytes",
        [to_bytes](jnivm::ENV* e, jnivm::Object* self) -> std::shared_ptr<FakeJni::JByteArray> {
            return to_bytes(e, self);
        });
}

void register_java_lang_class_methods(FakeJni::Jvm& jvm) {
    // jnivm's own canonical java/lang/Class object, the real type
    // GetObjectClass() returns. Attach the real getClassLoader() here
    // instead of declaring a competing stub class for the same name.
    auto* vm = jnivm::VM::FromJavaVM(&jvm);
    if (vm == nullptr) return;
    auto* env = vm->GetEnv().get();
    if (env == nullptr) return;
    auto java_lang_class = jnivm::InternalFindClass(env, "java/lang/Class");
    if (!java_lang_class) return;
    // HookInstanceFunction, not Hook, the same trap that kept
    // WeakReference.get() missing (see its comment below): Hook() takes its
    // binding from Function<T>::type, which for a plain lambda is
    // FunctionType::None, so the method lands as STATIC and the engine's
    // instance lookup never finds it.
    //
    // First attempt at this was reverted because answering getClassLoader sends
    // the engine down its reflective loadClass path, which then resolves
    // classes Stud did not have, the app stopped short of Home and the idle
    // frame rate collapsed. Those classes are registered now
    // (PlatformSystemDialogHandlerStub, FacialAgeEstimationProtocolStub), so
    // the path has somewhere to land. If this ever regresses again, the symptom
    // to look for is the app stalling at RootSwitchNavigator instead of Home.
    java_lang_class->HookInstanceFunction(
        env, "getClassLoader",
        [](jnivm::ENV*, jnivm::Object*) -> std::shared_ptr<ClassLoaderStub> {
            return shared_class_loader();
        });

    // java.lang.ref.WeakReference.get(), jnivm has its own real
    // `jnivm::Weak` registered under this exact class name (so a
    // competing stub would collide, per the ByteBuffer/Class lesson),
    // but it exposes no methods. Djinni's proxy cache calls get() on
    // one during platform registration, and the missing method showed
    // up as "djinni (djinni_support.cpp:339): GetMethodID returned
    // null". Hook the real method onto the canonical class instead.
    auto weak_reference = jnivm::InternalFindClass(env, "java/lang/ref/WeakReference");
    if (weak_reference) {
        // HookInstanceFunction, not Hook: `Class::Hook` takes its binding
        // from `Function<T>::type`, which for a plain lambda is
        // `FunctionType::None`, so a lambda hooked with Hook() lands as
        // a STATIC method and an instance lookup never finds it. That is
        // exactly why `get()` stayed missing through two earlier attempts.
        // HookInstanceFunction forces `FunctionType::Instance`, and the
        // concrete `Weak*` receiver satisfies jnivm's own runtime
        // typecheck (Weak is registered as java/lang/ref/WeakReference).
        weak_reference->HookInstanceFunction(
            env, "get", [](jnivm::ENV*, jnivm::Weak* self) -> std::shared_ptr<jnivm::Object> {
                if (self == nullptr) return nullptr;
                return self->wrapped.lock();
            });
        // The constructor is what Djinni actually needed (live-named by
        // the GetMethodID MISS diagnostic:
        // `class=java/lang/ref/WeakReference method=<init>
        //  sig=(Ljava/lang/Object;)Ljava/lang/ref/WeakReference;`).
        // jnivm rewrites `<init>` into a static returning the class, so
        // it is hooked in that shape, a Class* first parameter.
        weak_reference->Hook(
            env, "<init>",
            [](jnivm::ENV*, jnivm::Class*,
               std::shared_ptr<jnivm::Object> referent) -> std::shared_ptr<jnivm::Weak> {
                auto weak = std::make_shared<jnivm::Weak>();
                weak->wrapped = referent;
                return weak;
            });
    }
}

BEGIN_NATIVE_DESCRIPTOR(SharedPreferencesStub)
{ FakeJni::Function<&SharedPreferencesStub::contains>{}, "contains" },
{ FakeJni::Function<&SharedPreferencesStub::getString>{}, "getString" },
{ FakeJni::Function<&SharedPreferencesStub::edit>{}, "edit" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SharedPreferencesEditorStub)
{ FakeJni::Function<&SharedPreferencesEditorStub::putString>{}, "putString" },
{ FakeJni::Function<&SharedPreferencesEditorStub::remove>{}, "remove" },
{ FakeJni::Function<&SharedPreferencesEditorStub::apply>{}, "apply" },
{ FakeJni::Function<&SharedPreferencesEditorStub::commit>{}, "commit" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DisplayMetricsStub)
{ FakeJni::Field<&DisplayMetricsStub::widthPixels>{}, "widthPixels" },
{ FakeJni::Field<&DisplayMetricsStub::heightPixels>{}, "heightPixels" },
{ FakeJni::Field<&DisplayMetricsStub::density>{}, "density" },
{ FakeJni::Field<&DisplayMetricsStub::densityDpi>{}, "densityDpi" },
{ FakeJni::Field<&DisplayMetricsStub::scaledDensity>{}, "scaledDensity" },
{ FakeJni::Field<&DisplayMetricsStub::xdpi>{}, "xdpi" },
{ FakeJni::Field<&DisplayMetricsStub::ydpi>{}, "ydpi" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MotionEventStub)
{ FakeJni::Constructor<MotionEventStub>{} },
{ FakeJni::Function<&MotionEventStub::getAction>{}, "getAction" },
{ FakeJni::Function<&MotionEventStub::getSource>{}, "getSource" },
{ FakeJni::Function<&MotionEventStub::getDeviceId>{}, "getDeviceId" },
{ FakeJni::Function<&MotionEventStub::getToolType>{}, "getToolType" },
{ FakeJni::Function<&MotionEventStub::getButtonState>{}, "getButtonState" },
{ FakeJni::Function<&MotionEventStub::getActionButton>{}, "getActionButton" },
{ FakeJni::Function<&MotionEventStub::getMetaState>{}, "getMetaState" },
{ FakeJni::Function<&MotionEventStub::getFlags>{}, "getFlags" },
{ FakeJni::Function<&MotionEventStub::getEdgeFlags>{}, "getEdgeFlags" },
{ FakeJni::Function<&MotionEventStub::getPointerCount>{}, "getPointerCount" },
{ FakeJni::Function<&MotionEventStub::getPointerId>{}, "getPointerId" },
{ FakeJni::Function<&MotionEventStub::getHistorySize>{}, "getHistorySize" },
{ FakeJni::Function<&MotionEventStub::getClassification>{}, "getClassification" },
{ FakeJni::Function<&MotionEventStub::getEventTime>{}, "getEventTime" },
{ FakeJni::Function<&MotionEventStub::getDownTime>{}, "getDownTime" },
{ FakeJni::Function<&MotionEventStub::getHistoricalEventTime>{}, "getHistoricalEventTime" },
{ FakeJni::Function<&MotionEventStub::getXPrecision>{}, "getXPrecision" },
{ FakeJni::Function<&MotionEventStub::getYPrecision>{}, "getYPrecision" },
{ FakeJni::Function<&MotionEventStub::getX>{}, "getX" },
{ FakeJni::Function<&MotionEventStub::getY>{}, "getY" },
{ FakeJni::Function<&MotionEventStub::getAxisValue>{}, "getAxisValue" },
{ FakeJni::Function<&MotionEventStub::getHistoricalAxisValue>{}, "getHistoricalAxisValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(KeyEventStub)
{ FakeJni::Constructor<KeyEventStub>{} },
{ FakeJni::Function<&KeyEventStub::getAction>{}, "getAction" },
{ FakeJni::Function<&KeyEventStub::getKeyCode>{}, "getKeyCode" },
{ FakeJni::Function<&KeyEventStub::getScanCode>{}, "getScanCode" },
{ FakeJni::Function<&KeyEventStub::getMetaState>{}, "getMetaState" },
{ FakeJni::Function<&KeyEventStub::getRepeatCount>{}, "getRepeatCount" },
{ FakeJni::Function<&KeyEventStub::getSource>{}, "getSource" },
// The engine asks a KeyEvent what character it produces (live-caught
// GetMethodID MISS for `getUnicodeChar ()I`). Both real overloads exist on
// the class; only the no-argument one was ever registered nowhere, so the
// lookup failed and the key produced no character.
{ FakeJni::Function<static_cast<FakeJni::JInt (KeyEventStub::*)()>(&KeyEventStub::getUnicodeChar)>{},
  "getUnicodeChar" },
{ FakeJni::Function<static_cast<FakeJni::JInt (KeyEventStub::*)(FakeJni::JInt)>(
      &KeyEventStub::getUnicodeChar)>{},
  "getUnicodeChar" },
{ FakeJni::Function<&KeyEventStub::getDeviceId>{}, "getDeviceId" },
{ FakeJni::Function<&KeyEventStub::getFlags>{}, "getFlags" },
{ FakeJni::Function<&KeyEventStub::getEventTime>{}, "getEventTime" },
{ FakeJni::Function<&KeyEventStub::getDownTime>{}, "getDownTime" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ResourcesStub)
{ FakeJni::Function<&ResourcesStub::getDisplayMetrics>{}, "getDisplayMetrics" },
END_NATIVE_DESCRIPTOR

namespace {
int g_display_width = 800;
int g_display_height = 600;
float g_display_density = 1.0f;
int g_display_phys_mm_w = 0;
int g_display_phys_mm_h = 0;
int g_display_output_px_w = 0;
int g_display_output_px_h = 0;
}  // namespace

// Real total system memory in MB, the quantity the app's device-info helper reads from
// ActivityManager on a device. Stud hardcoded 16384 in several places;
// this reports the machine it is actually running on.
namespace {

// One line of a sysfs/DMI file, trimmed. Empty when unreadable, which is
// the normal case in a container or on hardware that reports nothing.
std::string read_sysfs_line(const char* path) {
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return {};
    while (!line.empty() && (line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

}  // namespace

// The real machine's vendor and model, from DMI.
//
// android.os.Build.MANUFACTURER/MODEL stay "Stud"; that is the honest
// answer for the Android layer, which genuinely is not running on a
// phone. But the User-Agent's device field describes the HARDWARE, and
// Roblox uses it for real device analytics, so reporting the actual
// laptop is both more truthful and more useful than a placeholder.
// Falls back to the Build values when DMI says nothing.
std::string real_hardware_name() {
    static const std::string cached = [] {
        std::string vendor = read_sysfs_line("/sys/devices/virtual/dmi/id/sys_vendor");
        std::string product = read_sysfs_line("/sys/devices/virtual/dmi/id/product_name");
        // "ASUSTeK COMPUTER INC." is how DMI spells a name humans write
        // as "ASUS", and the product string usually already carries the
        // brand, so prefer the product alone when it does.
        if (product.empty()) return std::string();
        if (vendor.empty()) return product;
        // DMI vendor strings carry legal suffixes a device name never
        // would ("ASUSTeK COMPUTER INC."), so compare on the first word
        // and, when the product already leads with the brand, use the
        // product alone, which is what the app's device-info helper does on a device.
        std::string brand = vendor.substr(0, vendor.find(' '));
        // "ASUSTeK" vs the product's "ASUS": match on the shorter of the
        // two prefixes so a stylised vendor spelling still counts.
        const size_t compare_len = std::min<size_t>(4, std::min(brand.size(), product.size()));
        if (compare_len > 0 &&
            strncasecmp(brand.c_str(), product.c_str(), compare_len) == 0) {
            return product;
        }
        return brand + " " + product;
    }();
    return cached;
}

int real_total_memory_mb() {
    static const int cached = [] {
        const long pages = ::sysconf(_SC_PHYS_PAGES);
        const long page_size = ::sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page_size > 0) {
            return static_cast<int>((static_cast<long long>(pages) * page_size) / (1024 * 1024));
        }
        // No invented number: sysconf failing is a real condition, and
        // reporting a plausible-looking 16GB would be indistinguishable
        // from a real measurement in every log and every request.
        return 0;
    }();
    return cached;
}

// MANUFACTURER + " " + MODEL, matching the model is not
// repeated when it already starts with the manufacturer, and the first
// letter is upper-cased. Both come from android.os.Build, which Stud
// fills in from the real machine, the User-Agent used to send the
// literal string "Stud", which describes no device at all.
std::string real_device_name() {
    // Prefer the real hardware; fall back to the Build values.
    const std::string hardware = real_hardware_name();
    std::string manufacturer = BuildStub::MANUFACTURER->asStdString();
    std::string model = BuildStub::MODEL->asStdString();
    if (!hardware.empty()) {
        manufacturer.clear();
        model = hardware;
    }
    std::string name;
    if (model.rfind(manufacturer, 0) == 0) {
        name = model;
    } else if (manufacturer.empty()) {
        name = model;
    } else {
        name = manufacturer + " " + model;
    }
    if (!name.empty()) {
        name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
    }
    // Control characters are replaced with '_' by the real builder's
    // own code, since they would corrupt the header.
    for (char& c : name) {
        if (static_cast<unsigned char>(c) <= 31 || static_cast<unsigned char>(c) >= 127) c = '_';
    }
    return name;
}

// Read back what the compositor and window actually reported, so
// anything that must describe this display (the User-Agent, chiefly)
// uses the real numbers rather than a second set of guesses.
float g_measured_density = 0.0f;

void set_measured_display_density(float density) { g_measured_density = density; }

float engine_layout_density() { return g_display_density; }

DisplayFacts real_display_facts() {
    DisplayFacts f;
    f.width_px = g_display_width;
    f.height_px = g_display_height;
    f.density = g_display_density;
    // Real xdpi/ydpi when the compositor gave physical millimetres;
    // otherwise Android's own baseline of 160dpi scaled by density,
    // which is what DisplayMetrics itself falls back to.
    if (g_display_output_px_w > 0 && g_display_phys_mm_w > 0) {
        f.xdpi = static_cast<float>(g_display_output_px_w) * 25.4f /
                 static_cast<float>(g_display_phys_mm_w);
    } else {
        f.xdpi = 160.0f * g_display_density;
    }
    if (g_display_output_px_h > 0 && g_display_phys_mm_h > 0) {
        f.ydpi = static_cast<float>(g_display_output_px_h) * 25.4f /
                 static_cast<float>(g_display_phys_mm_h);
    } else {
        f.ydpi = 160.0f * g_display_density;
    }
    f.output_width_px = g_display_output_px_w;
    f.output_height_px = g_display_output_px_h;
    f.measured_density = g_measured_density;
    return f;
}

void set_real_display_output_geometry(int px_w, int px_h, int mm_w, int mm_h) {
    g_display_output_px_w = px_w;
    g_display_output_px_h = px_h;
    g_display_phys_mm_w = mm_w;
    g_display_phys_mm_h = mm_h;
    const bool usable = px_w > 0 && px_h > 0 && mm_w > 0 && mm_h > 0;
    std::printf("stud: real display output: %dpx x %dpx, %dmm x %dmm%s\n", px_w, px_h, mm_w, mm_h,
                usable ? "" : " (incomplete, density-derived dpi used)");
    std::fflush(stdout);
}

void set_real_display_metrics(int width_px, int height_px, float density) {
    g_display_width = width_px;
    g_display_height = height_px;
    g_display_density = density;
    std::printf("stud: DisplayMetrics seeded: %dx%d density=%.2f\n", width_px, height_px,
                static_cast<double>(density));
    std::fflush(stdout);
}

std::shared_ptr<DisplayMetricsStub> ResourcesStub::getDisplayMetrics() {
    auto metrics = std::make_shared<DisplayMetricsStub>();
    metrics->widthPixels = g_display_width;
    metrics->heightPixels = g_display_height;
    metrics->density = g_display_density;
    metrics->densityDpi = static_cast<FakeJni::JInt>(160.0f * g_display_density);
    metrics->scaledDensity = g_display_density;
    // Real dots-per-inch, from the display's own physical size when the
    // compositor reported one. Note this is the whole OUTPUT's dpi, not
    // the window's, which is exactly right: dpi is a property of the
    // panel, and a window occupying part of it has the same pixel pitch.
    // Falls back to the old density-derived synthesis only when the
    // compositor reported no physical size at all.
    if (g_display_phys_mm_w > 0 && g_display_phys_mm_h > 0 && g_display_output_px_w > 0 &&
        g_display_output_px_h > 0) {
        metrics->xdpi =
            static_cast<float>(g_display_output_px_w) / (static_cast<float>(g_display_phys_mm_w) / 25.4f);
        metrics->ydpi =
            static_cast<float>(g_display_output_px_h) / (static_cast<float>(g_display_phys_mm_h) / 25.4f);
    } else {
        metrics->xdpi = 160.0f * g_display_density;
        metrics->ydpi = 160.0f * g_display_density;
    }
    return metrics;
}

std::shared_ptr<SharedPreferencesStub> SharedPreferencesStub::get_or_create(const std::string& name) {
    static std::unordered_map<std::string, std::shared_ptr<SharedPreferencesStub>> instances;
    auto it = instances.find(name);
    if (it != instances.end()) return it->second;
    auto created = std::make_shared<SharedPreferencesStub>(name);
    instances.emplace(name, created);
    return created;
}

std::shared_ptr<SharedPreferencesEditorStub> SharedPreferencesStub::edit() {
    return std::make_shared<SharedPreferencesEditorStub>(
        std::static_pointer_cast<SharedPreferencesStub>(shared_from_this()));
}

void register_android_framework_stubs(FakeJni::Jvm& jvm) {
    jvm.registerClass<LooperStub>();
    jvm.registerClass<HandlerStub>();
    jvm.registerClass<HandlerThreadStub>();
    jvm.registerClass<ClassLoaderStub>();
    register_java_lang_class_methods(jvm);
    register_java_lang_string_methods(jvm);
    jvm.registerClass<BuildStub>();
    jvm.registerClass<BuildVersionStub>();
    jvm.registerClass<DebugStub>();
    jvm.registerClass<PointStub>();
    jvm.registerClass<DeviceUtilsStub>();
    jvm.registerClass<DisplayMetricsStub>();
    jvm.registerClass<MotionEventStub>();
    jvm.registerClass<KeyEventStub>();
    jvm.registerClass<JavaLangSystemStub>();
    jvm.registerClass<JavaLangErrorStub>();
    jvm.registerClass<JavaLangExceptionStub>();
    jvm.registerClass<JavaLangRuntimeExceptionStub>();
    jvm.registerClass<JavaLangLongStub>();
    jvm.registerClass<JavaLangIntegerStub>();
    jvm.registerClass<JavaLangBooleanStub>();
    jvm.registerClass<JavaLangDoubleStub>();
    jvm.registerClass<JavaUtilIteratorStub>();
    jvm.registerClass<JavaUtilHashSetStub>();
    jvm.registerClass<JavaUtilHashMapStub>();
    jvm.registerClass<SharedPreferencesStub>();
    jvm.registerClass<SharedPreferencesEditorStub>();
    jvm.registerClass<ResourcesStub>();
}

}  // namespace stud::jni_bridge
