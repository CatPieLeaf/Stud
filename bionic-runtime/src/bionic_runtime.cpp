#include "stud/bionic_runtime.h"
#include "stud/property_area.h"

#include <sched.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

extern char** environ;

namespace stud::bionic_runtime {

namespace {

namespace fs = std::filesystem;

bool file_readable(const std::string& path) {
    return ::access(path.c_str(), R_OK) == 0;
}

bool file_executable(const std::string& path) {
    return ::access(path.c_str(), X_OK) == 0;
}

// Where a real, extracted bionic can be. In order:
//
//   1. STUD_BIONIC_DIR, so a machine that keeps it elsewhere (or a
//      packaged build) needs no rebuild to say so.
//   2. The repo's own third-party directory, which is where tools/
//      setup.sh puts it and what STUD_THIRD_PARTY_DIR compiles in.
//
// It is not vendored and never will be: it comes out of a real Android
// system image, which Stud has no right to redistribute. See
// tools/setup.sh.
// Where the running executable lives, so an installed or bundled Stud
// finds its own files without being told. An AppImage is mounted at a
// different path every run, so nothing may be absolute.
std::string own_dir() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    std::string exe(buf);
    const auto slash = exe.find_last_of('/');
    return slash == std::string::npos ? std::string{} : exe.substr(0, slash);
}

std::vector<std::string> bionic_dir_candidates() {
    std::vector<std::string> out;
    if (const char* env = std::getenv("STUD_BIONIC_DIR")) {
        if (*env != '\0') out.emplace_back(env);
    }
    // An install tree, relative to whichever binary is asking:
    // <prefix>/bin/stud and <prefix>/libexec/stud/... both reach
    // <prefix>/lib/stud/android-bionic from here.
    const std::string dir = own_dir();
    if (!dir.empty()) {
        out.emplace_back(dir + "/../lib/stud/android-bionic");
        out.emplace_back(dir + "/../../lib/stud/android-bionic");
    }
#ifdef STUD_THIRD_PARTY_DIR
    out.emplace_back(std::string(STUD_THIRD_PARTY_DIR) + "/android-bionic");
#endif
    return out;
}

// What genuinely has to come from a real Android, and nothing more.
//
// libandroid.so used to be on this list and is deliberately not any
// more: Stud builds its own and binds it over /system/lib64 inside the
// sandbox, so the copy here was never the one that got loaded. Requiring
// it ruled out AOSP's own Runtime APEX, which is bionic proper and
// carries no framework libraries at all.
bool is_real_bionic_dir(const std::string& dir) {
    return !dir.empty() && file_readable(dir + "/libc.so") && file_readable(dir + "/libm.so") &&
           file_readable(dir + "/libdl.so") && file_readable(dir + "/libdl_android.so") &&
           file_readable(dir + "/liblog.so") && file_readable(dir + "/libc++.so") &&
           file_readable(dir + "/tzdata");
}

std::string find_bwrap_on_path() {
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return {};
    std::string path(path_env);
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t sep = path.find(':', start);
        std::string dir = path.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/bwrap";
            if (file_executable(candidate)) return candidate;
        }
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return {};
}

}  // namespace

std::string default_shipped_bionic_directory() {
    for (const std::string& dir : bionic_dir_candidates()) {
        if (is_real_bionic_dir(dir)) return dir;
    }
    throw BionicNotFound();
}

// XDG-scoped path for the generated DNS property area (see
// property_area.h's own doc comment for the real, ground-truth-traced
// mechanism this backs), same convention as stud-ipc's own launch
// socket path.
std::string default_property_area_path() {
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        return std::string(xdg_runtime_dir) + "/stud/dns_property_area";
    }
    return "/tmp/stud-dns-property-area";
}

// Minimal /etc/resolv.conf parse, just real "nameserver <ip>"
// lines, in the real order they appear, up to the two real properties
// (net.dns1/net.dns2) this project currently has any use for. No other
// resolv.conf directive (search/options/etc.) matters for this.
std::vector<std::string> real_host_nameservers() {
    std::vector<std::string> servers;
    std::ifstream in("/etc/resolv.conf");
    std::string line;
    while (servers.size() < 2 && std::getline(in, line)) {
        std::istringstream iss(line);
        std::string keyword, value;
        if ((iss >> keyword >> value) && keyword == "nameserver") {
            servers.push_back(value);
        }
    }
    return servers;
}

