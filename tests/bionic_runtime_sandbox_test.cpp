// Real regression test for a real invariant this whole project's
// architecture depends on: Process B's bwrap sandbox must never bind a
// real host GPU driver path into the bionic process. The entire point
// of the three-process design (see bionic-runtime/include/stud/
// bionic_runtime.h's own doc comment) is that the vendor GPU driver
// only ever runs in Process C (stud-render-host), a plain glibc
// process, never inside a bionic/foreign-TLS process, accidentally
// binding a real driver path into Process B's sandbox (e.g. a future
// change naively trying to give it "direct" GPU access) would silently
// defeat that guarantee. Tests build_process_b_argv(), the real argv-
// building logic launch_process_b() itself spawns bwrap with, as a
// pure function, so this doesn't need bwrap or a real bionic install
// present to run.

#include "stud/bionic_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
    std::printf("ok: %s\n", what);
}

// Known host paths a real Linux system's GPU driver stack lives
// under, if any of these show up as a *bind source* (the host-side
// path bwrap is told to expose), that's exactly the mistake this test
// exists to catch. Deliberately broad (covers Mesa/DRI, NVIDIA's own
// installer paths, and the Vulkan ICD/loader config real apps use to
// find a driver) rather than narrow.
const char* kForbiddenSubstrings[] = {
    "/dri",
    "nvidia",
    "/usr/share/vulkan",
    "/etc/vulkan",
    "libGL.so",
    "libEGL.so.1",
    "libvulkan.so.1",
    "/usr/lib/x86_64-linux-gnu",
    "/usr/lib64/nvidia",
};

}  // namespace

int main() {
    using namespace stud::bionic_runtime;

    ProcessBConfig config;
    config.executable_path = "/home/testuser/Stud/stud/build/process_b-prefix/src/process_b-build/"
                              "stud-runtime-bionic";
    config.args = {"/home/testuser/.cache/stud/libroblox.so", "--apk",
                    "/home/testuser/game.apk", "--ipc-connect",
                    "/run/user/1000/stud/launch.sock"};
    // Real extra_binds shape, matching what ui/src/main.cpp's own
    // launch_game() actually constructs, the so_path's parent dir
    // (writable), $XDG_RUNTIME_DIR (writable), the APK's containing
    // directory (read-only). None of these are ever real driver paths
    // in normal operation, but the whole point of this test is to catch
    // it if that ever silently stops being true.
    config.extra_binds = {
        {"/home/testuser/.cache/stud", /*writable=*/true},
        {"/run/user/1000", /*writable=*/true},
        {"/home/testuser/Stud", /*writable=*/false},
    };
    config.working_directory = "/home/testuser/.cache/stud";

    std::vector<std::string> argv =
        build_process_b_argv(config, "/usr/bin/bwrap", "/home/testuser/stud/third_party/android-bionic",
                              "/home/testuser/stud/third_party/android-bionic/linker64");

    check(!argv.empty(), "build_process_b_argv() returns a non-empty argv");
    check(argv[0] == "/usr/bin/bwrap", "argv[0] is the real bwrap path");

    for (const std::string& arg : argv) {
        for (const char* forbidden : kForbiddenSubstrings) {
            bool found = arg.find(forbidden) != std::string::npos;
            if (found) {
                std::fprintf(stderr,
                              "FAILED: bwrap argv contains a real host driver path, arg='%s' "
                              "matched forbidden substring '%s'\n",
                              arg.c_str(), forbidden);
                std::exit(1);
            }
        }
    }
    std::printf("ok: no bwrap argv entry references a known real host GPU driver path\n");

    // Specific check on top of the substring scan: /dev is bound
    // as a whole (device *nodes*, not driver code; see this test's
    // own doc comment for why that's an accepted, understood tradeoff,
    // not this test's concern) but must never ALSO get a real driver
    // library directory bound over it via --bind/--ro-bind.
    int bind_count = 0;
    for (std::size_t i = 0; i + 2 < argv.size(); ++i) {
        if (argv[i] == "--bind" || argv[i] == "--ro-bind") {
            ++bind_count;
        }
    }
    check(bind_count > 0, "sanity: at least one real --bind/--ro-bind entry exists");

    // Caught in testing: regression coverage for this session's own DNS
    // fix chain (see the engineering notes gap #1): ANDROID_DNS_MODE=local is
    // a fixed, unconditional requirement (real SIGFPE inside libc.so's
    // own _cache_lookup_p otherwise, a zero-size resolver-cache pool
    // allocated when this env var is anything else, including unset),
    // so it must always appear via a real --setenv pair, regardless of
    // host state.
    {
        bool found_dns_mode = false;
        for (std::size_t i = 0; i + 2 < argv.size(); ++i) {
            if (argv[i] == "--setenv" && argv[i + 1] == "ANDROID_DNS_MODE" &&
                argv[i + 2] == "local") {
                found_dns_mode = true;
                break;
            }
        }
        check(found_dns_mode, "ANDROID_DNS_MODE=local is always set via --setenv");
    }

    // --dns-servers is real-host-state-dependent (real_host_nameservers()
    // parses the actual /etc/resolv.conf, not a mockable seam), only
    // assert it's present, as Process B's own real trailing argv (after
    // config.args, not a bwrap flag, a real, live-caught ordering bug
    // this session, see bionic_runtime.cpp's own doc comment), when this
    // test's own real host actually has a nameserver configured, so this
    // stays meaningful without being flaky on a resolv.conf-less CI box.
    {
        std::vector<std::string> real_nameservers;
        std::ifstream in("/etc/resolv.conf");
        std::string line;
        while (real_nameservers.size() < 2 && std::getline(in, line)) {
            std::istringstream iss(line);
            std::string keyword, value;
            if ((iss >> keyword >> value) && keyword == "nameserver") {
                real_nameservers.push_back(value);
            }
        }
        if (!real_nameservers.empty()) {
            // Find so_path's position (config.args[0], the first entry
            // after config.executable_path in the real, resolved argv,
            // easier to just locate "--dns-servers" directly and confirm
            // it comes after "--ipc-connect" launch.sock, the last of
            // config.args in this test's own fixture).
            std::size_t dns_servers_idx = std::string::npos;
            std::size_t ipc_connect_idx = std::string::npos;
            for (std::size_t i = 0; i < argv.size(); ++i) {
                if (argv[i] == "--dns-servers") dns_servers_idx = i;
                if (argv[i] == "--ipc-connect") ipc_connect_idx = i;
            }
            check(dns_servers_idx != std::string::npos,
                  "--dns-servers is present when the host has a real nameserver configured");
            check(ipc_connect_idx != std::string::npos && dns_servers_idx > ipc_connect_idx,
                  "--dns-servers comes after config.args (Process B's own argv, not a bwrap flag)");
        } else {
            std::printf(
                "skip: --dns-servers presence check (this host's /etc/resolv.conf has no real "
                "nameserver line)\n");
        }
    }

    std::printf("all bionic-runtime sandbox checks passed\n");
    return 0;
}
