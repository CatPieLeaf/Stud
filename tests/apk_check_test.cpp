// The APK check: its readers refuse what is not there, and its verdicts follow
// what the engine exports.

#include "stud/android_glue.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace glue = stud::android_glue;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

bool refused(const std::vector<uint8_t>& dex) {
    try {
        glue::DexMethods out;
        glue::read_dex_methods(dex, out);
    } catch (const glue::ExtractError&) {
        return true;
    }
    return false;
}

void put32(std::vector<uint8_t>& v, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(value >> (i * 8));
}

}  // namespace

int main(int, char** argv) {
    check(refused({}), "an empty file is not a DEX");
    check(refused({'d', 'e', 'x'}), "a short file is not a DEX");
    std::vector<uint8_t> lying(0x70, 0);
    lying[0] = 'd', lying[1] = 'e', lying[2] = 'x', lying[3] = '\n';
    put32(lying, 0x60, 1);           // one class
    put32(lying, 0x64, 0x7fffffff);  // far past the end
    check(refused(lying), "a table past the end of the file is refused");

    std::ifstream self(argv[0], std::ios::binary);
    const std::vector<uint8_t> elf(std::istreambuf_iterator<char>(self), {});
    const glue::ElfSymbols symbols = glue::read_elf_symbols(elf);
    check(symbols.imported.count("fflush") != 0 || symbols.imported.count("exit") != 0,
          "an ELF file's imports are read");

    // An engine with nothing: every required entry point is missing.
    glue::ElfSymbols empty;
    const glue::ApkCheckReport none = glue::check_apk_contents({}, empty, {}, nullptr);
    check(!none.blocking.empty(), "a missing required entry point blocks");

    // A method Stud calls, with a second overload beside the one it expects.
    const std::string method = "com/roblox/engine/jni/NativeGLInterface.nativeInitClientSettings";
    const std::string real = "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I";
    const auto mentions = [](const glue::ApkCheckReport& r, const char* text) {
        for (const std::string& line : r.blocking) {
            if (line.find(text) != std::string::npos) return true;
        }
        return false;
    };
    glue::ElfSymbols engine;
    engine.defined.insert("Java_com_roblox_engine_jni_NativeGLInterface_nativeInitClientSettings");
    glue::DexMethods overloaded;
    overloaded.native_signatures[method] = {"()V", real};
    check(!mentions(glue::check_apk_contents(overloaded, engine, {}, nullptr), "changed from"),
          "an overload beside the expected signature is no change");
    overloaded.native_signatures[method] = {"()V"};
    check(mentions(glue::check_apk_contents(overloaded, engine, {}, nullptr), "changed from"),
          "a signature that has gone blocks");

    std::set<std::string> provided = {"memcpy"};
    empty.imported = {"memcpy", "brand_new_libc_call"};
    const glue::ApkCheckReport libc = glue::check_apk_contents({}, empty, {}, &provided);
    bool named = false;
    for (const std::string& line : libc.blocking) {
        named = named || line.find("brand_new_libc_call") != std::string::npos;
    }
    check(named, "an import nothing provides is named");
    return 0;
}
