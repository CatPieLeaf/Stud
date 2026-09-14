// M7 test: real APK asset extraction (android-glue/src/apk_extract.cpp),
// and that AAssetManager correctly serves an extracted file end-to-end,
// closing the gap between "AAssetManager works" (tested with synthetic
// data elsewhere) and "AAssetManager serves Roblox's real files."
//
// Uses a real, small, hand-built zip fixture (not the real 100+MB Roblox
// APK, portable to CI/any machine, doesn't depend on the user having
// supplied an APK) containing an "assets/ssl/cacert.pem"-shaped entry, a
// nested-directory entry, and a non-assets entry that must NOT be
// extracted.

#include "stud/android_glue.h"
#include "stud/ndk_types.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace {

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        std::exit(1);
    }
    std::printf("ok: %s\n", what.c_str());
}

bool file_exists(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <fixture-apk-path> <scratch-dest-dir>\n", argv[0]);
        return 1;
    }
    std::string apk_path = argv[1];
    std::string dest_dir = argv[2];

    // Missing/corrupt APK -> throws clearly, not a crash.
    bool threw = false;
    try {
        stud::android_glue::extract_apk_assets(dest_dir + "/does_not_exist.apk", dest_dir);
    } catch (const stud::android_glue::ExtractError&) {
        threw = true;
    }
    check(threw, "extracting a nonexistent APK throws ExtractError instead of crashing");

    stud::android_glue::extract_apk_assets(apk_path, dest_dir);

    check(file_exists(dest_dir + "/ssl/cacert.pem"),
          "assets/ssl/cacert.pem extracted with the assets/ prefix stripped, nested dir created");
    check(file_exists(dest_dir + "/top_level.txt"), "a non-nested assets/ entry extracted correctly");
    check(!file_exists(dest_dir + "/AndroidManifest.xml"),
          "a non-assets/ entry (manifest) was correctly NOT extracted");
    check(!file_exists(dest_dir + "/classes.dex"), "dex files were correctly NOT extracted");

    // Real content check, not just presence.
    {
        std::ifstream f(dest_dir + "/ssl/cacert.pem");
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content == "-----BEGIN CERTIFICATE-----\nFAKE_BUT_REAL_BYTES\n-----END CERTIFICATE-----\n",
              "extracted cacert.pem's content matches the fixture's real bytes exactly");
    }

    // End-to-end through AAssetManager itself, not just raw filesystem
    // checks; this is the actual consumer.
    stud::android_glue::set_asset_base_directory(dest_dir);
    AAssetManager* mgr = AAssetManager_fromJava(nullptr, nullptr);
    AAsset* asset = AAssetManager_open(mgr, "ssl/cacert.pem", 0);
    check(asset != nullptr, "AAssetManager_open finds the real extracted cacert.pem");

    off_t len = AAsset_getLength(asset);
    check(len > 0, "AAsset_getLength reports the real extracted file's size");

    const void* buf = AAsset_getBuffer(asset);
    // 27 bytes, the literal string's own length, not counting its
    // implicit trailing NUL (the real file's 28th byte is '\n', not 0).
    check(buf != nullptr && std::memcmp(buf, "-----BEGIN CERTIFICATE-----", 27) == 0,
          "AAsset_getBuffer's zero-copy mapping shows the real cacert.pem bytes");

    AAsset_close(asset);

    // M8: real native-library extraction (lib/x86_64/<name>).
    std::string so_dest = dest_dir + "/extracted_libroblox.so";
    stud::android_glue::extract_apk_native_library(apk_path, "libroblox.so", so_dest);
    check(file_exists(so_dest), "extract_apk_native_library extracts the real lib/x86_64 entry");
    {
        std::ifstream f(so_dest);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content == "FAKE_ELF_BYTES_NOT_A_REAL_SHARED_OBJECT",
              "extracted native library content matches the x86_64 fixture exactly, not the "
              "arm64-v8a one");
    }

    threw = false;
    try {
        stud::android_glue::extract_apk_native_library(apk_path, "libDoesNotExist.so",
                                                         dest_dir + "/missing.so");
    } catch (const stud::android_glue::ExtractError&) {
        threw = true;
    }
    check(threw, "extracting a native library that doesn't exist in the APK throws ExtractError");

    // Split-APK bundles, in both of the shapes people actually download.
    //
    // .apkm/.apks name the base "base.apk"; .xapk names it after the
    // package and describes the set in a manifest.json. Both are treated
    // as bundles, and in both the base must be read first; every other
    // split is an overlay on it.
    // The bundle fixtures are built beside the plain APK.
    const std::string fixture_dir = apk_path.substr(0, apk_path.find_last_of('/'));
    for (const char* bundle : {"toy_bundle.apkm", "toy_bundle.xapk"}) {
        const std::string bundle_path = fixture_dir + "/" + bundle;
        const std::string out_dir = dest_dir + "/" + bundle;

        const auto sources = stud::android_glue::resolve_apk_sources_for_metadata(bundle_path);
        check(sources.size() >= 2,
              std::string(bundle) + " is recognised as a bundle, not read as one APK");
        const bool base_first = sources.front().find("arm64") == std::string::npos &&
                                sources.front().find("x86_64") == std::string::npos;
        check(base_first, std::string(bundle) + "'s base split is read first");

        // The library has to come out of the x86_64 split, and never out
        // of the arm64 one sitting beside it.
        const std::string so_out = out_dir + ".so";
        stud::android_glue::extract_apk_native_library(bundle_path, "libroblox.so", so_out);
        std::ifstream f(so_out);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content == "FAKE_ELF_BYTES_NOT_A_REAL_SHARED_OBJECT",
              std::string(bundle) + ": the x86_64 split's library is the one extracted");

        // Assets from every member, not just the base.
        stud::android_glue::extract_apk_assets(bundle_path, out_dir);
        check(std::ifstream(out_dir + "/from_base.txt").good(),
              std::string(bundle) + ": assets from the base split are extracted");
        check(std::ifstream(out_dir + "/from_split.txt").good(),
              std::string(bundle) + ": assets from a config split are extracted too");
    }

    std::printf("all apk-extract checks passed\n");
    return 0;
}
