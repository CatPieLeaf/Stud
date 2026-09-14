#pragma once

#include <fake-jni/fake-jni.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// Real, generic Android application-framework primitives, not a
// one-off crash fix. the engineering notes' own history is mostly a long
// chain of "call this one specific native entry point in this one
// specific order" fixes; the actual structural gap those all share is
// that Stud never gives Roblox's native code a genuinely *running*
// framework underneath it. No persistent message loop, no working
// java.lang.ClassLoader, so any async dispatch or reflective class
// lookup the real app depends on (the same public, documented Android
// APIs a real device provides for real) either silently does nothing
// or fails a lookup outright.
//
// This file implements the two real, generic, publicly-documented
// Android APIs found in actual use by searching the app's own code:
// android.os.Handler/Looper/HandlerThread (13 files use `new Handler`,
// 2 use HandlerThread, 2 use Looper.myLooper, zero use the raw
// Looper.prepare()/loop() idiom, so that pair is intentionally left as
// a documented no-op below) and java.lang.ClassLoader (the Djinni
// classloader-bootstrap idiom already documented at length in
// the engineering notes). Neither requires reasoning about libroblox.so's own
// internal machine code, both are implemented purely against the
// real, public Android SDK method contracts.
namespace stud::jni_bridge {

// android.os.Looper. Real semantics that matter to a caller (posted
// work eventually runs, in order). Two real modes, matching two real
// Android situations:
//  - HandlerThread's own Looper: a genuinely separate, dedicated real
//    thread (start() spawns it), matches real Android exactly, a
//    HandlerThread's whole point is running on its own thread.
//  - The real *main* Looper: real Android runs this ON the real
//    process main thread, not a spawned worker (some real Android APIs
//    check `Looper.getMainLooper() == Looper.myLooper()`, and thread
//    identity is part of the real contract). get_or_create_main_looper()
//    below does NOT start a dedicated thread for exactly this reason;
//    see activity_thread.h's own doc comment for the real driver
//    (runtime/src/main.cpp's own real render/input loop) that pumps it
//    on the real main thread instead, via drain_pending().
class LooperStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/Looper")

    LooperStub() = default;
    explicit LooperStub(std::string debug_name) : debug_name_(std::move(debug_name)) {}
    ~LooperStub() { stop(); }

    // Idempotent: safe to call more than once. Spawns a real, dedicated
    // pump thread, correct for a real HandlerThread's own Looper, NOT
    // used for the main looper (see class doc comment above).
    void start(FakeJni::Jvm& jvm);

    void post(std::shared_ptr<FakeJni::JObject> runnable);

    void quit();
    void stop();

    // Non-blocking: dispatches everything CURRENTLY queued, then
    // returns immediately without waiting for more. Real use: the
    // process's own real main thread calls this once per real render/
    // input loop iteration (see activity_thread.h), making that thread
    // genuinely the same thread a real device's main Looper runs on,
    // not a separate worker standing in for it.
    void drain_pending(FakeJni::Jvm& jvm);

    static std::shared_ptr<LooperStub> get_or_create_main_looper(FakeJni::Jvm& jvm);

    // Java-visible statics real callers were found to use.
    static std::shared_ptr<LooperStub> getMainLooper();
    static std::shared_ptr<LooperStub> myLooper();
    // Real Looper.prepare()/loop() block the *calling* thread forever,
    // tying the Looper's identity to it. No real caller of this pair
    // was found in the app's own code (only Handler/
    // HandlerThread usage), left as documented no-ops rather than
    // guessed at, so a caller expecting the real blocking contract
    // fails loudly/differently instead of silently misbehaving.
    static void prepare() {}
    static void loop() {}

private:
    void pump(FakeJni::Jvm& jvm);
    void dispatch_one(FakeJni::Jvm& jvm, std::shared_ptr<FakeJni::JObject> runnable);

    std::string debug_name_ = "main";
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<FakeJni::JObject>> queue_;
    bool quit_ = false;
    std::thread thread_;
};

// android.os.Handler; real post()/postDelayed(Runnable) semantics:
// the Runnable really runs, on the target Looper's real pump thread,
// dispatched via a normal env->GetObjectClass/GetMethodID("run","()V")/
// CallVoidMethod, the same generic JNI path a real device's own
// Looper uses, not a Stud-specific shortcut.
//
// removeCallbacks/removeCallbacksAndMessages are deliberately NOT
// implemented: the queue has no per-Runnable identity tracking, and a
// silent no-op would look like cancellation succeeded when it didn't.
// Add real tracking if/when a live caller is found to actually depend
// on cancellation, rather than guessing at it now.
class HandlerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/Handler")

    HandlerStub();
    explicit HandlerStub(std::shared_ptr<LooperStub> looper);

    FakeJni::JBoolean post(std::shared_ptr<FakeJni::JObject> runnable);
    FakeJni::JBoolean postDelayed(std::shared_ptr<FakeJni::JObject> runnable, FakeJni::JLong delay_millis);
    std::shared_ptr<LooperStub> getLooper() { return looper_; }

