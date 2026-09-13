// M7 test: real APK asset extraction (android-glue/src/apk_extract.cpp),
// and that AAssetManager correctly serves an extracted file end-to-end --
// closing the gap between "AAssetManager works" (tested with synthetic
// data elsewhere) and "AAssetManager serves Roblox's real files."
//
// Uses a real, small, hand-built zip fixture (not the real 100+MB Roblox
// APK -- portable to CI/any machine, doesn't depend on the user having
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

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
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
    // checks -- this is the actual consumer.
    stud::android_glue::set_asset_base_directory(dest_dir);
    AAssetManager* mgr = AAssetManager_fromJava(nullptr, nullptr);
    AAsset* asset = AAssetManager_open(mgr, "ssl/cacert.pem", 0);
    check(asset != nullptr, "AAssetManager_open finds the real extracted cacert.pem");

    off_t len = AAsset_getLength(asset);
    check(len > 0, "AAsset_getLength reports the real extracted file's size");

    const void* buf = AAsset_getBuffer(asset);
    // 27 bytes -- the literal string's own length, not counting its
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

    std::printf("all apk-extract checks passed\n");
    return 0;
}
