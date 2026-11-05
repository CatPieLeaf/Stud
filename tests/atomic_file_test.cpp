// A save that fails must leave the last good file alone. SharedPreferences
// holds the engine's flag cache state, and losing it costs a session its
// graphics flags.

#include "stud/atomic_file.h"

#include "check.h"

#include <sys/resource.h>

#include <csignal>
#include <fstream>
#include <iterator>

namespace {

std::string read_all(const std::string& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    using stud::jni_bridge::write_file_atomically;
    const std::filesystem::path dir = argv[1];
    fresh_dir(dir);
    const std::string path = (dir / "prefs.json").string();

    check(write_file_atomically(path, "first"), "a new file is written");
    check_eq(read_all(path), "first", "with its contents");
    check(write_file_atomically(path, "second"), "an existing one is replaced");
    check_eq(read_all(path), "second", "whole");

    // The disk refuses: a process over its file size limit gets EFBIG, and
    // only once the buffered text is really written, at the close.
    std::signal(SIGXFSZ, SIG_IGN);
    rlimit original{};
    check(::getrlimit(RLIMIT_FSIZE, &original) == 0, "the file size limit reads");
    rlimit none = original;
    none.rlim_cur = 0;
    check(::setrlimit(RLIMIT_FSIZE, &none) == 0, "the file size limit is set to nothing");
    const bool wrote = write_file_atomically(path, "third, which the disk refuses");
    check(::setrlimit(RLIMIT_FSIZE, &original) == 0, "the file size limit is restored");

    check(!wrote, "a refused write says so");
    check_eq(read_all(path), "second", "and the last good file is untouched");
    size_t files = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(dir)) ++files;
    check(files == 1, "and no temp file is left behind");

    check(!write_file_atomically((dir / "no" / "such" / "folder").string(), "x"),
          "a folder that isn't there is a failure, not a crash");
    return 0;
}
