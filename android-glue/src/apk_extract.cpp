#include "stud/android_glue.h"

#include <miniz.h>
#include <miniz_zip.h>

#include <sys/stat.h>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

// mkdir -p equivalent -- miniz's mz_zip_reader_extract_to_file() doesn't
// create parent directories on its own, and the APK's assets/ tree is
// genuinely nested (e.g. "assets/ExtraContent/places/Mobile.rbxl",
// "assets/ssl/cacert.pem").
void make_directories(const std::string& path) {
    std::string current;
    for (char c : path) {
        if (c == '/' && !current.empty()) {
            ::mkdir(current.c_str(), 0755);
        }
        current += c;
    }
    if (!current.empty()) {
        ::mkdir(current.c_str(), 0755);
    }
}

std::string parent_directory(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

std::string base_name(const std::string& path) {
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// APKMirror's .apkm and SAI's .apks are both plain zips holding a whole
// split-APK bundle: a base.apk with the app's assets and dex, plus one
// split per ABI/density/language. Google Play has shipped Roblox as a
// bundle for a long time, so a monolithic .apk is the thing that is
// getting harder to find, not the bundle -- supporting bundles directly
// is what stops Stud depending on someone republishing a merged APK.
constexpr std::string_view kBundleMarker = "base.apk";

bool zip_contains(mz_zip_archive& zip, const char* entry) {
    return mz_zip_reader_locate_file(&zip, entry, nullptr, 0) >= 0;
}

// .xapk is the same idea with different names.
//
// APKMirror's .apkm and SAI's .apks both call the base split "base.apk",
// which is what kBundleMarker looks for. An .xapk (APKPure and friends)
// names it after the package instead -- `com.roblox.client.apk` -- and
// describes the set in a `manifest.json` beside it. So an .xapk holds a
// perfectly ordinary split-APK bundle that the base.apk check alone
// would miss, and Stud would then try to read the outer zip as if it
// were one APK and find no libroblox.so in it.
//
// The manifest is the honest marker: it is what makes this file a
// bundle rather than a zip that happens to contain an apk. Its contents
// are not parsed -- the split members are already found by walking the
// archive, and a format that can rename the base can rename anything
// else in the JSON too.
constexpr std::string_view kXapkManifest = "manifest.json";

bool looks_like_bundle(mz_zip_archive& zip) {
    if (zip_contains(zip, std::string(kBundleMarker).c_str())) return true;
    if (!zip_contains(zip, std::string(kXapkManifest).c_str())) return false;
    // A manifest alone is not enough: plenty of zips carry one. It has to
    // come with at least one top-level .apk for this to be a bundle.
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::string_view name(stat.m_filename);
        if (name.size() < 5 || name.substr(name.size() - 4) != ".apk") continue;
        if (name.find('/') != std::string_view::npos) continue;
        return true;
    }
    return false;
}

// The base split of a bundle, whatever it is called.
//
// It is the one that carries the app's own assets and manifest, and it
// has to be read first -- every later split is an overlay on it. "base"
// for an .apkm/.apks; for an .xapk, the member with no `config.` or
// `split_` in its name, which is how those tools name every non-base
// split.
bool is_base_split(std::string_view name) {
    if (name == kBundleMarker) return true;
    return name.find("config.") == std::string_view::npos &&
           name.find("split_") == std::string_view::npos &&
           name.find("split.") == std::string_view::npos;
}

// Splits for ABIs Stud cannot run. Everything else in the bundle -- the
// base, the x86_64 split, feature splits like gmasdk, language and
// density splits -- is kept, since any of them may legitimately carry
// assets.
bool is_foreign_abi_split(std::string_view name) {
    return name.find("arm64_v8a") != std::string_view::npos ||
           name.find("armeabi_v7a") != std::string_view::npos ||
           name.find("_x86.apk") != std::string_view::npos ||
           name.find("config.x86.apk") != std::string_view::npos ||
           name.find("mips") != std::string_view::npos;
}

std::string cache_root() {
    if (const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME")) {
        return std::string(xdg_cache_home) + "/stud";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/.cache/stud";
    }
    return "/tmp/stud";
}

// Unpacks the bundle's own .apk members once into the cache and returns
// their paths, base first. Cached by the bundle's file name and size:
// re-extracting ~200MB on every launch would be a real, noticeable cost,
// and a bundle that differs in either is a different bundle.
std::vector<std::string> unpack_bundle_members(const std::string& bundle_path,
                                                mz_zip_archive& zip) {
    struct ::stat bundle_stat{};
    const long long bundle_size =
        ::stat(bundle_path.c_str(), &bundle_stat) == 0 ? static_cast<long long>(bundle_stat.st_size)
                                                        : 0;
    const std::string dest_dir = cache_root() + "/bundles/" + base_name(bundle_path) + "-" +
                                  std::to_string(bundle_size);
    make_directories(dest_dir);

    std::vector<std::string> members;
    std::string base_member;
    const mz_uint num_files = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < num_files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;

        std::string_view name(stat.m_filename);
        if (name.size() < 5 || name.substr(name.size() - 4) != ".apk") continue;
        if (name.find('/') != std::string_view::npos) continue;  // never nested in a real bundle
        if (is_foreign_abi_split(name)) continue;

        const std::string dest_path = dest_dir + "/" + std::string(name);
        struct ::stat existing{};
        const bool already_extracted =
            ::stat(dest_path.c_str(), &existing) == 0 &&
            static_cast<mz_uint64>(existing.st_size) == stat.m_uncomp_size;
        if (!already_extracted &&
            !mz_zip_reader_extract_to_file(&zip, i, dest_path.c_str(), 0)) {
            std::string error = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
            throw stud::android_glue::ExtractError("stud: failed to extract '" + std::string(name) + "' from bundle '" +
                                bundle_path + "': " + error);
        }
        // An .apkm/.apks says which member is the base by name, and that
        // always wins. An .xapk does not, so the first member that looks
        // like one stands in until a real base.apk turns up -- feature
        // splits (gmasdk.apk and friends) carry neither marker either, so
        // without the exact match taking precedence one of them could be
        // read first.
        if (name == kBundleMarker) {
            if (!base_member.empty()) members.push_back(base_member);
            base_member = dest_path;
        } else if (base_member.empty() && is_base_split(name)) {
            base_member = dest_path;
        } else {
            members.push_back(dest_path);
        }
    }
    if (!base_member.empty()) members.insert(members.begin(), base_member);
    return members;
}