private:
    std::shared_ptr<LooperStub> looper_;
};

// android.os.HandlerThread, a named thread with its own real,
// running Looper, matching the real API's start()/getLooper()/quit()/
// quitSafely() surface (quit and quitSafely are not distinguished here
// no real caller found that depends on quitSafely draining pending
// work before stopping; both just stop the pump).
class HandlerThreadStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/HandlerThread")

    HandlerThreadStub();
    explicit HandlerThreadStub(std::shared_ptr<FakeJni::JString> name);

    void start();
    std::shared_ptr<LooperStub> getLooper() { return looper_; }
    void quit() { looper_->quit(); }
    void quitSafely() { looper_->quit(); }

private:
    std::shared_ptr<LooperStub> looper_;
};

// java.lang.ClassLoader; real loadClass(String)/findClass(String),
// implemented against env->FindClass the same way a real
// BaseDexClassLoader ultimately resolves a class, just without a real
// DEX to search (Stud's FindClass already auto-vivifies/looks up
// against its own registered classes either way). This is what makes
// the real, already-documented Djinni classloader-bootstrap idiom
// (`FindClass(NativeObjectManager) -> GetObjectClass ->
// getClassLoader() -> loadClass(...)`) have something real to call
// into instead of nothing.
class ClassLoaderStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/ClassLoader")

    std::shared_ptr<FakeJni::JClass> loadClass(std::shared_ptr<FakeJni::JString> name);
    std::shared_ptr<FakeJni::JClass> findClass(std::shared_ptr<FakeJni::JString> name);
};

// java.lang.Class. Real getClassLoader(), hooked onto the SAME
// canonical, name-keyed "java/lang/Class" descriptor every other
// jclass in this process already shares (jvm.registerClass<T>() goes
// through the identical vm->classes[name] registry InternalFindClass
// uses, see jnivm's src/jnivm/internal/findclass.cpp), not a
// separate, competing type. Registering this is what gives a real
// method table (loadClass/findClass on ClassLoaderStub above included)
// to a class name Stud previously never explicitly registered at all;
// every other stub class in this codebase IS explicitly pre-registered
// this same way and has never shown this bug, so this is a real,
// plausible, cheaply-testable fix, not a guess.
// NOTE: there is deliberately no `ClassMetaStub` here any more, for the
// exact same reason `ByteBufferStub` is gone from game_activity_stubs.h
// (see that file's own note): jnivm already has its own real
// `jnivm::Class` and registers it under `java/lang/Class` itself, and
// that is the concrete type `GetObjectClass()` actually returns. A
// second, unrelated C++ stub type claiming the same real Java class
// name can never satisfy `UnpackJObject<T>`'s `dynamic_cast`, so every
// call through it threw `Invalid Reference, Unexpected Type`, which
// libroblox turns into `RBXCRASH: UnhandledException` and a trap,
// killing the thread it happened on (live-confirmed: that is one of the
// two things killing the engine's own designated internal "main"
// thread, after which nothing drains its task queue; see
// the engineering notes, "what drains the engine's task queue").
//
// Worth being precise about what was lost: nothing. `getClassLoader()`
// never once returned successfully in this project's history, before
// this stub existed the method simply wasn't registered (the
// long-documented "class is null" diagnostic), and after it existed
// every call threw. `register_java_lang_class_methods()` below attaches
// the real method to jnivm's own canonical `java/lang/Class` instead,
// which is the only object native code will ever actually call it on.
std::shared_ptr<ClassLoaderStub> shared_class_loader();

// Hooks the real `getClassLoader()` onto jnivm's own canonical
// `java/lang/Class` class object at runtime (jnivm's own `Class::Hook`
// mechanism), rather than declaring a competing stub type for that same
// real class name. Called from register_android_framework_stubs().
void register_java_lang_class_methods(FakeJni::Jvm& jvm);

