#include "stud/android_framework.h"

#include <unistd.h>
#include <strings.h>

#include <algorithm>
#include <fstream>
#include <cctype>

#include "stud/bionic_jvm.h"
// For ContextJava: DeviceUtilsJava::getScreenPhysicalSizeInMillimeters takes a
// real android.content.Context, and FakeJni computes the JNI signature from
// the C++ type, so the complete type is needed here (the header can only
// forward-declare it, app_java_classes.h includes that header).
#include "stud/app_java_classes.h"

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

BEGIN_NATIVE_DESCRIPTOR(LooperJava)
{ FakeJni::Function<&LooperJava::getMainLooper>{}, "getMainLooper" },
{ FakeJni::Function<&LooperJava::myLooper>{}, "myLooper" },
{ FakeJni::Function<&LooperJava::prepare>{}, "prepare" },
{ FakeJni::Function<&LooperJava::loop>{}, "loop" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(HandlerJava)
{ FakeJni::Constructor<HandlerJava>{} },
{ FakeJni::Constructor<HandlerJava, std::shared_ptr<LooperJava>>{} },
{ FakeJni::Function<&HandlerJava::post>{}, "post" },
{ FakeJni::Function<&HandlerJava::postDelayed>{}, "postDelayed" },
{ FakeJni::Function<&HandlerJava::getLooper>{}, "getLooper" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(HandlerThreadJava)
{ FakeJni::Constructor<HandlerThreadJava>{} },
{ FakeJni::Constructor<HandlerThreadJava, std::shared_ptr<FakeJni::JString>>{} },
{ FakeJni::Function<&HandlerThreadJava::start>{}, "start" },
{ FakeJni::Function<&HandlerThreadJava::getLooper>{}, "getLooper" },
{ FakeJni::Function<&HandlerThreadJava::quit>{}, "quit" },
{ FakeJni::Function<&HandlerThreadJava::quitSafely>{}, "quitSafely" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ClassLoaderJava)
{ FakeJni::Function<&ClassLoaderJava::loadClass>{}, "loadClass" },
{ FakeJni::Function<&ClassLoaderJava::findClass>{}, "findClass" },
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
BEGIN_NATIVE_DESCRIPTOR(BuildJava)
{ FakeJni::Field<&BuildJava::MANUFACTURER>{}, "MANUFACTURER", kStaticPublicField },
{ FakeJni::Field<&BuildJava::BRAND>{}, "BRAND", kStaticPublicField },
{ FakeJni::Field<&BuildJava::MODEL>{}, "MODEL", kStaticPublicField },
{ FakeJni::Field<&BuildJava::DEVICE>{}, "DEVICE", kStaticPublicField },
{ FakeJni::Field<&BuildJava::PRODUCT>{}, "PRODUCT", kStaticPublicField },
{ FakeJni::Field<&BuildJava::HARDWARE>{}, "HARDWARE", kStaticPublicField },
{ FakeJni::Field<&BuildJava::BOARD>{}, "BOARD", kStaticPublicField },
{ FakeJni::Field<&BuildJava::FINGERPRINT>{}, "FINGERPRINT", kStaticPublicField },
{ FakeJni::Field<&BuildJava::ID>{}, "ID", kStaticPublicField },
{ FakeJni::Field<&BuildJava::TAGS>{}, "TAGS", kStaticPublicField },
{ FakeJni::Field<&BuildJava::TYPE>{}, "TYPE", kStaticPublicField },
{ FakeJni::Field<&BuildJava::BOOTLOADER>{}, "BOOTLOADER", kStaticPublicField },
{ FakeJni::Field<&BuildJava::USER>{}, "USER", kStaticPublicField },
END_NATIVE_DESCRIPTOR

// android.os.Build$VERSION's fields are `public static final` too, the
// same fix as BuildJava above, which was applied there and missed here.
// Live-caught during a game launch as `GetFieldID MISS
// class=android/os/Build$VERSION static field=SDK_INT sig=I`, repeatedly.
BEGIN_NATIVE_DESCRIPTOR(BuildVersionJava)
{ FakeJni::Field<&BuildVersionJava::SDK_INT>{}, "SDK_INT", kStaticPublicField },
{ FakeJni::Field<&BuildVersionJava::RELEASE>{}, "RELEASE", kStaticPublicField },
{ FakeJni::Field<&BuildVersionJava::SECURITY_PATCH>{}, "SECURITY_PATCH", kStaticPublicField },
{ FakeJni::Field<&BuildVersionJava::INCREMENTAL>{}, "INCREMENTAL", kStaticPublicField },
{ FakeJni::Field<&BuildVersionJava::CODENAME>{}, "CODENAME", kStaticPublicField },
END_NATIVE_DESCRIPTOR

// Also `public static`, same live-caught miss during a game launch.
BEGIN_NATIVE_DESCRIPTOR(DebugJava)
{ FakeJni::Function<&DebugJava::isDebuggerConnected>{}, "isDebuggerConnected", kStaticPublicMethod },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(PointJava)
{ FakeJni::Constructor<PointJava>{} },
{ FakeJni::Constructor<PointJava, FakeJni::JInt, FakeJni::JInt>{} },
{ FakeJni::Field<&PointJava::x>{}, "x" },
{ FakeJni::Field<&PointJava::y>{}, "y" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DeviceUtilsJava)
{ FakeJni::Function<&DeviceUtilsJava::getScreenPhysicalSizeInMillimeters>{},
  "getScreenPhysicalSizeInMillimeters", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
END_NATIVE_DESCRIPTOR

std::shared_ptr<PointJava> DeviceUtilsJava::getScreenPhysicalSizeInMillimeters(
    std::shared_ptr<ContextJava> /*context*/) {
    auto metrics = ResourcesJava().getDisplayMetrics();
    auto to_mm = [](FakeJni::JInt pixels, FakeJni::JFloat dpi) -> FakeJni::JInt {
        if (dpi <= 0.0f) return 0;
        return static_cast<FakeJni::JInt>((static_cast<float>(pixels) / dpi) * 25.4f);
    };
    auto point = std::make_shared<PointJava>(to_mm(metrics->widthPixels, metrics->xdpi),
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

BEGIN_NATIVE_DESCRIPTOR(JavaLangSystemJava)
{ FakeJni::Function<&JavaLangSystemJava::identityHashCode>{}, "identityHashCode", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
{ FakeJni::Function<&JavaLangSystemJava::currentTimeMillis>{}, "currentTimeMillis", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
{ FakeJni::Function<&JavaLangSystemJava::nanoTime>{}, "nanoTime", FakeJni::JMethodID::PUBLIC | FakeJni::JMethodID::STATIC },
END_NATIVE_DESCRIPTOR

FakeJni::JInt JavaLangSystemJava::identityHashCode(std::shared_ptr<FakeJni::JObject> obj) {
    auto value = reinterpret_cast<std::uintptr_t>(obj.get());
    return static_cast<FakeJni::JInt>((value >> 4) & 0x7fffffff);
}

FakeJni::JLong JavaLangSystemJava::currentTimeMillis() {
    return static_cast<FakeJni::JLong>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

FakeJni::JLong JavaLangSystemJava::nanoTime() {
    return static_cast<FakeJni::JLong>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

BEGIN_NATIVE_DESCRIPTOR(JavaLangErrorJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangExceptionJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangRuntimeExceptionJava)
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangLongJava)
{ FakeJni::Constructor<JavaLangLongJava>{} },
{ FakeJni::Constructor<JavaLangLongJava, FakeJni::JLong>{} },
{ FakeJni::Function<&JavaLangLongJava::longValue>{}, "longValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangIntegerJava)
{ FakeJni::Constructor<JavaLangIntegerJava>{} },
{ FakeJni::Constructor<JavaLangIntegerJava, FakeJni::JInt>{} },
{ FakeJni::Function<&JavaLangIntegerJava::intValue>{}, "intValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangBooleanJava)
{ FakeJni::Constructor<JavaLangBooleanJava>{} },
{ FakeJni::Constructor<JavaLangBooleanJava, FakeJni::JBoolean>{} },
{ FakeJni::Function<&JavaLangBooleanJava::booleanValue>{}, "booleanValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaLangDoubleJava)
{ FakeJni::Constructor<JavaLangDoubleJava>{} },
{ FakeJni::Constructor<JavaLangDoubleJava, FakeJni::JDouble>{} },
{ FakeJni::Function<&JavaLangDoubleJava::doubleValue>{}, "doubleValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaUtilIteratorJava)
{ FakeJni::Function<&JavaUtilIteratorJava::hasNext>{}, "hasNext" },
{ FakeJni::Function<&JavaUtilIteratorJava::next>{}, "next" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaUtilHashSetJava)
{ FakeJni::Constructor<JavaUtilHashSetJava>{} },
{ FakeJni::Function<&JavaUtilHashSetJava::add>{}, "add" },
{ FakeJni::Function<&JavaUtilHashSetJava::contains>{}, "contains" },
{ FakeJni::Function<&JavaUtilHashSetJava::size>{}, "size" },
{ FakeJni::Function<&JavaUtilHashSetJava::isEmpty>{}, "isEmpty" },
{ FakeJni::Function<&JavaUtilHashSetJava::iterator>{}, "iterator" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(JavaUtilHashMapJava)
{ FakeJni::Constructor<JavaUtilHashMapJava>{} },
{ FakeJni::Function<&JavaUtilHashMapJava::put>{}, "put" },
{ FakeJni::Function<&JavaUtilHashMapJava::get>{}, "get" },
{ FakeJni::Function<&JavaUtilHashMapJava::containsKey>{}, "containsKey" },
{ FakeJni::Function<&JavaUtilHashMapJava::size>{}, "size" },
{ FakeJni::Function<&JavaUtilHashMapJava::isEmpty>{}, "isEmpty" },
END_NATIVE_DESCRIPTOR

void LooperJava::start(FakeJni::Jvm& jvm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable() || quit_) return;
    thread_ = std::thread([this, &jvm] { pump(jvm); });
}

void LooperJava::post(std::shared_ptr<FakeJni::JObject> runnable) {
    if (!runnable) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(runnable));
    }
    cv_.notify_one();
}

void LooperJava::quit() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
}

void LooperJava::stop() {
    quit();
    if (thread_.joinable()) thread_.join();
}

void LooperJava::dispatch_one(FakeJni::Jvm& jvm, std::shared_ptr<FakeJni::JObject> runnable) {
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

void LooperJava::pump(FakeJni::Jvm& jvm) {
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

void LooperJava::drain_pending(FakeJni::Jvm& jvm) {
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

std::shared_ptr<LooperJava> LooperJava::get_or_create_main_looper(FakeJni::Jvm& jvm) {
    static std::mutex init_mutex;
    static std::shared_ptr<LooperJava> instance;
    std::lock_guard<std::mutex> lock(init_mutex);
    if (!instance) {
        // Real Android semantics: the main Looper runs ON the real
        // process main thread, never a spawned worker (see this
        // class's own header doc comment), deliberately does NOT
        // call start() here. The real main thread drains it directly
        // via drain_pending() (see activity_thread.h).
        instance = std::make_shared<LooperJava>("main");
    }
    return instance;
}

std::shared_ptr<LooperJava> LooperJava::getMainLooper() {
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    return get_or_create_main_looper(env.getVM());
}

std::shared_ptr<LooperJava> LooperJava::myLooper() {
    // Real semantics: null unless the calling thread has its own
    // prepared Looper. No real caller of this codebase's own bring-up
    // was found relying on that distinction (see class-level comment on
    // prepare()/loop()), returning the main looper is a safe,
    // documented simplification, not a silent lie about a case that
    // matters here.
    return getMainLooper();
}

HandlerJava::HandlerJava() {
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    looper_ = LooperJava::get_or_create_main_looper(env.getVM());
}

HandlerJava::HandlerJava(std::shared_ptr<LooperJava> looper) : looper_(std::move(looper)) {
    if (!looper_) {
        auto& env = FakeJni::JniEnvContext().getJniEnv();
        looper_ = LooperJava::get_or_create_main_looper(env.getVM());
    }
}

FakeJni::JBoolean HandlerJava::post(std::shared_ptr<FakeJni::JObject> runnable) {
    if (!runnable || !looper_) return false;
    looper_->post(std::move(runnable));
    return true;
}

FakeJni::JBoolean HandlerJava::postDelayed(std::shared_ptr<FakeJni::JObject> runnable,
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

HandlerThreadJava::HandlerThreadJava() : looper_(std::make_shared<LooperJava>("HandlerThread")) {}

HandlerThreadJava::HandlerThreadJava(std::shared_ptr<FakeJni::JString> name)
    : looper_(std::make_shared<LooperJava>(name ? static_cast<std::string>(*name) : "HandlerThread")) {}

void HandlerThreadJava::start() {
    auto& env = FakeJni::JniEnvContext().getJniEnv();
    looper_->start(env.getVM());
}

std::shared_ptr<FakeJni::JClass> ClassLoaderJava::loadClass(std::shared_ptr<FakeJni::JString> name) {
    return findClass(std::move(name));
}

std::shared_ptr<FakeJni::JClass> ClassLoaderJava::findClass(std::shared_ptr<FakeJni::JString> name) {
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

std::shared_ptr<ClassLoaderJava> shared_class_loader() {
    static auto singleton = std::make_shared<ClassLoaderJava>();
    return singleton;
}

// Real java.lang.String methods the engine actually calls, attached to
// jnivm's own canonical String class rather than a competing class of the
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
    // instead of declaring a competing class for the same name.
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
    // (PlatformSystemDialogHandlerJava, FacialAgeEstimationProtocolJava), so
    // the path has somewhere to land. If this ever regresses again, the symptom
    // to look for is the app stalling at RootSwitchNavigator instead of Home.
    java_lang_class->HookInstanceFunction(
        env, "getClassLoader",
        [](jnivm::ENV*, jnivm::Object*) -> std::shared_ptr<ClassLoaderJava> {
            return shared_class_loader();
        });

    // java.lang.ref.WeakReference.get(), jnivm has its own real
    // `jnivm::Weak` registered under this exact class name (so a
    // competing class would collide, per the ByteBuffer/Class lesson),
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

BEGIN_NATIVE_DESCRIPTOR(SharedPreferencesJava)
{ FakeJni::Function<&SharedPreferencesJava::contains>{}, "contains" },
{ FakeJni::Function<&SharedPreferencesJava::getString>{}, "getString" },
{ FakeJni::Function<&SharedPreferencesJava::edit>{}, "edit" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(SharedPreferencesEditorJava)
{ FakeJni::Function<&SharedPreferencesEditorJava::putString>{}, "putString" },
{ FakeJni::Function<&SharedPreferencesEditorJava::remove>{}, "remove" },
{ FakeJni::Function<&SharedPreferencesEditorJava::apply>{}, "apply" },
{ FakeJni::Function<&SharedPreferencesEditorJava::commit>{}, "commit" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(DisplayMetricsJava)
{ FakeJni::Field<&DisplayMetricsJava::widthPixels>{}, "widthPixels" },
{ FakeJni::Field<&DisplayMetricsJava::heightPixels>{}, "heightPixels" },
{ FakeJni::Field<&DisplayMetricsJava::density>{}, "density" },
{ FakeJni::Field<&DisplayMetricsJava::densityDpi>{}, "densityDpi" },
{ FakeJni::Field<&DisplayMetricsJava::scaledDensity>{}, "scaledDensity" },
{ FakeJni::Field<&DisplayMetricsJava::xdpi>{}, "xdpi" },
{ FakeJni::Field<&DisplayMetricsJava::ydpi>{}, "ydpi" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(MotionEventJava)
{ FakeJni::Constructor<MotionEventJava>{} },
{ FakeJni::Function<&MotionEventJava::getAction>{}, "getAction" },
{ FakeJni::Function<&MotionEventJava::getSource>{}, "getSource" },
{ FakeJni::Function<&MotionEventJava::getDeviceId>{}, "getDeviceId" },
{ FakeJni::Function<&MotionEventJava::getToolType>{}, "getToolType" },
{ FakeJni::Function<&MotionEventJava::getButtonState>{}, "getButtonState" },
{ FakeJni::Function<&MotionEventJava::getActionButton>{}, "getActionButton" },
{ FakeJni::Function<&MotionEventJava::getMetaState>{}, "getMetaState" },
{ FakeJni::Function<&MotionEventJava::getFlags>{}, "getFlags" },
{ FakeJni::Function<&MotionEventJava::getEdgeFlags>{}, "getEdgeFlags" },
{ FakeJni::Function<&MotionEventJava::getPointerCount>{}, "getPointerCount" },
{ FakeJni::Function<&MotionEventJava::getPointerId>{}, "getPointerId" },
{ FakeJni::Function<&MotionEventJava::getHistorySize>{}, "getHistorySize" },
{ FakeJni::Function<&MotionEventJava::getClassification>{}, "getClassification" },
{ FakeJni::Function<&MotionEventJava::getEventTime>{}, "getEventTime" },
{ FakeJni::Function<&MotionEventJava::getDownTime>{}, "getDownTime" },
{ FakeJni::Function<&MotionEventJava::getHistoricalEventTime>{}, "getHistoricalEventTime" },
{ FakeJni::Function<&MotionEventJava::getXPrecision>{}, "getXPrecision" },
{ FakeJni::Function<&MotionEventJava::getYPrecision>{}, "getYPrecision" },
{ FakeJni::Function<&MotionEventJava::getX>{}, "getX" },
{ FakeJni::Function<&MotionEventJava::getY>{}, "getY" },
{ FakeJni::Function<&MotionEventJava::getAxisValue>{}, "getAxisValue" },
{ FakeJni::Function<&MotionEventJava::getHistoricalAxisValue>{}, "getHistoricalAxisValue" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(KeyEventJava)
{ FakeJni::Constructor<KeyEventJava>{} },
{ FakeJni::Function<&KeyEventJava::getAction>{}, "getAction" },
{ FakeJni::Function<&KeyEventJava::getKeyCode>{}, "getKeyCode" },
{ FakeJni::Function<&KeyEventJava::getScanCode>{}, "getScanCode" },
{ FakeJni::Function<&KeyEventJava::getMetaState>{}, "getMetaState" },
{ FakeJni::Function<&KeyEventJava::getRepeatCount>{}, "getRepeatCount" },
{ FakeJni::Function<&KeyEventJava::getSource>{}, "getSource" },
// The engine asks a KeyEvent what character it produces (live-caught
// GetMethodID MISS for `getUnicodeChar ()I`). Both real overloads exist on
// the class; only the no-argument one was ever registered nowhere, so the
// lookup failed and the key produced no character.
{ FakeJni::Function<static_cast<FakeJni::JInt (KeyEventJava::*)()>(&KeyEventJava::getUnicodeChar)>{},
  "getUnicodeChar" },
{ FakeJni::Function<static_cast<FakeJni::JInt (KeyEventJava::*)(FakeJni::JInt)>(
      &KeyEventJava::getUnicodeChar)>{},
  "getUnicodeChar" },
{ FakeJni::Function<&KeyEventJava::getDeviceId>{}, "getDeviceId" },
{ FakeJni::Function<&KeyEventJava::getFlags>{}, "getFlags" },
{ FakeJni::Function<&KeyEventJava::getEventTime>{}, "getEventTime" },
{ FakeJni::Function<&KeyEventJava::getDownTime>{}, "getDownTime" },
END_NATIVE_DESCRIPTOR

BEGIN_NATIVE_DESCRIPTOR(ResourcesJava)
{ FakeJni::Function<&ResourcesJava::getDisplayMetrics>{}, "getDisplayMetrics" },
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

// MANUFACTURER + " " + MODEL, matching the app's device-info helper: the model is not
// repeated when it already starts with the manufacturer, and the first
// letter is upper-cased. Both come from android.os.Build, which Stud
// fills in from the real machine, the User-Agent used to send the
// literal string "Stud", which describes no device at all.
std::string real_device_name() {
    // Prefer the real hardware; fall back to the Build values.
    const std::string hardware = real_hardware_name();
    std::string manufacturer = BuildJava::MANUFACTURER->asStdString();
    std::string model = BuildJava::MODEL->asStdString();
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

}  // namespace stud::jni_bridge

// For libandroid's AConfiguration, which lives in another library and
// reaches this by name: the same density DisplayMetrics reports, so a
// configuration's dp sizes agree with the engine's own layout.
extern "C" float stud_engine_layout_density() { return stud::jni_bridge::engine_layout_density(); }

namespace stud::jni_bridge {

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

std::shared_ptr<DisplayMetricsJava> ResourcesJava::getDisplayMetrics() {
    auto metrics = std::make_shared<DisplayMetricsJava>();
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

std::shared_ptr<SharedPreferencesJava> SharedPreferencesJava::get_or_create(const std::string& name) {
    static std::unordered_map<std::string, std::shared_ptr<SharedPreferencesJava>> instances;
    auto it = instances.find(name);
    if (it != instances.end()) return it->second;
    auto created = std::make_shared<SharedPreferencesJava>(name);
    instances.emplace(name, created);
    return created;
}

std::shared_ptr<SharedPreferencesEditorJava> SharedPreferencesJava::edit() {
    return std::make_shared<SharedPreferencesEditorJava>(
        std::static_pointer_cast<SharedPreferencesJava>(shared_from_this()));
}

void register_android_framework(FakeJni::Jvm& jvm) {
    jvm.registerClass<LooperJava>();
    jvm.registerClass<HandlerJava>();
    jvm.registerClass<HandlerThreadJava>();
    jvm.registerClass<ClassLoaderJava>();
    register_java_lang_class_methods(jvm);
    register_java_lang_string_methods(jvm);
    jvm.registerClass<BuildJava>();
    jvm.registerClass<BuildVersionJava>();
    jvm.registerClass<DebugJava>();
    jvm.registerClass<PointJava>();
    jvm.registerClass<DeviceUtilsJava>();
    jvm.registerClass<DisplayMetricsJava>();
    jvm.registerClass<MotionEventJava>();
    jvm.registerClass<KeyEventJava>();
    jvm.registerClass<JavaLangSystemJava>();
    jvm.registerClass<JavaLangErrorJava>();
    jvm.registerClass<JavaLangExceptionJava>();
    jvm.registerClass<JavaLangRuntimeExceptionJava>();
    jvm.registerClass<JavaLangLongJava>();
    jvm.registerClass<JavaLangIntegerJava>();
    jvm.registerClass<JavaLangBooleanJava>();
    jvm.registerClass<JavaLangDoubleJava>();
    jvm.registerClass<JavaUtilIteratorJava>();
    jvm.registerClass<JavaUtilHashSetJava>();
    jvm.registerClass<JavaUtilHashMapJava>();
    jvm.registerClass<SharedPreferencesJava>();
    jvm.registerClass<SharedPreferencesEditorJava>();
    jvm.registerClass<ResourcesJava>();
}

}  // namespace stud::jni_bridge