// Returns the real .apk files to read from: the file itself when it is a
// plain APK, or the bundle's own unpacked members when it is an .apkm,
// .apks or .xapk. Detected by content, not by file extension, since any
// of those extensions can hold either thing.
std::vector<std::string> resolve_apk_sources_impl(const std::string& path) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) {
        throw stud::android_glue::ExtractError("stud: failed to open '" + path + "' as a zip archive: " +
                            mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    }
    if (!looks_like_bundle(zip)) {
        mz_zip_reader_end(&zip);
        return {path};
    }
    std::vector<std::string> members;
    try {
        members = unpack_bundle_members(path, zip);
    } catch (...) {
        mz_zip_reader_end(&zip);
        throw;
    }
    mz_zip_reader_end(&zip);
    if (members.empty()) {
        throw stud::android_glue::ExtractError("stud: '" + path +
                            "' looks like a split-APK bundle but contains no usable .apk member");
    }
    return members;
}

}  // namespace

namespace stud::android_glue {

namespace {
void extract_assets_from_single_apk(const std::string& apk_path, const std::string& dest_dir);
void extract_native_library_from_single_apk(const std::string& apk_path,
                                             const std::string& library_name,
                                             const std::string& dest_path, bool* found);
}  // namespace

void extract_apk_assets(const std::string& apk_path, const std::string& dest_dir) {
    // Base first, then splits: a split that also ships an asset wins, and
    // that is the same precedence a real device's split loader applies.
    for (const std::string& source : resolve_apk_sources_impl(apk_path)) {
        extract_assets_from_single_apk(source, dest_dir);
    }
}

void extract_apk_native_library(const std::string& apk_path, const std::string& library_name,
                                 const std::string& dest_path) {
    // The x86_64 native library lives in its own ABI split in a bundle,
    // and in the single file for a monolithic APK -- try each source and
    // take the first that actually carries it.
    const std::vector<std::string> sources = resolve_apk_sources_impl(apk_path);
    for (const std::string& source : sources) {
        bool found = false;
        extract_native_library_from_single_apk(source, library_name, dest_path, &found);
        if (found) return;
    }
    throw ExtractError("stud: no x86_64 '" + library_name + "' in '" + apk_path +
                        "' (searched " + std::to_string(sources.size()) +
                        " APK(s); a bundle with no x86_64 split cannot run here)");
}

namespace {

void extract_assets_from_single_apk(const std::string& apk_path, const std::string& dest_dir) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, apk_path.c_str(), 0)) {
        throw ExtractError("stud: failed to open APK '" + apk_path +
                            "' as a zip archive: " + mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    }

    make_directories(dest_dir);

    static constexpr std::string_view kPrefix = "assets/";
    mz_uint num_files = mz_zip_reader_get_num_files(&zip);

    for (mz_uint i = 0; i < num_files; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }

        std::string_view name(stat.m_filename);
        if (name.size() <= kPrefix.size() || name.substr(0, kPrefix.size()) != kPrefix) {
            continue;
        }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            continue;
        }

        std::string relative(name.substr(kPrefix.size()));
        std::string dest_path = dest_dir + "/" + relative;
        make_directories(parent_directory(dest_path));

        if (!mz_zip_reader_extract_to_file(&zip, i, dest_path.c_str(), 0)) {
            std::string error = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
            mz_zip_reader_end(&zip);
            throw ExtractError("stud: failed to extract '" + std::string(name) + "' from APK '" +
                                apk_path + "': " + error);
        }
    }

    mz_zip_reader_end(&zip);
}