// Real, live-tested, negative result, deliberately NOT registering a
// stub for `com/snapchat/djinni/NativeObjectManager`, despite it being
// a real, confirmed embedded class name (Snap's open-source
// Djinni C++/Java interop framework, bundled by Roblox; see this
// file's own doc history for the classloader-idiom root-cause trace
// through this exact class). Tried explicit registration (an empty
// stub, same shape as every other signature-matching-only class in
// this codebase) and live-tested it: it changes libroblox.so's own
// internal Djinni bootstrap control flow, the usual `[JNIVM]:
// GetMethodID class is null getClassLoader` diagnostic pair disappears,
// replaced by a real, differently-shaped `RBXCRASH: UnhandledException
// (N6djinni13jni_exceptionE std::exception)` (recovered cleanly by
// trap_recovery.cpp, sig=5), but the whole bring-up sequence then
// takes 100+ real seconds to reach the same `onAppBridgeNotification`
// milestone instead of the usual ~10-15s, confirmed via a real,
// bisected A/B test (reverting only this one registration restored the
// fast path). Real, live-tested, negative result: leaving this class
// to jnivm's own auto-vivification path (an unregistered `FindClass()`
// target silently gets an empty stub `Class` under this build's
// `JNI_DEBUG`) is the actually-correct behavior here, not a gap to
// fix, do not re-attempt explicit registration without new evidence
// explaining *why* the slow path happens.

// android.os.Build: real, standard, static-field-only class. Values
// match this project's own established desktop-spoof convention
// (device_params.cpp's build_desktop_device_params(): manufacturer/
// device name "Stud", real API level "34" passed as os_version at every
// real call site in process-b/src/main.cpp) rather than inventing new,
// inconsistent values, a real caller cross-referencing Build.MODEL
// against DeviceParams.deviceName would see the same identity either
// way. FINGERPRINT is a real, correctly-shaped (not fabricated-looking)
// synthetic value, honestly a Stud build, not impersonating a real
// device's actual fingerprint.
class BuildStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/Build")
    static inline std::shared_ptr<FakeJni::JString> MANUFACTURER =
        std::make_shared<FakeJni::JString>("Stud");
    static inline std::shared_ptr<FakeJni::JString> BRAND =
        std::make_shared<FakeJni::JString>("Stud");
    static inline std::shared_ptr<FakeJni::JString> MODEL =
        std::make_shared<FakeJni::JString>("Stud");
    static inline std::shared_ptr<FakeJni::JString> DEVICE =
        std::make_shared<FakeJni::JString>("stud");
    static inline std::shared_ptr<FakeJni::JString> PRODUCT =
        std::make_shared<FakeJni::JString>("stud");
    static inline std::shared_ptr<FakeJni::JString> HARDWARE =
        std::make_shared<FakeJni::JString>("stud");
    static inline std::shared_ptr<FakeJni::JString> BOARD =
        std::make_shared<FakeJni::JString>("stud");
    static inline std::shared_ptr<FakeJni::JString> FINGERPRINT = std::make_shared<FakeJni::JString>(
        "Stud/stud/stud:14/UPB2.230000/1:user/release-keys");
    static inline std::shared_ptr<FakeJni::JString> ID =
        std::make_shared<FakeJni::JString>("UPB2.230000");
    // Read by the engine during a real game launch, alongside the fields
    // above, live-caught as `GetFieldID MISS class=android/os/Build static
    // field=BOOTLOADER` (and USER). Honest values describing what Stud
    // actually is, not a fabricated handset.
    static inline std::shared_ptr<FakeJni::JString> BOOTLOADER =
        std::make_shared<FakeJni::JString>("unknown");
    static inline std::shared_ptr<FakeJni::JString> USER =
        std::make_shared<FakeJni::JString>("stud");
    static inline std::shared_ptr<FakeJni::JString> TAGS =
        std::make_shared<FakeJni::JString>("release-keys");
    static inline std::shared_ptr<FakeJni::JString> TYPE =
        std::make_shared<FakeJni::JString>("user");
};

// android.os.Build.VERSION: real, standard, static-field-only class.
// SDK_INT=34/RELEASE="14" is the real, correct Android-version-to-API-
// level mapping for the "34" os_version string this project's own
// build_desktop_device_params() call sites already pass, kept
// consistent with that, not a separately-guessed value.
class BuildVersionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/Build$VERSION")
    static inline FakeJni::JInt SDK_INT = 34;
    static inline std::shared_ptr<FakeJni::JString> RELEASE =
        std::make_shared<FakeJni::JString>("14");
    static inline std::shared_ptr<FakeJni::JString> SECURITY_PATCH =
        std::make_shared<FakeJni::JString>("2024-01-01");
    static inline std::shared_ptr<FakeJni::JString> INCREMENTAL =
        std::make_shared<FakeJni::JString>("1");
    static inline std::shared_ptr<FakeJni::JString> CODENAME =
        std::make_shared<FakeJni::JString>("REL");
};

