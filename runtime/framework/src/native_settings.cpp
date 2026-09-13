#include "stud/native_settings.h"

#include "stud/trap_recovery.h"

#include <cstring>

namespace stud::jni_bridge {

namespace {

using SetBaseUrlFn = void (*)(JNIEnv*, jclass, jstring, jstring);
using SetHttpClientProxyFn = void (*)(JNIEnv*, jclass, jstring, jlong);
using SetMultipleCookiesFn = void (*)(JNIEnv*, jclass, jstring, jstring);
using SetFilesDirectoryFn = void (*)(JNIEnv*, jclass, jstring);
using SetCacheDirectoryFn = void (*)(JNIEnv*, jclass, jstring);
using SetBaseDataDirectoriesFn = void (*)(JNIEnv*, jclass, jstring, jstring);
using SetPreferencesFileFn = void (*)(JNIEnv*, jclass, jstring);
using SetExternalDirectoryFn = void (*)(JNIEnv*, jclass, jstring);
using SetExceptionReasonFilenameFn = void (*)(JNIEnv*, jclass, jstring);
using SetRobloxChannelFn = void (*)(JNIEnv*, jclass, jstring);
using OverrideChannelPlatformNameFn = void (*)(JNIEnv*, jclass, jstring);
using InitFastLogFn = void (*)(JNIEnv*, jclass);
using SetRobloxVersionFn = void (*)(JNIEnv*, jclass, jstring);

template <typename Fn>
Fn find_required_symbol(const stud::linker::LoadedLibrary& lib, const char* name) {
    void* addr = lib.find_symbol(name);
    if (addr == nullptr) {
        throw stud::linker::LoadError(std::string("native_settings: required symbol not found: ") +
                                       name);
    }
    return reinterpret_cast<Fn>(addr);
}

// The engine's own cookie jar for a URL, exactly as it hands it over:
// Netscape cookie-file format, one tab-separated record per line
// (domain, include-subdomains, path, secure, expiry, name, value), with
// host-only entries prefixed `#HttpOnly_`. Empty if this build does not
// export the entry point or the engine has nothing for that URL.
//
// The returned string contains real credentials. Never log it.
std::string read_raw_cookie_jar(FakeJni::Env& env, JNIEnv* jni_env,
                                 const stud::linker::LoadedLibrary& lib, const std::string& url) {
    using GetCookiesForDomainFn = jstring (*)(JNIEnv*, jclass, jstring);
    void* addr = lib.find_symbol(
        "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeGetCookiesForDomain");
    if (addr == nullptr) return {};
    auto* get_cookies = reinterpret_cast<GetCookiesForDomainFn>(addr);
    jstring url_jstring = env.NewStringUTF(url.c_str());
    jstring cookies_back = nullptr;
    bool ok = call_trapping_abort_with_result(get_cookies, cookies_back, jni_env, nullptr,
                                               url_jstring);
    clear_pending_jni_exception(jni_env, "nativeGetCookiesForDomain");
    if (!ok || cookies_back == nullptr) return {};
    auto resolved = env.resolveReference(cookies_back);
    auto as_string = std::dynamic_pointer_cast<FakeJni::JString>(resolved);
    return as_string ? as_string->asStdString() : std::string();
}

}  // namespace

NativeSettingsResult run_native_settings_bootstrap(FakeJni::Jvm& jvm,
                                                    const stud::linker::LoadedLibrary& lib,
                                                    const NativeSettingsConfig& config) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    NativeSettingsResult result;

    // Real, exact order from the app's own settings bootstrap (the engineering notes,
    // "instantiate controllers" investigation): exception-reason
    // filename first, then base URL/channel/platform name, then cache/
    // files directories, then FastLog init, then Roblox version --
    // matched exactly here rather than an arbitrary order, since the bootstrap's
    // own real callers depend on this sequence (e.g. its directory
    // calls explicitly null-check that field first, gated on running after
    // the exception-filename/base-URL group above it).
    auto* set_exception_reason_filename = find_required_symbol<SetExceptionReasonFilenameFn>(
        lib,
        "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetExceptionReasonFilename");
    jstring exception_reason_filename_jstring =
        env.NewStringUTF(config.exception_reason_filename.c_str());
    result.set_exception_reason_filename_called = true;
    result.set_exception_reason_filename_trapped_abort = !call_trapping_abort(
        set_exception_reason_filename, jni_env, nullptr, exception_reason_filename_jstring);
    clear_pending_jni_exception(jni_env, "nativeSetExceptionReasonFilename");

