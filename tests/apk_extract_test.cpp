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
#include "check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <miniz.h>
#include <miniz_zip.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

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
    fresh_dir(dest_dir);
    std::filesystem::remove(dest_dir + "/../escaped.txt");
    // Bundles unpack under this; never the real one of whoever runs the test.
    ::setenv("XDG_STATE_HOME", (dest_dir + "/state").c_str(), 1);

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
    check(!file_exists(dest_dir + "/../escaped.txt"),
          "an assets/../ entry was refused instead of written outside dest_dir");

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

    // The extraction directory is writable from the sandbox, so a symlink
    // planted at the destination must be replaced, never written through.
    {
        const std::string victim = dest_dir + "/symlink_victim";
        const std::string planted = dest_dir + "/planted_libroblox.so";
        std::ofstream(victim) << "untouched";
        ::unlink(planted.c_str());
        check(::symlink(victim.c_str(), planted.c_str()) == 0, "symlink planted");
        stud::android_glue::extract_apk_native_library(apk_path, "libroblox.so", planted);
        std::ifstream f(victim);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        check(content == "untouched", "extraction does not write through a planted symlink");
        struct stat info {};
        check(::lstat(planted.c_str(), &info) == 0 && S_ISREG(info.st_mode),
              "the planted symlink is replaced by a real file");
    }

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

    // Only the bundle in use stays unpacked.
    {
        const std::string keep = fixture_dir + "/toy_bundle.apkm";
        const auto kept = stud::android_glue::resolve_apk_sources_for_metadata(keep);
        const auto gone =
            stud::android_glue::resolve_apk_sources_for_metadata(fixture_dir + "/toy_bundle.xapk");
        stud::android_glue::keep_only_apk_bundle(keep);
        check(file_exists(kept.front()), "the bundle in use stays unpacked");
        check(!file_exists(gone.front()), "any other bundle is removed");
        stud::android_glue::keep_only_apk_bundle(apk_path);
        check(!file_exists(kept.front()), "a plain APK leaves no bundle behind");
    }

    // A folder of splits, as gplaydl leaves it: the base named after the
    // package, a feature split that looks just as base-like by name, the
    // x86_64 split, a split for another ABI and a stray metadata file.
    {
        const std::string folder = dest_dir + "/split_folder";
        fresh_dir(folder);
        auto make_apk = [&](const std::string& name, const char* entry, const char* text) {
            mz_zip_archive zip{};
            const std::string path = folder + "/" + name;
            check(mz_zip_writer_init_file(&zip, path.c_str(), 0) &&
                      mz_zip_writer_add_mem(&zip, entry, text, std::strlen(text), MZ_NO_COMPRESSION) &&
                      mz_zip_writer_finalize_archive(&zip),
                  "a split for the folder fixture is written: " + name);
            mz_zip_writer_end(&zip);
        };
        make_apk("com.roblox.client-1-pgs.apk", "res/feature.txt", "feature");
        make_apk("com.roblox.client-1.apk", "assets/ssl/cacert.pem", "certificates");
        make_apk("com.roblox.client-1-config.x86_64.apk", "lib/x86_64/libroblox.so", "engine");
        make_apk("com.roblox.client-1-config.arm64_v8a.apk", "lib/arm64-v8a/libroblox.so", "arm");
        std::ofstream(folder + "/metadata.json") << "{}";

        const std::string bundle = dest_dir + "/folder.apks";
        stud::android_glue::bundle_split_folder(folder, bundle);
        mz_zip_archive zip{};
        check(mz_zip_reader_init_file(&zip, bundle.c_str(), 0), "the folder's bundle opens as a zip");
        check(mz_zip_reader_locate_file(&zip, "base.apk", nullptr, 0) >= 0,
              "the split with the app's assets becomes base.apk");
        check(mz_zip_reader_locate_file(&zip, "split_com.roblox.client-1-config.x86_64.apk", nullptr, 0) >= 0,
              "the x86_64 split is in the bundle");
        check(mz_zip_reader_locate_file(&zip, "split_com.roblox.client-1-pgs.apk", nullptr, 0) >= 0,
              "the base-looking feature split is in, as a split");
        check(mz_zip_reader_locate_file(&zip, "split_com.roblox.client-1-config.arm64_v8a.apk", nullptr, 0) < 0,
              "the other ABI's split is left out");
        check(mz_zip_reader_get_num_files(&zip) == 3, "nothing but the three splits is in the bundle");
        mz_zip_reader_end(&zip);
        check(stud::android_glue::apk_has_asset(bundle, "ssl/cacert.pem"),
              "the bundle reads like any .apks: the base's assets are found");
        const std::string so = dest_dir + "/folder_libroblox.so";
        stud::android_glue::extract_apk_native_library(bundle, "libroblox.so", so);
        check(file_exists(so), "and the engine comes out of its x86_64 split");

        // No x86_64 split: refused, and no bundle left behind.
        std::filesystem::remove(folder + "/com.roblox.client-1-config.x86_64.apk");
        const std::string refused = dest_dir + "/refused.apks";
        bool no_engine = false;
        try {
            stud::android_glue::bundle_split_folder(folder, refused);
        } catch (const stud::android_glue::ExtractError&) {
            no_engine = true;
        }
        check(no_engine && !file_exists(refused) && !file_exists(refused + ".part"),
              "a folder with no x86_64 split is refused and leaves nothing behind");
    }

    std::printf("all apk-extract checks passed\n");
    return 0;
}
