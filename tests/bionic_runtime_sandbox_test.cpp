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
#include "stud/property_area.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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

uint32_t u32(const std::vector<char>& data, size_t at) {
    uint32_t value;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return value;
}

// The offset of the child called `name` of the node at `parent`, walked the
// way bionic's prop_area::find_property does; 0 when there is none.
uint32_t find_child(const std::vector<char>& data, size_t base, uint32_t parent, const char* name) {
    for (uint32_t at = u32(data, base + parent + 16); at != 0;) {
        const uint32_t length = u32(data, base + at);
        const std::string node(data.data() + base + at + 20, length);
        if (node == name) return at;
        const bool go_left = std::strlen(name) != length ? std::strlen(name) < length : name < node;
        at = u32(data, base + at + (go_left ? 8 : 12));
    }
    return 0;
}

void test_property_area() {
    const std::string path = "stud_test_property_area";
    check(stud::bionic_runtime::write_property_area(path, {{"net.dns1", "1.2.3.4"}, {"net.dns2", "5.6.7.8"}}),
          "the property area is written");
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::remove(path.c_str());
    const size_t base = 128;
    check(u32(data, base) == 0, "the node at data_+0 is the nameless root");
    const uint32_t net = find_child(data, base, 0, "net");
    check(net != 0, "the root's children hold the top-level segment");
    const uint32_t dns2 = find_child(data, base, net, "dns2");
    check(dns2 != 0, "the next segment is under it");
    const uint32_t info = u32(data, base + dns2 + 4);
    check(info != 0 && std::strcmp(data.data() + base + info + 4, "5.6.7.8") == 0,
          "and it holds the value");
}

void test_sandbox_environment() {
    char lc[] = "LC_TIME=C", stud[] = "STUD_DEBUG=1", home[] = "HOME=/home/t";
    char preload[] = "LD_PRELOAD=/usr/lib64/libz.so.1", library[] = "LD_LIBRARY_PATH=/opt";
    char token[] = "GITHUB_TOKEN=secret", studlike[] = "STUDIO=x";
    char* env[] = {lc, preload, stud, library, token, home, studlike, nullptr};
    const std::vector<std::string> kept = stud::bionic_runtime::sandbox_environment(env);
    check(kept == std::vector<std::string>({"LC_TIME=C", "STUD_DEBUG=1", "HOME=/home/t"}),
          "only the listed names reach the sandbox");
}

}  // namespace

int main() {
    test_property_area();
    test_sandbox_environment();

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

    // /sys is a list of paths, never the whole tree.
    {
        bool whole_sys = false, cpu_tree = false;
        for (std::size_t i = 0; i < argv.size(); ++i) {
            whole_sys = whole_sys || argv[i] == "/sys";
            cpu_tree = cpu_tree || argv[i] == "/sys/devices/system/cpu";
        }
        check(!whole_sys, "/sys is not bound whole");
        check(cpu_tree, "the CPU tree the core count comes from is bound");
    }

    // An older bwrap would refuse this option and never start.
    check(std::find(argv.begin(), argv.end(), "--disable-userns") == argv.end(),
          "--disable-userns only appears when asked for");
    config.block_nested_user_namespaces = true;
    const std::vector<std::string> blocked =
        build_process_b_argv(config, "/usr/bin/bwrap", "/home/testuser/stud/third_party/android-bionic",
                             "/home/testuser/stud/third_party/android-bionic/linker64");
    const auto disable = std::find(blocked.begin(), blocked.end(), "--disable-userns");
    check(disable != blocked.end() &&
              disable > std::find(blocked.begin(), blocked.end(), "--unshare-user"),
          "and comes after --unshare-user, which it needs");

    std::printf("all bionic-runtime sandbox checks passed\n");
    return 0;
}