// Best-effort generation of the DNS property area from the host's
// own real resolver config; see property_area.h's own doc comment for
// why this exists (real, live-traced: bionic's DNS resolver reads
// net.dns1/net.dns2 via __system_property_get(), not /etc/resolv.conf
// as a plain text file, so bind-mounting resolv.conf alone; real and
// still worth doing, see build_process_b_argv(), isn't sufficient by
// itself). Returns false (and leaves no stale file behind) if the host
// has no real nameservers configured, or the file couldn't be written,
// honest degradation, matching every other "optional host file" pattern
// in this file, not a hard failure.
bool generate_property_area_if_possible() {
    std::vector<std::string> servers = real_host_nameservers();
    std::string path = default_property_area_path();
    if (servers.empty()) {
        std::error_code ec;
        fs::remove(path, ec);
        return false;
    }
    std::vector<std::pair<std::string, std::string>> props;
    props.emplace_back("net.dns1", servers[0]);
    if (servers.size() > 1) {
        props.emplace_back("net.dns2", servers[1]);
    }
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    return stud::bionic_runtime::write_property_area(path, props);
}

std::string default_linker64_path() {
    std::string linker64 = default_shipped_bionic_directory() + "/linker64";
    if (file_executable(linker64)) return linker64;
    throw LinkerNotFound();
}