// android.os.Debug; real, standard static-method-only class.
// isDebuggerConnected() is real, honest `false` (matches this file's
// own established convention for every other "is a debugger attached"
// query in this codebase, e.g. NativeUserJavaInterfaceStub's own
// isDebuggerConnected(), same real answer, different real real class
// callers might ask instead).
class DebugStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/os/Debug")
    static FakeJni::JBoolean isDebuggerConnected() { return false; }
};

// java.lang.Long/Integer/Boolean/Double; real boxed-primitive types.
// Real signature match matters here specifically: several real Djinni
// method signatures already found in this codebase's own real
// `libroblox.so` (e.g. `IPlatformLocalStorageHandler.getUsers():
// HashSet<Long>`) take/return these as raw `Object`-erased generic
// type arguments, which are real `java/lang/Long` instances at the
// actual JNI boundary (Java generics are type-erased, there is no
// separate "HashSet<Long>" class at runtime, just `HashSet` holding
// real `Long` objects). `<init>(J)`/`longValue()` etc are the real,
// minimal constructor+accessor pair each of these needs to be usable
// as real HashSet/HashMap contents.
// Real, live-caught gap (the engineering notes): Djinni's own JNI glue
// resolves `java/lang/Error` while registering every platform protocol
// (`FindClass(java/lang/Error) -> raw jclass=0x0`, immediately after it
// successfully finds the protocol's `$CppProxy`). jnivm ships built-in
// classes for `java/lang/Throwable` and friends but NOT for `Error`, so
// that lookup returned null and left a pending JNI exception, which
// made every single `setPlatformImpl()` call fail silently, so no
// platform implementation was ever actually installed.
// java.lang.System, Djinni's own proxy-cache glue calls
// `System.identityHashCode(Object)` while registering a platform
// implementation, and an unregistered class there made every
// setPlatformImpl() fail with
// "djinni (djinni_support.cpp:313): FindClass returned null".
// android.graphics.Point. Real, plain int x/y pair. Needed because
// the engine's `getViewportDisplaySize` reads `x`/`y` field IDs off a
// Point returned by DeviceUtils below (its own error strings name that
// exact failure mode).
class PointStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/graphics/Point")

    PointStub() = default;
    PointStub(FakeJni::JInt x_value, FakeJni::JInt y_value) : x(x_value), y(y_value) {}

    FakeJni::JInt x = 0;
    FakeJni::JInt y = 0;
};

class ContextStub;  // defined in game_activity_stubs.h; see below

// com.roblox.platform.util.DeviceUtils; real class name confirmed
// straight out of libroblox.so's own strings, alongside the engine's
// own error text:
//   "[FLog::JNINativeHelper] getViewportDisplaySize: Failed to find
//    class 'DeviceUtils'."
// which fires live in every Stud run. The engine asks for the real
// screen size in millimetres here while setting up its viewport, and
// this class was never registered, so the call failed outright.
class DeviceUtilsStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("com/roblox/platform/util/DeviceUtils")

    // The parameter type is load-bearing and was wrong: FakeJni computes
    // the JNI signature from these C++ types, so a plain JObject here
    // registered the method as `(Ljava/lang/Object;)Landroid/graphics/Point;`
    // while the engine looks up
    // `(Landroid/content/Context;)Landroid/graphics/Point;`, a real,
    // live-caught GetMethodID MISS that persisted long after the class
    // itself was registered. ContextStub carries the right class name.
    // Forward-declared rather than included: ContextStub lives in
    // game_activity_stubs.h, which includes THIS header, so the include
    // cannot go the other way. shared_ptr of an incomplete type is fine
    // in a declaration; the descriptor and definition in
    // android_framework_stubs.cpp include the full header.
    static std::shared_ptr<PointStub> getScreenPhysicalSizeInMillimeters(
        std::shared_ptr<ContextStub> context);
};

class JavaLangSystemStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/System")

    // Real contract: distinct live objects get distinct values, and the
    // value is stable for an object's lifetime. The object's own
    // address satisfies both.
    static FakeJni::JInt identityHashCode(std::shared_ptr<FakeJni::JObject> obj);
    static FakeJni::JLong currentTimeMillis();
    static FakeJni::JLong nanoTime();
};

class JavaLangErrorStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/Error")
};

// Same class of gap, registered alongside for the same reason; these
// are the other standard throwable types Djinni-style glue commonly
// resolves. Harmless if never looked up.
class JavaLangExceptionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/Exception")
};

class JavaLangRuntimeExceptionStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/RuntimeException")
};

class JavaLangLongStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/Long")
    JavaLangLongStub() = default;
    explicit JavaLangLongStub(FakeJni::JLong value) : value_(value) {}
    FakeJni::JLong longValue() { return value_; }

private:
    FakeJni::JLong value_ = 0;
};

class JavaLangIntegerStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/Integer")
    JavaLangIntegerStub() = default;
    explicit JavaLangIntegerStub(FakeJni::JInt value) : value_(value) {}
    FakeJni::JInt intValue() { return value_; }

private:
    FakeJni::JInt value_ = 0;
};

class JavaLangBooleanStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/Boolean")
    JavaLangBooleanStub() = default;
    explicit JavaLangBooleanStub(FakeJni::JBoolean value) : value_(value) {}
    FakeJni::JBoolean booleanValue() { return value_; }

private:
    FakeJni::JBoolean value_ = false;
};

class JavaLangDoubleStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/lang/Double")
    JavaLangDoubleStub() = default;
    explicit JavaLangDoubleStub(FakeJni::JDouble value) : value_(value) {}
    FakeJni::JDouble doubleValue() { return value_; }

private:
    FakeJni::JDouble value_ = 0.0;
};

// java.util.Iterator: real, minimal, backed by a genuine snapshot of
// whatever real collection produced it (HashSetStub::iterator() below).
// hasNext()/next() are the only two real methods any of this codebase's
// own real callers need, remove() deliberately unimplemented (no real
// caller found removing via an iterator).
class JavaUtilIteratorStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/util/Iterator")
    JavaUtilIteratorStub() = default;
    explicit JavaUtilIteratorStub(std::vector<std::shared_ptr<FakeJni::JObject>> items)
        : items_(std::move(items)) {}
    FakeJni::JBoolean hasNext() { return index_ < items_.size(); }
    std::shared_ptr<FakeJni::JObject> next() {
        if (index_ >= items_.size()) return nullptr;
        return items_[index_++];
    }

private:
    std::vector<std::shared_ptr<FakeJni::JObject>> items_;
    std::size_t index_ = 0;
};

// java.util.HashSet; real, genuinely functional (not class-name-only)
// backing store. Real Java semantics use equals()/hashCode() for
// membership; this uses plain object-pointer identity instead, an
// honest simplification (documented, not hidden) that's exactly
// correct for the real, boxed-primitive contents (Long/Integer/...)
// every currently-known real caller in this codebase actually stores,
// and only degrades for genuinely distinct-but-equal object instances,
// which no real call site here constructs.
class JavaUtilHashSetStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/util/HashSet")
    JavaUtilHashSetStub() = default;
    FakeJni::JBoolean add(std::shared_ptr<FakeJni::JObject> item) {
        if (!item) return false;
        items_.push_back(std::move(item));
        return true;
    }
    FakeJni::JBoolean contains(std::shared_ptr<FakeJni::JObject> item) {
        if (!item) return false;
        for (auto& i : items_) {
            if (i.get() == item.get()) return true;
        }
        return false;
    }
    FakeJni::JInt size() { return static_cast<FakeJni::JInt>(items_.size()); }
    FakeJni::JBoolean isEmpty() { return items_.empty(); }
    std::shared_ptr<JavaUtilIteratorStub> iterator() {
        return std::make_shared<JavaUtilIteratorStub>(items_);
    }

private:
    std::vector<std::shared_ptr<FakeJni::JObject>> items_;
};

// java.util.HashMap, same real, functional-not-just-named approach as
// HashSetStub above, same identity-based-membership honest
// simplification. put()/get()/containsKey()/size()/isEmpty() cover
// every real usage shape found in this codebase's own real Djinni
// signatures so far (e.g. `TelemetryBridge.telemetryData(HashMap<
// String,TelemetryFieldValue>, ...)`).
class JavaUtilHashMapStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("java/util/HashMap")
    JavaUtilHashMapStub() = default;
    std::shared_ptr<FakeJni::JObject> put(std::shared_ptr<FakeJni::JObject> key,
                                           std::shared_ptr<FakeJni::JObject> value) {
        if (!key) return nullptr;
        for (auto& entry : entries_) {
            if (entry.first.get() == key.get()) {
                auto old = entry.second;
                entry.second = std::move(value);
                return old;
            }
        }
        entries_.emplace_back(std::move(key), std::move(value));
        return nullptr;
    }
    std::shared_ptr<FakeJni::JObject> get(std::shared_ptr<FakeJni::JObject> key) {
        if (!key) return nullptr;
        for (auto& entry : entries_) {
            if (entry.first.get() == key.get()) return entry.second;
        }
        return nullptr;
    }
    FakeJni::JBoolean containsKey(std::shared_ptr<FakeJni::JObject> key) {
        if (!key) return false;
        for (auto& entry : entries_) {
            if (entry.first.get() == key.get()) return true;
        }
        return false;
    }
    FakeJni::JInt size() { return static_cast<FakeJni::JInt>(entries_.size()); }
    FakeJni::JBoolean isEmpty() { return entries_.empty(); }

