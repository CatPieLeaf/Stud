#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Process-B bootstrap: launches Stud's runtime as a REAL bionic process,
// booted by a real, unmodified bionic linker64 (extracted from a Waydroid
// Android system image, see third_party/android-bionic/), inside an
// unprivileged bubblewrap (bwrap) sandbox that synthesizes /system/lib64
// and /system/bin/linker64 (paths real bionic's dynamic linker hardcodes,
// confirmed via a live syscall trace against the actual extracted linker64; see
// docs/bionic-process-b.md) while leaving the rest of the host filesystem
// (or the specific subtrees this process needs: GPU device nodes, the
// Wayland socket, DNS/TLS config, Stud's own install tree) visible.
//
// This replaces the old in-process design, which loaded bionic's libc.so
// into Stud's own glibc process and manually forged its private
// libc_shared_globals struct byte-for-byte (field offsets taken from one
// specific extracted binary, correct only for that exact build).
// That design required every glibc<->bionic call boundary (every JNI
// vtable slot, every pthread call) to manually swap the %fs segment
// register via stud::tls_compat, fragile by construction, since ANY
// thread anything in the shared process spawns (including driver-internal
// worker threads Stud can't enumerate) is exposed to the same TLS-
// ambiguity crash class. Booting a genuine, separate bionic process
// instead means bionic-compiled code's TLS is set up once, natively, by
// real bionic code, for every thread it ever has, there's no "other
// ABI" to swap to/from inside this process for bionic-native code at all.
//
// bwrap is a deliberate dependency, not hand-rolled pivot_root/mount-
// namespace code: it's a standard, already-hardened, widely-deployed
// unprivileged sandboxing tool (same category of dependency Flatpak-based
// sandboxing already is for other Roblox-on-Linux tools), and reimplementing
// its mount-namespace/pivot_root logic by hand would be substantial,
// unnecessary attack surface for an already-solved problem.
namespace stud::bionic_runtime {

struct BionicNotFound : std::runtime_error {
    BionicNotFound()
        : std::runtime_error(
              "stud: could not find Stud's own extracted bionic runtime files "
              "(libc.so/libm.so/libdl.so/liblog.so/libandroid.so/libc++.so/"
              "tzdata), extract them from a Waydroid system image first "
              "(see docs/bionic-process-b.md)") {}
};

struct LinkerNotFound : std::runtime_error {
    LinkerNotFound()
        : std::runtime_error(
              "stud: could not find a real bionic linker64 binary, extract "
              "it from a Waydroid system image's com.android.runtime APEX "
              "(system/apex/com.android.runtime.apex, apex_payload.img:/bin/"
              "linker64) first (see docs/bionic-process-b.md)") {}
};

struct BwrapNotFound : std::runtime_error {
    BwrapNotFound()
        : std::runtime_error("stud: bwrap (bubblewrap) is required to launch "
                              "Stud's bionic runtime process but was not found "
                              "on PATH, install the bubblewrap package") {}
};

// Directory containing the extracted real bionic .so set (see
// third_party/android-bionic/). Throws BionicNotFound if the expected
// files aren't present.
std::string default_shipped_bionic_directory();

// Path to the extracted real bionic linker64 binary within
// default_shipped_bionic_directory(). Throws LinkerNotFound if absent.
std::string default_linker64_path();

// One real host path to bind into the sandbox at the same absolute path
// it has outside the sandbox (e.g. "/dev/dri", the Wayland socket
// directory, third_party/angle (ANGLE built from source), the extracted Roblox APK
// directory). `writable` controls whether it's bound read-write or
// read-only; sockets/device nodes generally need read-write, static
// library/data trees don't.
struct HostBind {
    std::string path;
    bool writable = false;
};

struct ProcessBConfig {
    // The bionic ELF executable to run as Process B's entry point (a
    // real bionic build of stud-runtime, produced by the NDK toolchain
    // see runtime/CMakeLists.txt's bionic-target build).
    std::string executable_path;
    std::vector<std::string> args;