// Whether this process can create a user namespace at all, the one
// thing bwrap cannot do without, and the one thing a Flatpak refuses.
//
// Measured directly rather than inferred from being in a Flatpak: the
// host allows nesting (`unshare -Ur, unshare -Ur true` succeeds), while
// the same call inside Stud's own Flatpak returns EPERM even though the
// app carries --allow=devel and the kernel's own limits are untouched
// (user.max_user_namespaces reads 2147483647 in there). What refuses it
// is the seccomp filter Flatpak installs on every app; org.flatpak.Builder
// behaves identically, so this is a property of Flatpak rather than of
// anything in this manifest. bwrap's own diagnosis blames the kernel.
// "No permissions to creating new namespace, likely because the kernel
// does not allow non-privileged user namespaces", which is what a
// Flatpak launch printed while Process B never started and no Roblox
// window ever appeared.
//
// The probe is a fork whose child does nothing but the syscall, so the
// answer costs one process and cannot disturb this one.
bool can_create_user_namespace() {
    static const bool available = [] {
        pid_t pid = ::fork();
        if (pid < 0) return false;
        if (pid == 0) {
            ::_exit(::unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
        }
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }();
    return available;
}

// One symlink, replacing whatever is already at that path.
bool place_link(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directories(link.parent_path(), ec);
    fs::remove(link, ec);
    fs::create_symlink(target, link, ec);
    return !ec;
}

// The same tree build_process_b_argv() mounts, built with symlinks
// instead, for the case where there is no mount namespace to mount it
// in (see can_create_user_namespace()).
//
// This is only reachable where the root filesystem is writable, which on
// an ordinary desktop it is not, and there bwrap works, so this path is
// never taken. Inside a Flatpak "/" is a per-instance tmpfs owned by the
// user (`tmpfs rw,nosuid,nodev,uid=1000`), private to this sandbox, so
// creating /system there affects nothing outside it and disappears with
// the app.
//
// The one thing it cannot reproduce is the sandbox's uid 0: bionic's
// prop_area::map_fd_ro() requires the property file to be owned by root,
// which bwrap arranged by remapping this user to 0 inside its own user
// namespace. Without that namespace the file stays user-owned and
// __system_properties_init() returns -1, honest degradation, and
// nothing in Stud's own DNS path depends on it (the resolver is set up
// through _resolv_set_nameservers_for_net and ANDROID_DNS_MODE, both
// still done below).
void link_android_system_tree(const ProcessBConfig& config,
                             const std::string& bionic_lib_dir,
                             const std::string& linker64_path) {
    std::error_code ec;
    for (const char* dir : {"/system/lib64", "/system/bin", "/system/etc",
                            "/system/usr/share/zoneinfo"}) {
        fs::create_directories(dir, ec);
        if (ec) {
            throw std::runtime_error(
                "stud: this process cannot create user namespaces and cannot write " +
                std::string(dir) + " either, so there is no way to give Process B the "
                "/system tree bionic's linker requires: " + ec.message());
        }
    }

    // The overlay's own names win, exactly as they do in the bind list.
    std::set<std::string> overlay_names;
    const std::string overlay_lib64_dir =
        fs::path(config.executable_path).parent_path().string() + "/lib64";
    if (fs::is_directory(overlay_lib64_dir)) {
        for (const auto& entry : fs::directory_iterator(overlay_lib64_dir, ec)) {
            const std::string filename = entry.path().filename().string();
            if (filename.find(".so") != std::string::npos) overlay_names.insert(filename);
        }
    }
    for (const auto& entry : fs::directory_iterator(bionic_lib_dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string filename = entry.path().filename().string();
        if (overlay_names.count(filename) != 0) continue;
        place_link(entry.path(), fs::path("/system/lib64") / filename);
    }
    if (fs::is_directory(overlay_lib64_dir)) {
        for (const auto& entry : fs::directory_iterator(overlay_lib64_dir, ec)) {
            const std::string filename = entry.path().filename().string();
            if (filename.find(".so") == std::string::npos) continue;
            place_link(entry.path(), fs::path("/system/lib64") / filename);
        }
    }

    place_link(linker64_path, "/system/bin/linker64");
    place_link(linker64_path, "/system/lib64/linker64");
    place_link(bionic_lib_dir + "/tzdata", "/system/usr/share/zoneinfo/tzdata");
    if (fs::exists("/etc/hosts")) place_link("/etc/hosts", "/system/etc/hosts");
    if (fs::exists(default_property_area_path())) {
        place_link(default_property_area_path(), "/dev/__properties__");
    }
}

// Process B's own environment, for the same launch: what bwrap would
// have set with --setenv, applied to a copy of this process's own.
std::vector<std::string> process_b_environment(const ProcessBConfig& config,
                                               const std::string& ca_bundle_path) {
    std::map<std::string, std::string> env;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const char* eq = std::strchr(*e, '=');
        if (eq == nullptr) continue;
        env[std::string(static_cast<const char*>(*e), eq)] = std::string(eq + 1);
    }
    // See build_process_b_argv()'s own comments for why each of these is
    // not optional.
    env["ANDROID_DNS_MODE"] = "local";
    if (!ca_bundle_path.empty()) env["SSL_CERT_FILE"] = ca_bundle_path;
    for (const auto& [name, value] : config.extra_env) env[name] = value;

    std::vector<std::string> flat;
    flat.reserve(env.size());
    for (const auto& [name, value] : env) flat.push_back(name + "=" + value);
    return flat;
}

std::vector<std::string> build_process_b_argv(const ProcessBConfig& config,
                                               const std::string& bwrap_path,
                                               const std::string& bionic_lib_dir,
                                               const std::string& linker64_path) {
    // Real bionic linker64's own hardcoded namespace-fallback search order
    // (confirmed via a live syscall trace against the actual extracted binary, not
    // assumed from AOSP source of an unconfirmed vintage; see
    // docs/bionic-process-b.md): with no /system/etc/ld.config.txt present,
    // it tries /system/lib64, /odm/lib64, /vendor_extra/lib64, /vendor/
    // lib64 in that order for every needed library. Only /system/lib64
    // needs to actually exist; the others are allowed to (and, in this
    // sandbox, do) come back ENOENT.
    std::vector<std::string> argv_storage;
    argv_storage.push_back(bwrap_path);
    argv_storage.push_back("--tmpfs");
    argv_storage.push_back("/");
    // Fix from testing (was `--dev-bind /dev /dev`, the whole real
    // host devtmfs): a real syscall trace of a full, working run showed
    // Process B itself (the real bionic linker64 process specifically,
    // isolated from Process C/stud-render-host's own, separate, much
    // larger real GPU device access, confirmed live: 155 real
    // /dev/nvidia*+/dev/dri/* opens all came from render-host's own
    // PID, zero from Process B's) only ever touches /dev/urandom and
    // /dev/pmsg0. It never touches the GPU directly at all, exactly
    // matching this project's own real architecture goal (the vendor
    // driver never runs inside a process sharing bionic/foreign TLS).
    // `--dev /dev` (bwrap's own synthetic minimal /dev: null/zero/full/
    // random/urandom/tty/ptmx/pts/shm) covers /dev/urandom for free and,
    // critically, stays a real bwrap-managed tmpfs rather than a live
    // bind of the actual host devtmfs, which is what makes a new
    // synthetic entry (the DNS property area bound in below) creatable
    // under it at all (a real, live-caught blocker with the old
    // --dev-bind /dev /dev: "Permission denied" trying to create a new
    // mount point under the real, root-owned host devtmfs). /dev/pmsg0
    // (a real Android pstore/persistent-log device, not part of bwrap's
    // synthetic set, and not present on this non-Android host at all)
    // is bound best-effort via --dev-bind-try; real, tolerates
    // absence, matching every other optional-host-path pattern in this
    // file.
    argv_storage.push_back("--dev");
    argv_storage.push_back("/dev");
    argv_storage.push_back("--dev-bind-try");
    argv_storage.push_back("/dev/pmsg0");
    argv_storage.push_back("/dev/pmsg0");
    argv_storage.push_back("--proc");
    argv_storage.push_back("/proc");

    // Real /sys, read-only. The sandbox root is a --tmpfs, so /sys did
    // not exist inside it at all, and that is how many CPUs the engine
    // thinks this machine has. Android's own CPU detection reads
    // /sys/devices/system/cpu/{possible,present,online}; with none of
    // them readable the count comes back as 1, and the engine sizes its
    // whole TaskScheduler from it.
    //
    // Live-measured against a real Sober session on this same machine:
    //   Sober:  Shader Loading Threads: 8 (TaskScheduler: 16, ...)
    //   Stud:   Shader Loading Threads: 0 (TaskScheduler: 1,  ...)
    // A one-thread scheduler is not merely slow: the start-game task
    // submitted by UgcExperienceController::submitStartGameTask never
    // ran at all, so the game DataModel was never built
    // (`stepDataModelJob: No DM yet` forever) and no NetworkClient was
    // ever created.
    //
    // Read-only, and it grants no device access; that lives in /dev,
    // which stays a synthetic bwrap tmpfs above, so the "vendor GPU
    // driver never runs inside Process B" constraint is untouched.
    argv_storage.push_back("--ro-bind-try");
    argv_storage.push_back("/sys");
    argv_storage.push_back("/sys");

    // /system/lib64 is a tmpfs Stud fills file by file, NOT a bind of
    // the bionic directory.
    //
    // It used to be `--bind bionic_lib_dir /system/lib64`, and the
    // overlay below then created its own mount points inside that bind.
    // Creating a mount point is a write, so that only ever worked
    // because the bionic directory happened to be one this user owns:
    // true of a build tree, false of an installed package, where it is
    // root-owned and the sandbox's uid 0 is this unprivileged user. The
    // real symptom was `bwrap: Can't create file /system/lib64/
    // libEGL.so: Permission denied` and Process B never starting at all,
    // from every package while the build tree worked perfectly.
    //
    // A tmpfs is writable regardless of who owns what on the host, so
    // this works the same from a build tree, an rpm, a deb or an
    // AppImage, and every real library under it is now read-only,
    // which the whole-directory bind was not.
    argv_storage.push_back("--tmpfs");
    argv_storage.push_back("/system/lib64");

    // The overlay's own names win, so they are bound instead of (not on
    // top of) the bionic file of the same name, one mount per
    // destination, rather than relying on mount stacking order.
    std::set<std::string> overlay_names;
    const std::string overlay_lib64_dir =
        fs::path(config.executable_path).parent_path().string() + "/lib64";
    if (fs::is_directory(overlay_lib64_dir)) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(overlay_lib64_dir, ec)) {
            const std::string filename = entry.path().filename().string();
            if (filename.find(".so") != std::string::npos) overlay_names.insert(filename);
        }
    }

    {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(bionic_lib_dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const std::string filename = entry.path().filename().string();
            if (overlay_names.count(filename) != 0) continue;
            argv_storage.push_back("--ro-bind");
            argv_storage.push_back(entry.path().string());
            argv_storage.push_back("/system/lib64/" + filename);
        }
    }

    argv_storage.push_back("--ro-bind");
    argv_storage.push_back(linker64_path);
    argv_storage.push_back("/system/bin/linker64");

    // Real bionic tzdata lookup path, confirmed via strings in the
    // extracted libc.so itself (__bionic_open_tzdata_path): tries
    // /apex/com.android.tzdata/etc/tz/tzdata first, then falls back to
    // this path. No /apex in this sandbox, so this is the one that
    // must resolve. Needed for anything touching java.util.TimeZone-
    // adjacent native code; its absence was a real, confirmed crash
    // cause earlier in this project's history.
    argv_storage.push_back("--dir");
    argv_storage.push_back("/system/usr/share/zoneinfo");
    argv_storage.push_back("--ro-bind");
    argv_storage.push_back(bionic_lib_dir + "/tzdata");
    argv_storage.push_back("/system/usr/share/zoneinfo/tzdata");

    // Process B's own build output (libEGL.so/libGLESv2.so render-client
    // stubs, libandroid.so/libmediandk.so android-glue implementations,
    // and the empty libOpenSLES.so/libOpenMAXAL.so/libnativewindow.so
    // dead-link stand-ins; see process-b/CMakeLists.txt's own doc
    // comments) lives in a lib64/ directory sibling to executable_path,
    // and must overlay bionic_lib_dir's /system/lib64 bind above (real
    // bionic's linker64 checks /system/lib64 first in its fallback
    // search order, so these take priority over anything of the same
    // name that happens to exist in bionic_lib_dir). Bound file-by-file,
    // not as a whole-directory bind, so it layers on top of the
    // directory already bound at this mountpoint rather than replacing
    // it.
    {
        if (fs::is_directory(overlay_lib64_dir)) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(overlay_lib64_dir, ec)) {
                // ".so" anywhere in the name, not extension() == ".so".
                //
                // Bug found in testing: libvulkan.so.1's extension is
                // ".1", so it was the one file in this directory the
                // overlay silently skipped; Stud's real Vulkan library
                // was never bound into the sandbox at all. What the
                // engine found instead was the ZERO-BYTE libvulkan.so.1
                // that ships in the bionic lib dir, so dlopen failed with
                // `file offset for the library "libvulkan.so.1" >= file
                // size: 0 >= 0`, the engine logged "Mode 6 failed: Unable
                // to load Vulkan API", and every run fell back to the
                // glsles3 shader pack (whose terrain and part shaders
                // then fail to compile under ANGLE). Versioned sonames
                // are ordinary for real libraries; the filter has to
                // handle them.
                const std::string filename = entry.path().filename().string();
                if (filename.find(".so") == std::string::npos) continue;
                argv_storage.push_back("--ro-bind");
                argv_storage.push_back(entry.path().string());
                argv_storage.push_back("/system/lib64/" + entry.path().filename().string());
            }
        }
    }

    // The executable itself and its containing directory, made visible at
    // the identical absolute path inside the sandbox as outside it,
    // same transparent-passthrough convention as extra_binds below, so
    // relative/logged paths mean the same thing on both sides.
    {
        std::string exe_dir = fs::path(config.executable_path).parent_path().string();
        if (!exe_dir.empty()) {
            argv_storage.push_back("--ro-bind");
            argv_storage.push_back(exe_dir);
            argv_storage.push_back(exe_dir);
        }
    }

    // Gap found in testing: this sandbox's root is `--tmpfs /`, so
    // /etc/resolv.conf genuinely doesn't exist inside it at all,
    // confirmed via a real syscall trace capture showing bionic's own DNS
    // resolver worker connect()ing to a broken 0.0.0.0 "nameserver"
    // (its real fallback when it can't find real resolver config) and
    // exiting immediately, and separately trying to open
    // "/system/etc/hosts" (bionic's own real hosts-file search path,
    // distinct from glibc's /etc/hosts) and getting ENOENT. This means
    // ANY real hostname libroblox.so's own engine needs to resolve,
    // very plausibly including whatever a real game-server join
    // ultimately connects to, fails silently at the DNS layer before
    // a single real outbound connection is ever attempted. `--ro-bind`
    // on a symlink source (this host's own /etc/resolv.conf is a real
    // systemd-resolved stub symlink) binds the resolved real file's
    // content, not a dangling symlink, transparent regardless of the
    // host's own resolver setup. Best-effort: a host without one of
    // these files just doesn't get that specific bind, not a hard
    // failure (matches this project's own established pattern for
    // optional host files).
    if (fs::exists("/etc/resolv.conf")) {
        argv_storage.push_back("--ro-bind");
        argv_storage.push_back("/etc/resolv.conf");
        argv_storage.push_back("/etc/resolv.conf");
    }
    if (fs::exists("/etc/hosts")) {
        argv_storage.push_back("--ro-bind");
        argv_storage.push_back("/etc/hosts");
        argv_storage.push_back("/system/etc/hosts");
    }

    // Untested until now: hypothesis: this sandbox has never bound
    // any real CA certificate store at all. Process A's own HTTP fetches
    // (ClientSettings, PlaceLauncher, authenticated-user identity) all
    // run in glibc Process A, outside this sandbox entirely, and were
    // never evidence that Roblox's own native code, running inside
    // Process B, doing its own real internal networking (the
    // `StartupController`/BrowserTrackerId async task documented at
    // length in this file's own gap #1), can complete a real TLS
    // handshake at all. A missing trust store is exactly the kind of
    // failure that wouldn't show up as an obvious crash or error log,
    // BoringSSL/OpenSSL-style TLS libraries generally just fail (or
    // hang, depending on retry logic) the handshake silently from the
    // caller's perspective. `SSL_CERT_FILE` is the real, standard env
    // var both OpenSSL and BoringSSL honor for this; binding the host's
    // real CA bundle at its own real path costs nothing if this turns
    // out not to be the real cause.
    std::string real_ca_bundle_path;
    if (fs::exists("/etc/pki/tls/certs/ca-bundle.crt")) {
        real_ca_bundle_path = "/etc/pki/tls/certs/ca-bundle.crt";
    } else if (fs::exists("/etc/ssl/certs/ca-certificates.crt")) {
        real_ca_bundle_path = "/etc/ssl/certs/ca-certificates.crt";
    }
    if (!real_ca_bundle_path.empty()) {
        argv_storage.push_back("--ro-bind");
        argv_storage.push_back(real_ca_bundle_path);
        argv_storage.push_back(real_ca_bundle_path);
    }

    // Live-traced follow-up to the resolv.conf/hosts binds above.
    // Caught in testing: history: this used to fail outright
    // ("Permission denied" creating a new mount point at
    // /dev/__properties__) back when /dev was the old --dev-bind /dev
    // /dev (a live view of the real, root-owned host devtmpfs, which
    // can't have new mount points created under it without real
    // privileges). Fixed by switching /dev itself to bwrap's own
    // synthetic --dev above (see that block's own doc comment for the
    // real a live syscall trace evidence backing the switch), /dev stays a real
    // bwrap-managed tmpfs now, so a new synthetic entry under it can be
    // created normally.
    if (fs::exists(default_property_area_path())) {
        argv_storage.push_back("--ro-bind");
        argv_storage.push_back(default_property_area_path());
        argv_storage.push_back("/dev/__properties__");
    }

    for (const HostBind& bind : config.extra_binds) {
        argv_storage.push_back(bind.writable ? "--bind" : "--ro-bind");
        argv_storage.push_back(bind.path);
        argv_storage.push_back(bind.path);
    }

    if (!config.working_directory.empty()) {
        argv_storage.push_back("--chdir");
        argv_storage.push_back(config.working_directory);
    }

    // Caught in testing:, fixed (not configurable) requirement, not an
    // optional diagnostic like config.extra_env below. traced in the engine
    // (symbol resolution against the real, unstripped extracted libc.so
    // resolved the exact crash site by name): a real SIGFPE
    // (FPE_INTDIV) inside libc.so's own _cache_lookup_p (res_cache.c),
    // reached the first time anything actually calls getaddrinfo() after
    // _resolv_set_nameservers_for_net() registers a nameserver. Root
    // cause, confirmed by re-reading _resolv_set_nameservers_for_net's
    // own code (see the engineering notes gap #1): when creating a new
    // resolver-cache entry, it reads real env var ANDROID_DNS_MODE, and
    // ONLY allocates a real, nonzero-sized entry pool
    // (`calloc(0x50, 0x280)`, 640 entries; real Android's own actual
    // cache size) if it's exactly "local"; any other value (including
    // unset, this sandbox's real prior state) allocates a **zero-entry**
    // pool (`calloc(0x50, 0)`) instead, and the real cache-lookup code
    // then divides by that pool size somewhere inside _cache_lookup_p:
    // a real, silent zero-size allocation "succeeding" (calloc(x, 0) is
    // valid, non-null) followed by a real crash the first time it's used
    // is exactly the kind of failure this project's "live-test
    // everything" discipline exists to catch; nothing about the
    // registration call itself (rc==0) hinted this was coming. Real,
    // documented Android convention: init.rc sets this for app/system
    // processes; Stud has no init.rc, so it must be set explicitly here.
    argv_storage.push_back("--setenv");
    argv_storage.push_back("ANDROID_DNS_MODE");
    argv_storage.push_back("local");

    // See the real CA-bundle bind above, SSL_CERT_FILE is the real,
    // standard OpenSSL/BoringSSL env var for this, set unconditionally
    // when a real bundle was found and bound.
    if (!real_ca_bundle_path.empty()) {
        argv_storage.push_back("--setenv");
        argv_storage.push_back("SSL_CERT_FILE");
        argv_storage.push_back(real_ca_bundle_path);
    }

    for (const auto& [name, value] : config.extra_env) {
        argv_storage.push_back("--setenv");
        argv_storage.push_back(name);
        argv_storage.push_back(value);
    }

    argv_storage.push_back("--unshare-pid");
    argv_storage.push_back("--unshare-ipc");
    argv_storage.push_back("--unshare-uts");
    argv_storage.push_back("--unshare-cgroup");
    if (!config.share_network) {
        argv_storage.push_back("--unshare-net");
    }
    // Caught in testing:, real root cause of the DNS property-area fix
    // (see property_area.h's own doc comment): bionic's own real
    // prop_area::map_fd_ro(), confirmed via this project's own
    // reading of the actual extracted libc.so, requires the mapped
    // file's real st_uid/st_gid to both be 0 (real Android's own
    // security model: only privileged system processes are meant to
    // write property files). Confirmed live: __system_properties_init()
    // executed cleanly (no trap) but returned -1 with the file owned by
    // this project's own real, unprivileged host user. A real, standard,
    // well-known unprivileged-sandboxing technique (the same one Flatpak
    // and rootless Docker use) fixes this WITHOUT any real host
    // privilege escalation: a new user namespace can remap the real,
    // unprivileged calling uid to appear as uid/gid 0 *only inside that
    // namespace's own view*. Process B still has zero real capabilities
    // on the host, this only changes what stat() reports for files it
    // already, really owns. Not yet live-verified past this point; see
    // this project's own the engineering notes for the real, honest caveat about
    // whether Roblox's own code branches differently on getuid()==0 in
    // some other, unexpected way.
    argv_storage.push_back("--unshare-user");
    argv_storage.push_back("--uid");
    argv_storage.push_back("0");
    argv_storage.push_back("--gid");
    argv_storage.push_back("0");
    // Caught in testing: regression fixed: --die-with-parent here directly
    // contradicts this project's own documented, intentional architecture
    // (ui/src/main.cpp's launch_game()/proceed(): "hand off and get out
    // of the way ... the actual game runs in the separate runtime
    // process", same as a magnet link doesn't need its torrent client's
    // launcher to stay open). As long as stud-ui exited promptly right
    // after this posix_spawn() (the correct, intended behavior, and the
    // ACTUAL behavior once a separate real bug, QApplication::quit()
    // being a no-op before app.exec() started, got fixed), this flag
    // made bwrap kill Process B the moment its immediate parent (stud-ui)
    // exited, seconds into every real launch, before it ever printed a
    // single line. Silently masked for a long time by the other bug
    // (stud-ui never actually exiting kept Process B's parent alive by
    // accident); surfaced immediately, live, the moment that bug was
    // fixed. Process B is meant to run fully independently of Process A's
    // lifetime. No die-with-parent semantics belong here at all.

    // Invoke linker64 directly as the loader helper (its own documented
    // "linker64 PROGRAM [ARGS]" mode), rather than relying on the
    // kernel's own PT_INTERP-driven exec of executable_path, verified
    // end-to-end this way (real "hello from bionic" output, exit 0)
    // during Phase 1 bring-up; doesn't require executable_path's own
    // PT_INTERP string to resolve to any particular path.
    argv_storage.push_back("/system/lib64/linker64");
    argv_storage.push_back(config.executable_path);
    for (const std::string& arg : config.args) {
        argv_storage.push_back(arg);
    }

    // Direct follow-up once net.dns1/net.dns2 (the property-area
    // approach above) were confirmed absent from this libc.so's actual
    // resolver code (see the engineering notes gap #1): threads the same real host
    // nameservers into Process B's own real argv (not a bwrap flag.
    // This must come after config.args, in Process B's own command
    // line) so its bootstrap can call the real, exported
    // _resolv_set_nameservers_for_net directly. Kept additive alongside
    // the property-area bind above rather than replacing it, property
    // propagation may still matter for other, unrelated
    // __system_property_get() callers even though it didn't pan out for
    // DNS specifically.
    {
        std::vector<std::string> servers = real_host_nameservers();
        if (!servers.empty()) {
            std::string joined = servers[0];
            for (size_t i = 1; i < servers.size(); ++i) joined += "," + servers[i];
            argv_storage.push_back("--dns-servers");
            argv_storage.push_back(joined);
        }
    }

    return argv_storage;
}

