#pragma once

#include <dlfcn.h>

#include <stdexcept>
#include <string>
#include <string_view>

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

class LoadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class LoadedLibrary {
public:
    explicit LoadedLibrary(void* handle) : handle_(handle) {}

    void* find_symbol(std::string_view name) const {
        return ::dlsym(handle_, std::string(name).c_str());
    }

private:
    void* handle_;
};

}  // namespace stud::linker