void extract_native_library_from_single_apk(const std::string& apk_path,
                                             const std::string& library_name,
                                             const std::string& dest_path, bool* found) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, apk_path.c_str(), 0)) {
        throw ExtractError("stud: failed to open APK '" + apk_path +
                            "' as a zip archive: " + mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
    }

    std::string entry_name = "lib/x86_64/" + library_name;
    int file_index = mz_zip_reader_locate_file(&zip, entry_name.c_str(), nullptr, 0);
    if (file_index < 0) {
        // Not an error here: in a bundle only one split carries it, so
        // the caller walks every source and reports the failure once.
        mz_zip_reader_end(&zip);
        return;
    }

    make_directories(parent_directory(dest_path));

    if (!mz_zip_reader_extract_to_file(&zip, static_cast<mz_uint>(file_index), dest_path.c_str(), 0)) {
        std::string error = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
        mz_zip_reader_end(&zip);
        throw ExtractError("stud: failed to extract '" + entry_name + "' from APK '" + apk_path +
                            "': " + error);
    }

    mz_zip_reader_end(&zip);
    if (found != nullptr) *found = true;
}

}  // namespace

std::vector<std::string> resolve_apk_sources_for_metadata(const std::string& path) {
    return resolve_apk_sources_impl(path);
}

std::string apk_source_fingerprint(const std::string& apk_path) {
    struct ::stat st{};
    if (::stat(apk_path.c_str(), &st) != 0) return {};
    return apk_path + "|" + std::to_string(static_cast<long long>(st.st_size)) + "|" +
           std::to_string(static_cast<long long>(st.st_mtime));
}

std::string default_libroblox_cache_path() {
    if (const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME")) {
        return std::string(xdg_cache_home) + "/stud/libroblox.so";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.cache/stud/libroblox.so";
}

std::string default_assets_cache_dir() {
    if (const char* xdg_cache_home = std::getenv("XDG_CACHE_HOME")) {
        return std::string(xdg_cache_home) + "/stud/assets";
    }
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "") + "/.cache/stud/assets";
}

}  // namespace stud::android_glue