    // Real, optional, called first when a real files directory is
    // configured -- matches try_bootstrap.cpp's own established probe
    // ordering ("nativeSetFilesDirectory, early"). Skipped (not just a
    // no-op empty-string call) when files_directory is empty, since an
    // empty path isn't a real directory Roblox's engine could actually
    // write into -- callers without a real persistent directory yet get
    // the previous, unchanged behavior rather than a call that would
    // just fail silently on Roblox's own side.
    if (!config.files_directory.empty()) {
        auto* set_files_directory = find_required_symbol<SetFilesDirectoryFn>(
            lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetFilesDirectory");
        jstring files_directory_jstring = env.NewStringUTF(config.files_directory.c_str());
        result.set_files_directory_called = true;
        result.set_files_directory_trapped_abort =
            !call_trapping_abort(set_files_directory, jni_env, nullptr,
                                         files_directory_jstring);
        clear_pending_jni_exception(jni_env, "nativeSetFilesDirectory");
    }

    // Real gap closed (the engineering notes, "verify the 2D shell renders"
    // entry): a real device's RbxStorage subsystem needs this to
    // initialize its local-storage layer at all -- confirmed via real
    // FLog output ("Failed to get cache directory: Path does not exist:
    // \"\"") that omitting it isn't a silent no-op, it's a real,
    // observable failure blocking Roblox's own storage init.
    if (!config.cache_directory.empty()) {
        auto* set_cache_directory = find_required_symbol<SetCacheDirectoryFn>(
            lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetCacheDirectory");
        jstring cache_directory_jstring = env.NewStringUTF(config.cache_directory.c_str());
        result.set_cache_directory_called = true;
        result.set_cache_directory_trapped_abort =
            !call_trapping_abort(set_cache_directory, jni_env, nullptr,
                                         cache_directory_jstring);
        clear_pending_jni_exception(jni_env, "nativeSetCacheDirectory");
    }

    // Real, immediately-following calls in the app's own settings bootstrap, right
    // after its cache/files directory pair.
    auto* init_fast_log = find_required_symbol<InitFastLogFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeInitFastLog");
    result.init_fast_log_called = true;
    result.init_fast_log_trapped_abort =
        !call_trapping_abort(init_fast_log, jni_env, nullptr);
    clear_pending_jni_exception(jni_env, "nativeInitFastLog");

    auto* set_roblox_version = find_required_symbol<SetRobloxVersionFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetRobloxVersion");
    jstring roblox_version_jstring = env.NewStringUTF(config.roblox_version.c_str());
    result.set_roblox_version_called = true;
    result.set_roblox_version_trapped_abort =
        !call_trapping_abort(set_roblox_version, jni_env, nullptr, roblox_version_jstring);
    clear_pending_jni_exception(jni_env, "nativeSetRobloxVersion");

    if (!config.base_internal_directory.empty() && !config.base_external_directory.empty()) {
        auto* set_base_data_directories = find_required_symbol<SetBaseDataDirectoriesFn>(
            lib,
            "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetBaseDataDirectories");
        jstring internal_jstring = env.NewStringUTF(config.base_internal_directory.c_str());
        jstring external_jstring = env.NewStringUTF(config.base_external_directory.c_str());
        result.set_base_data_directories_called = true;
        result.set_base_data_directories_trapped_abort =
            !call_trapping_abort(set_base_data_directories, jni_env, nullptr,
                                         internal_jstring, external_jstring);
        clear_pending_jni_exception(jni_env, "nativeSetBaseDataDirectories");
    }

    // Real, confirmed: `InitHelper` (`InitHelper`) calls
    // `nativeSetPreferencesFile(<preferences name>)` immediately after
    // nativeSetBaseDataDirectories above, and the app's own getter returns the plain
    // preferences NAME `"prefs"` (the app's own preferences helper -> `"prefs"`)
    // -- not a path; the engine resolves it against the data directories
    // just set. Stud never called this.
    //
    // Why it matters (live-confirmed via the engine's own FLog, now
    // visible through Stud's logd sink): the engine logs
    // `initializeLuaApp_: ... cachedUserId:-1` and the Lua app's
    // PlatformAccountRouter therefore routes to the logged-out
    // `Landing` screen -- `userDidLogin` only arrives ~0.8s LATER, well
    // after routing already happened. On a real device the cached user
    // id is read back from this preferences store, written by a
    // previous run, so the router goes straight to `Home`. Without a
    // preferences file the engine has nowhere to persist it and every
    // Stud launch looks like a first-ever launch.
    {
        auto* set_preferences_file = find_required_symbol<SetPreferencesFileFn>(
            lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetPreferencesFile");
        jstring prefs_jstring = env.NewStringUTF("prefs");
        result.set_preferences_file_called = true;
        result.set_preferences_file_trapped_abort =
            !call_trapping_abort(set_preferences_file, jni_env, nullptr, prefs_jstring);
        clear_pending_jni_exception(jni_env, "nativeSetPreferencesFile");
    }

    if (!config.external_directory.empty()) {
        auto* set_external_directory = find_required_symbol<SetExternalDirectoryFn>(
            lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetExternalDirectory");
        jstring external_directory_jstring = env.NewStringUTF(config.external_directory.c_str());
        result.set_external_directory_called = true;
        result.set_external_directory_trapped_abort =
            !call_trapping_abort(set_external_directory, jni_env, nullptr,
                                         external_directory_jstring);
        clear_pending_jni_exception(jni_env, "nativeSetExternalDirectory");
    }

    // Real call order traced from the app's own HTTP/cookie layer (the engineering notes,
    // M7): base URL first, proxy and cookies are independent of it and
    // each other, order between them doesn't matter to Roblox's own code
    // (confirmed: the other two are called from unrelated places, never
    // adjacent in the real sequence) but is kept here for readability.
    auto* set_base_url = find_required_symbol<SetBaseUrlFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetBaseUrl");
    jstring base_url_jstring = env.NewStringUTF(config.base_url.c_str());
    jstring api_base_url_jstring = env.NewStringUTF(config.api_base_url.c_str());
    result.set_base_url_called = true;
    result.set_base_url_trapped_abort =
        !call_trapping_abort(set_base_url, jni_env, nullptr, base_url_jstring,
                                     api_base_url_jstring);
    clear_pending_jni_exception(jni_env, "nativeSetBaseUrl");

    // Real, immediately-following calls in the app's own settings bootstrap, right
    // after nativeSetBaseUrl.
    auto* set_roblox_channel = find_required_symbol<SetRobloxChannelFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetRobloxChannel");
    jstring roblox_channel_jstring = env.NewStringUTF(config.roblox_channel.c_str());
    result.set_roblox_channel_called = true;
    result.set_roblox_channel_trapped_abort =
        !call_trapping_abort(set_roblox_channel, jni_env, nullptr, roblox_channel_jstring);
    clear_pending_jni_exception(jni_env, "nativeSetRobloxChannel");

    auto* override_channel_platform_name = find_required_symbol<OverrideChannelPlatformNameFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeOverrideChannelPlatformName");
    jstring platform_name_jstring = env.NewStringUTF(config.platform_name.c_str());
    result.override_channel_platform_name_called = true;
    result.override_channel_platform_name_trapped_abort = !call_trapping_abort(
        override_channel_platform_name, jni_env, nullptr, platform_name_jstring);
    clear_pending_jni_exception(jni_env, "nativeOverrideChannelPlatformName");

    auto* set_http_client_proxy = find_required_symbol<SetHttpClientProxyFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetHttpClientProxy");
    jstring proxy_host_jstring = env.NewStringUTF(config.http_proxy_host.c_str());
    result.set_http_client_proxy_called = true;
    result.set_http_client_proxy_trapped_abort = !call_trapping_abort(
        set_http_client_proxy, jni_env, nullptr, proxy_host_jstring,
        static_cast<jlong>(config.http_proxy_port));
    clear_pending_jni_exception(jni_env, "nativeSetHttpClientProxy");

    auto* set_multiple_cookies = find_required_symbol<SetMultipleCookiesFn>(
        lib, "Java_com_roblox_engine_jni_NativeSettingsInterface_nativeSetMultipleCookies");
    jstring cookies_base_url_jstring = env.NewStringUTF(config.base_url.c_str());
    jstring cookies_jstring = env.NewStringUTF(config.cookies.c_str());
    result.set_multiple_cookies_called = true;
    result.set_multiple_cookies_trapped_abort =
        !call_trapping_abort(set_multiple_cookies, jni_env, nullptr,
                                     cookies_base_url_jstring, cookies_jstring);
    clear_pending_jni_exception(jni_env, "nativeSetMultipleCookies");

    // Real read-back: did the engine actually accept and store the
    // cookie we just handed it? The Lua app decides logged-in vs
    // logged-out from the engine's own auth state, so "we called
    // setMultipleCookies and it didn't trap" is not evidence the cookie
    // is really in the engine's jar. `nativeGetCookiesForDomain` is a
    // real exported entry point that reads it straight back out.
    //
    // Presence and length only -- never the value, which is a real
    // credential.
    {
        const std::string jar = read_raw_cookie_jar(env, jni_env, lib, config.base_url);
        if (jar.empty()) {
            std::printf("stud: nativeGetCookiesForDomain returned nothing for %s\n",
                        config.base_url.c_str());
        } else {
            // Names only -- values are real credentials. The format is
            // tab-separated records, so the name is the second-to-last
            // field of each line.
            std::string names;
            size_t line_start = 0;
            while (line_start < jar.size()) {
                size_t line_end = jar.find('\n', line_start);
                if (line_end == std::string::npos) line_end = jar.size();
                const std::string line = jar.substr(line_start, line_end - line_start);
                line_start = line_end + 1;
                size_t value_tab = line.rfind('\t');
                if (value_tab == std::string::npos || value_tab == 0) continue;
                size_t name_tab = line.rfind('\t', value_tab - 1);
                if (name_tab == std::string::npos) continue;
                if (!names.empty()) names += ", ";
                names += line.substr(name_tab + 1, value_tab - name_tab - 1);
            }
            std::printf("stud: engine cookie jar for %s: %zu bytes, contains .ROBLOSECURITY=%s, "
                        "names=[%s]\n",
                        config.base_url.c_str(), jar.size(),
                        jar.find(".ROBLOSECURITY") != std::string::npos ? "YES" : "NO",
                        names.c_str());
        }
        std::fflush(stdout);
    }

    return result;
}

std::vector<std::string> engine_cookies_for_url(FakeJni::Jvm& jvm,
                                                 const stud::linker::LoadedLibrary& lib,
                                                 const std::string& url) {
    FakeJni::LocalFrame frame(jvm);
    auto& env = frame.getJniEnv();
    auto* jni_env = static_cast<JNIEnv*>(&env);

    const std::string jar = read_raw_cookie_jar(env, jni_env, lib, url);
    std::vector<std::string> headers;
    size_t line_start = 0;
    while (line_start < jar.size()) {
        size_t line_end = jar.find('\n', line_start);
        if (line_end == std::string::npos) line_end = jar.size();
        std::string line = jar.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        // `#HttpOnly_<domain>` is a real record, not a comment; any other
        // line starting with '#' is one.
        static constexpr const char* kHttpOnly = "#HttpOnly_";
        bool http_only = line.rfind(kHttpOnly, 0) == 0;
        if (http_only) line.erase(0, std::strlen(kHttpOnly));
        else if (line[0] == '#') continue;

        // domain \t include-subdomains \t path \t secure \t expiry \t name \t value
        std::string field[7];
        size_t count = 0;
        size_t pos = 0;
        while (count < 7) {
            size_t tab = line.find('\t', pos);
            if (tab == std::string::npos) {
                field[count++] = line.substr(pos);
                break;
            }
            field[count++] = line.substr(pos, tab - pos);
            pos = tab + 1;
        }
        if (count < 7 || field[5].empty()) continue;

        // A Set-Cookie style header, which is what a cookie store parses.
        // Domain is carried over so a jar holding more than one host's
        // cookies still applies each to the right one.
        std::string header = field[5] + "=" + field[6];
        if (!field[0].empty()) header += "; Domain=" + field[0];
        header += "; Path=" + (field[2].empty() ? std::string("/") : field[2]);
        if (field[3] == "TRUE") header += "; Secure";
        if (http_only) header += "; HttpOnly";
        headers.push_back(std::move(header));
    }
    return headers;
}

}  // namespace stud::jni_bridge
