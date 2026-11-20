#pragma once

// What every test here reports through. A failed check ends the test, so the
// first line of a failure is the one that matters.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

// ctest's own "skipped", for a test whose subject this machine doesn't have.
// Needs SKIP_RETURN_CODE in tests/CMakeLists.txt.
inline constexpr int kSkipped = 77;

inline void check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what.c_str());
        std::exit(1);
    }
    std::printf("ok: %s\n", what.c_str());
}

inline void check_eq(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " (got \"" + got + "\", wanted \"" + want + "\")");
}

// For a test that reports every failure rather than only the first: expect()
// as it goes, then return finish() from main.
inline int g_failures = 0;

inline void expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAILED: %s\n", what.c_str());
    ++g_failures;
}

inline int finish(const std::string& test) {
    if (g_failures == 0) std::printf("%s: ok\n", test.c_str());
    return g_failures == 0 ? 0 : 1;
}

// A scratch folder with nothing in it. These live in the build tree, and a
// file left by an earlier run would pass for this run's output.
inline void fresh_dir(const std::filesystem::path& dir) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
}
