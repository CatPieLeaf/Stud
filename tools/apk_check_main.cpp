// Says what a Roblox APK would need from Stud, without running it.
//
//     stud_apk_check <apk or bundle> [directories of the bundled .so files]
//
// Exits 1 if the APK cannot start under this Stud.

#include "stud/android_glue.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <apk or bundle> [bundled library directories]\n", argv[0]);
        return 2;
    }
    namespace glue = stud::android_glue;
    const std::string apk = argv[1];
    const std::string engine = glue::default_libroblox_cache_path() + ".check";
    try {
        glue::extract_apk_native_library(apk, "libroblox.so", engine);
        const glue::ApkCheckReport report =
            glue::check_apk(apk, engine, std::vector<std::string>(argv + 2, argv + argc));
        std::remove(engine.c_str());
        std::printf("Roblox %s, needs Android API %d\n", glue::apk_version_name(apk).c_str(),
                    glue::apk_min_sdk(apk));
        for (const std::string& line : report.blocking) std::printf("cannot start: %s\n", line.c_str());
        for (const std::string& line : report.warnings) std::printf("warning: %s\n", line.c_str());
        for (const std::string& line : report.notes) std::printf("note: %s\n", line.c_str());
        return report.blocking.empty() ? 0 : 1;
    } catch (const glue::ExtractError& e) {
        std::remove(engine.c_str());
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }
}
