// Every STUD_* variable the source reads has to appear in
// docs/environment.md.
//
// Documentation that is not checked stops being true. Stud reads over a
// hundred of these and they were, before this test, written down nowhere
// at all; the point of the document is that somebody investigating a
// problem can find the switch for it, and that only works if adding a
// switch and forgetting the document is a build failure rather than a
// silent omission.
//
// Whole names only: "STUD_DEBUG" is not documented by "STUD_DEBUG_LOG" being
// there, which is how a real variable once went unnoticed. Reads go through
// getenv and a few helpers; add a helper to the pattern when it's added.
//
// Deliberately one-directional: a variable in the source and not in the
// document fails. A name in the document that the source no longer reads
// does not, because the document also covers variables Stud only ever
// SETS for its children (STUD_GRAPHICS_MODE, STUD_TEXTURE_CACHE_MB) and
// ones read through other spellings.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The directories Stud's own code lives in. third_party/ and build/ are
// somebody else's and are skipped.
bool is_ours(const fs::path& p) {
    for (const auto& part : p) {
        const std::string s = part.string();
        if (s.rfind("build", 0) == 0 || s == "third_party" || s == ".git" || s == ".claude" ||
            s == ".flatpak-builder") {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // The source root, handed in by CMake so this does not depend on
    // where the test binary is run from.
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const fs::path doc = root / "docs" / "environment.md";

    if (!fs::exists(doc)) {
        std::fprintf(stderr, "env_documented_test: %s does not exist\n", doc.string().c_str());
        return 1;
    }
    const std::string doc_text = read_file(doc);
    const std::regex name_pattern("STUD_[A-Z0-9_]+");
    std::set<std::string> documented;
    for (std::sregex_iterator it(doc_text.begin(), doc_text.end(), name_pattern), end; it != end;
         ++it) {
        documented.insert(it->str());
    }

    const std::regex pattern(
        "(?:getenv|env_or_null|qEnvironmentVariable[A-Za-z]*|env\\.value|env\\.contains)"
        "\\((?:QStringLiteral\\()?\"(STUD_[A-Z0-9_]+)\"");
    std::set<std::string> missing;
    std::set<std::string> seen;

    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const fs::path& p = entry.path();
        if (!is_ours(fs::relative(p, root))) continue;
        const std::string ext = p.extension().string();
        if (ext != ".cpp" && ext != ".c" && ext != ".h" && ext != ".hpp") continue;

        const std::string text = read_file(p);
        for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
            const std::string name = (*it)[1].str();
            seen.insert(name);
            if (documented.count(name) == 0) missing.insert(name);
        }
    }

    if (seen.empty()) {
        std::fprintf(stderr,
                     "env_documented_test: found no STUD_* variables at all under %s, which means "
                     "this test is looking in the wrong place rather than that everything is "
                     "documented\n",
                     root.string().c_str());
        return 1;
    }

    if (!missing.empty()) {
        std::fprintf(stderr,
                     "env_documented_test: %zu environment variable(s) are read by the source and "
                     "missing from docs/environment.md:\n",
                     missing.size());
        for (const std::string& name : missing) std::fprintf(stderr, "    %s\n", name.c_str());
        return 1;
    }

    std::printf("env_documented_test: all %zu STUD_* variables are documented\n", seen.size());
    return 0;
}