namespace {

// One spawn, shared by both launch paths below. `envp` empty means this
// process's own environment, which is what bwrap gets (it sets Process
// B's with --setenv from the inside).
pid_t spawn_process_b(const std::string& program,
                      std::vector<std::string> argv_storage,
                      const std::vector<std::string>& envp_storage,
                      const std::string& working_directory,
                      int stdout_fd) {
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (std::string& s : argv_storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    std::vector<char*> envp;
    if (!envp_storage.empty()) {
        envp.reserve(envp_storage.size() + 1);
        for (const std::string& s : envp_storage) envp.push_back(const_cast<char*>(s.c_str()));
        envp.push_back(nullptr);
    }

    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_t* actions_ptr = nullptr;
    if (stdout_fd >= 0 || !working_directory.empty()) {
        ::posix_spawn_file_actions_init(&actions);
        actions_ptr = &actions;
    }
    if (stdout_fd >= 0) {
        ::posix_spawn_file_actions_adddup2(actions_ptr, stdout_fd, STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(actions_ptr, stdout_fd, STDERR_FILENO);
    }
    // bwrap has --chdir; without it the working directory has to be set
    // here, and for the same reason (ProcessBConfig::working_directory's
    // own comment: Roblox writes to relative paths).
    if (!working_directory.empty()) {
        ::posix_spawn_file_actions_addchdir_np(actions_ptr, working_directory.c_str());
    }

    int rc = ::posix_spawn(&pid, program.c_str(), actions_ptr, nullptr, argv.data(),
                           envp.empty() ? environ : envp.data());
    if (actions_ptr != nullptr) ::posix_spawn_file_actions_destroy(actions_ptr);
    if (rc != 0) {
        throw std::runtime_error("stud: posix_spawn(" + program + ") failed: " +
                                 std::string(std::strerror(rc)));
    }
    return pid;
}

}  // namespace

pid_t launch_process_b(const ProcessBConfig& config) {
    const std::string bionic_lib_dir =
        config.bionic_lib_dir.empty() ? default_shipped_bionic_directory() : config.bionic_lib_dir;
    const std::string linker64_path =
        config.linker64_path.empty() ? default_linker64_path() : config.linker64_path;

    if (!file_readable(config.executable_path)) {
        throw std::runtime_error("stud: Process B executable not found or not readable: " +
                                  config.executable_path);
    }

    // Best-effort; see generate_property_area_if_possible()'s own
    // doc comment. A failure here just means build_process_b_argv()
    // won't find the file to bind (it checks fs::exists itself), same
    // honest degradation as a host with no real /etc/hosts.
    generate_property_area_if_possible();

    // No user namespace, no bwrap; see can_create_user_namespace().
    // Process B then runs in whatever sandbox already refused it one,
    // which in the only case this happens (a Flatpak) is a real sandbox
    // of its own: no host filesystem, its own /tmp, its own tmpfs root.
    // What is genuinely lost is Stud's own narrowing on top of that,
    // the GPU driver paths bwrap keeps out of this process, and the PID/
    // IPC/UTS namespaces, so it is a fallback, never a preference.
    if (!can_create_user_namespace()) {
        link_android_system_tree(config, bionic_lib_dir, linker64_path);
        std::fprintf(stderr,
                     "stud: no user namespaces available (a Flatpak sandbox refuses them), "
                     "so Process B runs without its own bwrap sandbox\n");

        std::string ca_bundle_path;
        if (fs::exists("/etc/pki/tls/certs/ca-bundle.crt")) {
            ca_bundle_path = "/etc/pki/tls/certs/ca-bundle.crt";
        } else if (fs::exists("/etc/ssl/certs/ca-certificates.crt")) {
            ca_bundle_path = "/etc/ssl/certs/ca-certificates.crt";
        }

        // The same loader invocation the sandboxed argv ends with, and
        // the same --dns-servers tail after Process B's own arguments.
        std::vector<std::string> argv_storage = {"/system/lib64/linker64",
                                                 config.executable_path};
        for (const std::string& arg : config.args) argv_storage.push_back(arg);
        std::vector<std::string> servers = real_host_nameservers();
        if (!servers.empty()) {
            std::string joined = servers[0];
            for (size_t i = 1; i < servers.size(); ++i) joined += "," + servers[i];
            argv_storage.push_back("--dns-servers");
            argv_storage.push_back(joined);
        }
        return spawn_process_b("/system/lib64/linker64", std::move(argv_storage),
                               process_b_environment(config, ca_bundle_path),
                               config.working_directory, config.stdout_fd);
    }

    const std::string bwrap_path = find_bwrap_on_path();
    if (bwrap_path.empty()) throw BwrapNotFound();

    return spawn_process_b(bwrap_path,
                           build_process_b_argv(config, bwrap_path, bionic_lib_dir, linker64_path),
                           {}, std::string(), config.stdout_fd);
}

}  // namespace stud::bionic_runtime