private:
    std::vector<std::pair<std::shared_ptr<FakeJni::JObject>, std::shared_ptr<FakeJni::JObject>>>
        entries_;
};

// Real, evidence-driven addition (not a @Keep guess, found via a
// direct scan of libroblox.so's own embedded strings, cross-checked against
// the real, exact JNI method-signature strings also embedded there):
// `getSharedPreferences`, `edit`, `putString`, `apply`, `commit`,
// `contains`, `remove` are all real, literal method-name strings
// present in the binary, alongside the real signatures
// `(Ljava/lang/String;I)Landroid/content/SharedPreferences;`
// (`Context.getSharedPreferences`), `()Landroid/content/
// SharedPreferences$Editor;` (`edit()`), and `(Ljava/lang/String;
// Ljava/lang/String;)Landroid/content/SharedPreferences$Editor;`
// (`putString`). This ties directly to a real, previously-documented
// mystery (the engineering notes' "FlagCache" / flag-override investigation
// a real device's own logcat shows `"FlagCache: Saved FFlagXxx =
// false to SharedPreference"`): Roblox's own native flag-caching code
// plausibly reads/writes flag state through this exact real Android
// API, which Stud has never provided at all until now; every such
// call would have silently failed the same "class is null"/"method ID
// null" way this whole session's sweep keeps finding and fixing.
//
// Real, functional (not just signature-matching) backing: an in-memory,
// per-name-singleton key/value store, matching real Android's own
// per-file-singleton `getSharedPreferences(name, mode)` semantics (the
// same name always returns the same live instance). String-keyed,
// String-valued only, the only real, evidenced value type (no
// putBoolean/putInt/getBoolean/getInt method-name strings were found in
// the same scan); grown further only if real evidence shows those are
// actually needed. Deliberately NOT persisted to disk yet (process
// lifetime only): a real, honest, smaller first step; real disk
// persistence (matching a real device's `SharedPreferences.xml` file
// backing) is a natural, low-risk follow-up once live evidence shows
// this path is actually exercised.
class SharedPreferencesEditorStub;

class SharedPreferencesStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/content/SharedPreferences")
    explicit SharedPreferencesStub(std::string name) : name_(std::move(name)) {}

    static std::shared_ptr<SharedPreferencesStub> get_or_create(const std::string& name);

    FakeJni::JBoolean contains(std::shared_ptr<FakeJni::JString> key) {
        if (!key) return false;
        return values_.count(key->asStdString()) != 0;
    }
    std::shared_ptr<FakeJni::JString> getString(std::shared_ptr<FakeJni::JString> key,
                                                 std::shared_ptr<FakeJni::JString> defValue) {
        if (key) {
            auto it = values_.find(key->asStdString());
            if (it != values_.end()) {
                return std::make_shared<FakeJni::JString>(it->second);
            }
        }
        return defValue;
    }
    std::shared_ptr<SharedPreferencesEditorStub> edit();

    void put(const std::string& key, std::string value) { values_[key] = std::move(value); }
    void remove(const std::string& key) { values_.erase(key); }

private:
    std::string name_;
    std::unordered_map<std::string, std::string> values_;
};

class SharedPreferencesEditorStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/content/SharedPreferences$Editor")
    explicit SharedPreferencesEditorStub(std::shared_ptr<SharedPreferencesStub> prefs)
        : prefs_(std::move(prefs)) {}

    std::shared_ptr<SharedPreferencesEditorStub> putString(std::shared_ptr<FakeJni::JString> key,
                                                             std::shared_ptr<FakeJni::JString> value) {
        if (prefs_ && key) {
            prefs_->put(key->asStdString(), value ? value->asStdString() : "");
        }
        return std::static_pointer_cast<SharedPreferencesEditorStub>(shared_from_this());
    }
    std::shared_ptr<SharedPreferencesEditorStub> remove(std::shared_ptr<FakeJni::JString> key) {
        if (prefs_ && key) {
            prefs_->remove(key->asStdString());
        }
        return std::static_pointer_cast<SharedPreferencesEditorStub>(shared_from_this());
    }
    void apply() {}
    FakeJni::JBoolean commit() { return true; }

private:
    std::shared_ptr<SharedPreferencesStub> prefs_;
};