    // Extra real host paths Process B needs visible verbatim (GPU device
    // nodes, Wayland socket dir, stud-ipc's socket dir, ANGLE's built
    // .so set, the extracted Roblox APK / libroblox.so path, Stud's own
    // install tree). /system/lib64, /system/bin/linker64, /proc, and
    // /dev are always provided automatically and don't need to be listed
    // here.
    std::vector<HostBind> extra_binds;

    // Overrides for testing; default to
    // default_shipped_bionic_directory()/default_linker64_path().
    std::string bionic_lib_dir;
    std::string linker64_path;

    // Real bwrap `--chdir` target, must be one of extra_binds' writable
    // paths (or otherwise real and writable inside the sandbox). Real,
    // confirmed hazard if left empty: bwrap has no cwd of its own, so it
    // fchdir()s the child into whatever cwd the launching process (Process
    // A) happened to have, which is frequently one of the *read-only*
    // ro-binds (e.g. Stud's own install tree), and real Roblox code does
    // real relative-path writes (confirmed via a live syscall trace: a real, blocking
    // `openat("_memProfStorage2.json", O_CREAT|O_TRUNC, ...)` on one of
    // its own worker threads) that fail with EROFS there and can stall
    // real bring-up progress for way longer than any of this project's
    // own bounded waits, since it's Roblox's own internal retry/backoff
    // around the failure, not anything Stud arms a timeout on. Empty
    // string leaves bwrap's default (no explicit --chdir) behavior,
    // matching this field being new/optional.
    std::string working_directory;

    // Real, previously-missing gap: extra environment variables for
    // Process B, passed via bwrap's own real `--setenv NAME VALUE`.
    // Before this field existed, launch_process_b() had no way at all
    // to set anything like STUD_VULKAN_CALL_TRACE/STUD_RENDER_CALL_TRACE
    // for a real production launch; every env-gated diagnostic this
    // project has built stayed permanently off outside a hand-run
    // bwrap test script, so e.g. real evidence of whether libroblox.so
    // ever even attempts dlopen("libvulkan.so.1") was never actually
    // collected from a real end-to-end run.
    std::vector<std::pair<std::string, std::string>> extra_env;

    // The file descriptor Process B should use as its stdout and stderr,
    // or -1 to inherit this process's. Process A tees its own output into
    // the session log, which replaces its fd 1 with a pipe only its own
    // thread reads, and Process A exits long before Process B does, so
    // inheriting that pipe would throw away everything Process B ever
    // says. It opens the session log itself (stud/session_log.h).
    int stdout_fd = -1;

    bool share_network = true;
};

// Spawns Process B inside the bwrap sandbox described above and returns
// immediately with its PID (the caller. Process A / stud-ui, owns
// waiting on it and relaying lifecycle over stud-ipc). Throws
// BionicNotFound / LinkerNotFound / BwrapNotFound if a prerequisite is
// missing, or std::runtime_error if spawning itself fails (fork/exec
// failure), does NOT swallow-and-retry; a failure here is a real,
// actionable error for the caller to surface, not something to loop on.
pid_t launch_process_b(const ProcessBConfig& config);

// The real bwrap argv launch_process_b() builds and spawns, exposed as
// its own pure, side-effect-free function purely so it's testable
// without actually needing bwrap/a real bionic install present (a real
// gap this project's own plan flagged: nothing regression-tests that
// the sandbox never ends up binding a real host GPU driver path,
// exactly the class of mistake that would defeat this project's whole
// point, isolating the vendor driver from any process sharing bionic/
// foreign TLS). argv[0] is the bwrap binary path.
std::vector<std::string> build_process_b_argv(const ProcessBConfig& config,
                                               const std::string& bwrap_path,
                                               const std::string& bionic_lib_dir,
                                               const std::string& linker64_path);

}  // namespace stud::bionic_runtime
