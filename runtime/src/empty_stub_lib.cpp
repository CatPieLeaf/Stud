// Deliberately empty. libroblox.so links libOpenSLES.so/libOpenMAXAL.so/
// libnativewindow.so as DT_NEEDED but imports zero symbols from any of
// them (confirmed via `the library's exported symbols --undefined-only` against the real,
// unmodified binary) -- real bionic's linker64 still requires the
// SONAME to resolve to *some* loadable ELF shared object during
// dlopen()'s dependency-graph walk, even though nothing in it is ever
// actually called. A real, sourced Waydroid-image implementation of
// these pulls in a deep, unrelated framework dependency chain (libmedia,
// libbinder, ICU, several AIDL HAL libraries) that isn't extractable
// unprivileged and isn't needed for anything Roblox's engine actually
// calls -- confirmed by testing the real implementations, genuinely
// empty stubs, and broken NDK link-only stubs side by side and seeing
// the exact same result each time for anything downstream of loading.