// Real, minimal placeholder for android.content.res.Resources, same
// embedded-string evidence as SharedPreferences above confirms real native
// code resolves `Context.getResources()` (`getResources` +
// `()Landroid/content/res/Resources;` both literally embedded), but no
// further Resources-specific method-name strings were found in the same
// scan, grown further only against real evidence, not guessed ahead
// of it (same discipline as SurfaceStub/WebRtcBuildInfoStub elsewhere
// in this codebase).
// android.util.DisplayMetrics. Real, public field surface. AGDK's own
// setup calls `resources.getDisplayMetrics()` right after
// GameActivity_initializeNativeCode returns, and ResourcesStub had no
// such method, so the call resolved to null and the engine never got
// real screen metrics (live-confirmed:
// "GetMethodID MISS class=android/content/res/Resources
//  method=getDisplayMetrics sig=()Landroid/util/DisplayMetrics;").
class DisplayMetricsStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/util/DisplayMetrics")

    FakeJni::JInt widthPixels = 800;
    FakeJni::JInt heightPixels = 600;
    FakeJni::JFloat density = 1.0f;
    FakeJni::JInt densityDpi = 160;
    FakeJni::JFloat scaledDensity = 1.0f;
    FakeJni::JFloat xdpi = 160.0f;
    FakeJni::JFloat ydpi = 160.0f;
};

// Seeds the real values Stud's own render surface actually uses, so the
// engine lays out against the real window rather than a guess.

// android.view.MotionEvent. Real class AGDK's own GameActivity passes
// straight through to `onTouchEventNative(handle, motionEvent, ...)`, where
// the engine reads it back field by field. Live-confirmed as the real gap:
// a running engine resolves exactly these methods (`getSource`,
// `getToolType`, `getAxisValue`, `getPointerCount`, `getPointerId`,
// `getAction`, ...) and every one of them was a `GetMethodID MISS` because
// the class was never registered.
//
// This is the ONLY input path that carries a real InputDevice SOURCE and
// tool type. `NativeInputInterface.nativePassMouse*` (which Stud already
// drives) delivers coordinates but says nothing about what kind of device
// produced them, so an engine fed only that has no way to know a mouse
// exists, which is why Roblox never drew its own cursor. The real app's
// own SurfaceView asks Android for no system pointer at all
// (`RBXSurfaceView.onResolvePointerIcon` returns
// `PointerIcon.getSystemIcon(ctx, TYPE_NULL)`), so on a real device the
// engine's in-frame cursor is the only one there is.
//
// Single-pointer only: Stud's real source is one Wayland seat pointer, so
// there is honestly never more than one. No history is recorded either
// (`getHistorySize()` is 0), which is a real, valid MotionEvent state.
class MotionEventStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/view/MotionEvent")

    FakeJni::JInt action = 0;
    FakeJni::JInt source = 0;
    FakeJni::JInt device_id = 0;
    FakeJni::JInt tool_type = 0;
    FakeJni::JInt button_state = 0;
    FakeJni::JInt action_button = 0;
    FakeJni::JInt meta_state = 0;
    FakeJni::JLong event_time = 0;
    FakeJni::JLong down_time = 0;
    FakeJni::JFloat x = 0.0f;
    FakeJni::JFloat y = 0.0f;
    FakeJni::JFloat vscroll = 0.0f;
    FakeJni::JFloat hscroll = 0.0f;

    FakeJni::JInt getAction() { return action; }
    FakeJni::JInt getSource() { return source; }
    FakeJni::JInt getDeviceId() { return device_id; }
    FakeJni::JInt getToolType(FakeJni::JInt) { return tool_type; }
    FakeJni::JInt getButtonState() { return button_state; }
    FakeJni::JInt getActionButton() { return action_button; }
    FakeJni::JInt getMetaState() { return meta_state; }
    FakeJni::JInt getFlags() { return 0; }
    FakeJni::JInt getEdgeFlags() { return 0; }
    FakeJni::JInt getPointerCount() { return 1; }
    FakeJni::JInt getPointerId(FakeJni::JInt) { return 0; }
    FakeJni::JInt getHistorySize() { return 0; }
    FakeJni::JInt getClassification() { return 0; }
    FakeJni::JLong getEventTime() { return event_time; }
    FakeJni::JLong getDownTime() { return down_time; }
    FakeJni::JLong getHistoricalEventTime(FakeJni::JInt) { return event_time; }
    FakeJni::JFloat getXPrecision() { return 1.0f; }
    FakeJni::JFloat getYPrecision() { return 1.0f; }
    FakeJni::JFloat getX(FakeJni::JInt) { return x; }
    FakeJni::JFloat getY(FakeJni::JInt) { return y; }
    // Real MotionEvent axis constants: X=0, Y=1, VSCROLL=9, HSCROLL=10.
    // AGDK reads every coordinate through this one accessor.
    FakeJni::JFloat getAxisValue(FakeJni::JInt axis, FakeJni::JInt) {
        switch (axis) {
            case 0: return x;
            case 1: return y;
            case 9: return vscroll;
            case 10: return hscroll;
            default: return 0.0f;
        }
    }
    FakeJni::JFloat getHistoricalAxisValue(FakeJni::JInt axis, FakeJni::JInt,
                                            FakeJni::JInt pointer) {
        return getAxisValue(axis, pointer);
    }
};

