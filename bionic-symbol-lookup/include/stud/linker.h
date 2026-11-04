#pragma once

#include <dlfcn.h>

#include <vector>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

#include "stud/session_log.h"

// Minimal, real-dlopen-backed drop-in for jni-bridge's own use of
// stud::linker::LoadedLibrary/LoadError (find_symbol() only, jni-bridge
// never calls a full ELF-loading API). The old, from-scratch ELF64
// loader this used to stand in for (linker/, plus its tls-compat/
// patch-engine dependencies) was deleted this session along with the
// rest of the old dual-ABI-same-process design, neither Process B nor
// the host (glibc) build needs Stud's own ELF parser to load anything
// anymore: real bionic's own linker64 (execve()'s PT_INTERP) loads
// libroblox.so in Process B, and ordinary host dlopen() loads whatever
// the host (glibc) build needs, both reached the same way any real
// caller reaches a loaded library's exports: dlsym() against the handle
// dlopen() returned. Shared between jni-bridge's host build and
// process-b's bionic build (both need the exact same LoadedLibrary/
// LoadError API, just backed by their own respective platform's real
// dlopen/dlsym).
namespace stud::linker {

// A required engine symbol is missing, the likeliest sign of a Roblox this
// Stud does not know, so every one of them is noted where the UI can see it.
class LoadError : public std::runtime_error {
public:
    explicit LoadError(const std::string& what) : std::runtime_error(what) {
        stud::logging::note_unsupported_roblox(what);
    }
};

class LoadedLibrary {
public:
    explicit LoadedLibrary(void* handle) : handle_(handle) {}

    void* find_symbol(std::string_view name) const {
        const std::string symbol(name);
        void* addr = ::dlsym(handle_, symbol.c_str());
        if (addr == nullptr) note_missing(symbol);
        return addr;
    }

    // What Roblox would call this entry point after bumping it: the last
    // number in the method's name up by one, or a first 2 or V2 on a name
    // with none. An overload's mangled name has no successor to guess.
    static std::vector<std::string> next_names(const std::string& symbol) {
        if (symbol.find("__") != std::string::npos) return {};
        const size_t method = symbol.rfind('_') + 1;
        size_t end = symbol.size();
        while (end > method && !std::isdigit(static_cast<unsigned char>(symbol[end - 1]))) --end;
        if (end == method) return {symbol + "2", symbol + "V2"};
        size_t begin = end;
        while (begin > method && std::isdigit(static_cast<unsigned char>(symbol[begin - 1]))) --begin;
        const std::string digits = symbol.substr(begin, end - begin);
        return {symbol.substr(0, begin) + std::to_string(std::stoul(digits) + 1) +
                symbol.substr(end)};
    }

private:
    // An engine entry point this build does not export, named once, the
    // first time anything looks for it. Every JNI export Stud calls is
    // looked up through here, so a Roblox update that renames or drops one
    // (a V2 becoming V3) shows up by name on its first launch instead of as
    // a feature that silently stopped working. A bumped name is pointed out
    // and not followed: a bump usually means different arguments.
    void note_missing(const std::string& symbol) const {
        if (symbol.rfind("Java_", 0) != 0 && symbol.rfind("JNI_", 0) != 0) return;
        static std::mutex mutex;
        static std::set<std::string> reported;
        std::lock_guard<std::mutex> lock(mutex);
        if (!reported.insert(symbol).second) return;
        std::string successor;
        for (std::vector<std::string> next = next_names(symbol); !next.empty();) {
            const std::string name = next.front();
            next.erase(next.begin());
            if (::dlsym(handle_, name.c_str()) == nullptr) continue;
            successor = name;
            next = next_names(name);
        }
        if (successor.empty()) {
            std::printf("stud: !!! this Roblox build does not export %s\n", symbol.c_str());
        } else {
            std::printf("stud: !!! this Roblox build does not export %s; it has %s, which Stud "
                        "does not call\n",
                        symbol.c_str(), successor.c_str());
        }
        std::fflush(stdout);
    }

    void* handle_;
};

}  // namespace stud::linker