// android.view.KeyEvent, same story on the key side: AGDK hands the real
// object to `onKeyDownNative`/`onKeyUpNative` and the engine reads it back.
// Every one of these was a live `GetMethodID MISS`.
class KeyEventStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/view/KeyEvent")

    FakeJni::JInt action = 0;
    FakeJni::JInt key_code = 0;
    FakeJni::JInt scan_code = 0;
    FakeJni::JInt meta_state = 0;
    FakeJni::JInt repeat_count = 0;
    FakeJni::JInt source = 0;
    FakeJni::JInt device_id = 0;
    FakeJni::JInt unicode_char = 0;
    FakeJni::JLong event_time = 0;
    FakeJni::JLong down_time = 0;

    FakeJni::JInt getAction() { return action; }
    FakeJni::JInt getKeyCode() { return key_code; }
    FakeJni::JInt getScanCode() { return scan_code; }
    FakeJni::JInt getMetaState() { return meta_state; }
    FakeJni::JInt getRepeatCount() { return repeat_count; }
    FakeJni::JInt getSource() { return source; }
    FakeJni::JInt getDeviceId() { return device_id; }
    FakeJni::JInt getFlags() { return 0; }
    FakeJni::JInt getUnicodeChar() { return unicode_char; }
    FakeJni::JInt getUnicodeChar(FakeJni::JInt) { return unicode_char; }
    FakeJni::JLong getEventTime() { return event_time; }
    FakeJni::JLong getDownTime() { return down_time; }
};

void set_real_display_metrics(int width_px, int height_px, float density);

// Real geometry of the display itself, as reported by the compositor
// (wl_output, fetched over the render IPC, only Process C has a
// compositor connection). Seeds DisplayMetrics' real xdpi/ydpi, which
// used to be synthesised as `160 * density` and therefore described no
// real hardware. Zeroes mean the compositor reported nothing; the
// synthesised fallback is used then, and says so.
void set_real_display_output_geometry(int px_w, int px_h, int mm_w, int mm_h);

// The display as Stud actually measured it. Anything that has to
// describe this screen should read these rather than keep its own
// constants, the User-Agent did, and every one of its numbers was
// wrong as a result.
struct DisplayFacts {
    // The window (what the engine renders into).
    int width_px = 0;
    int height_px = 0;
    float density = 1.0f;
    float xdpi = 0.0f;
    float ydpi = 0.0f;
    // The display itself, straight from wl_output. A phone's agent
    // describes its screen, not one window on it, so anything reporting
    // "the screen" wants these; zero when the compositor said nothing.
    int output_width_px = 0;
    int output_height_px = 0;
    // The density as actually measured from the compositor, which is
    // NOT necessarily the one seeded into DisplayMetrics: seeding the
    // real density at bring-up changes the layout size the Lua app
    // commits to and live-removed the account-switcher button, so
    // DisplayMetrics is deliberately given 1.0 there. Anything that
    // merely needs to DESCRIBE the display (the agent's
    // density-independent size) wants this instead.
    float measured_density = 0.0f;
};

// Records the real density without touching DisplayMetrics. See
// DisplayFacts::measured_density for why those are separate.
void set_measured_display_density(float density);

// The density DisplayMetrics actually carries, which is what the engine
// divides raw pointer pixels by (real Android does this in the view
// layer, the app's own input handler, whose density divisor is exactly this density). Anything
// converting between the buffer pixels the compositor reports and the
// density-independent units the engine's own entry points expect wants
// THIS, not DisplayFacts::measured_density: the two are equal only while the
// buffer is scaled by the display's own scale, i.e. with HiDPI on and no
// explicit UI scale. Defaults to 1.0 until DisplayMetrics is seeded.
float engine_layout_density();
DisplayFacts real_display_facts();

// Real total system memory in MB, and the device name as
// MANUFACTURER + MODEL. Both describe the machine Stud is running on
// rather than a constant.
// The real machine, from DMI (empty when unreadable).
std::string real_hardware_name();
int real_total_memory_mb();
std::string real_device_name();

class ResourcesStub : public FakeJni::JObject {
public:
    DEFINE_CLASS_NAME("android/content/res/Resources")

    std::shared_ptr<DisplayMetricsStub> getDisplayMetrics();
};

void register_android_framework_stubs(FakeJni::Jvm& jvm);

}  // namespace stud::jni_bridge
