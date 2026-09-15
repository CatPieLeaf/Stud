// Temporary diagnostic driver (the engineering notes' own "throwaway
// diagnostic driver" pattern, bypasses stud-ui's login/APK-config gate
// this sandbox genuinely has neither of, real interactive login and
// ~/.config/stud/settings.json). Reconstructs the exact same
// ProcessBConfig ui/src/main.cpp's launch_game() builds, calling
// bionic_runtime::launch_process_b() directly, so the real bring-up
// sequence (dlopen, JNI bootstrap, engine V2 sequence) runs exactly as it
// would from a real stud-ui launch. Purpose here specifically: live-verify
// the trap_recovery.cpp near_null unbounded-dereference fix, does the
// process now survive the known initializeLuaAppWithLoggedInUser crash?
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>
#include <thread>

#include "stud/bionic_runtime.h"
#include "stud/ipc.h"

namespace {
// Plain curl subprocess fetch. This driver is glibc-side and throwaway,
// so shelling out is simpler than linking libcurl just for one public GET
// (same real, public, no-login-needed endpoint main.cpp's own
// fetch_client_settings() hits from the real stud-ui launch path).
std::string curl_get(const std::string& url) {
    std::string cmd = "curl -s --max-time 10 '" + url + "'";
    std::array<char, 4096> buf{};
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;
    size_t n;
    while ((n = std::fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        result.append(buf.data(), n);
    }
    pclose(pipe);
    return result;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <libroblox.so path> <apk path>\n", argv[0]);
        return 1;
    }
    std::string so_path = argv[1];
    std::string apk_path = argv[2];

    // Where ExternalProject puts the bionic runtime inside the build
    // directory. Taken from this tool's own location so it works in any
    // checkout; STUD_DIAG_RUNTIME_BINARY overrides it.
    std::string process_b_binary;
    if (const char* env = std::getenv("STUD_DIAG_RUNTIME_BINARY")) {
        process_b_binary = env;
    } else {
        std::error_code ec;
        std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
        // <build>/tools/stud_diag_launch_direct -> <build>
        std::filesystem::path build_dir = self.parent_path().parent_path();
        process_b_binary =
            (build_dir / "runtime-prefix/src/runtime-build/stud-runtime-bionic").string();
    }
    if (!std::filesystem::exists(process_b_binary)) {
        std::fprintf(stderr, "diag: process_b binary not found: %s\n", process_b_binary.c_str());
        return 1;
    }

    std::string socket_path = stud::ipc::default_socket_path();

    stud::ipc::LaunchPayload payload;
    // Real session cookie. The earlier fake placeholder is gone; it
    // only ever answered the narrow "does cookie PRESENCE change which
    // branch runs" question, and every question past that one needs a
    // cookie the engine can actually authenticate with.
    //
    // Supplied at runtime, never hardcoded: a live credential in a
    // source file would sit in the repo and leak into logs. Point
    // STUD_DIAG_COOKIE_FILE at a file containing the raw
    // .ROBLOSECURITY value, or set STUD_DIAG_COOKIE directly.
    if (const char* cookie_file = std::getenv("STUD_DIAG_COOKIE_FILE")) {
        std::ifstream in(cookie_file);
        if (in) {
            std::string value((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
            while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
                value.pop_back();
            }
            payload.session_cookie = value;
            std::fprintf(stderr, "diag: using real session cookie from %s (%zu bytes)\n",
                         cookie_file, payload.session_cookie.size());
        } else {
            std::fprintf(stderr, "diag: could not read STUD_DIAG_COOKIE_FILE=%s\n", cookie_file);
        }
    } else if (const char* cookie_env = std::getenv("STUD_DIAG_COOKIE")) {
        payload.session_cookie = cookie_env;
        std::fprintf(stderr, "diag: using real session cookie from STUD_DIAG_COOKIE (%zu bytes)\n",
                     payload.session_cookie.size());
    }

    // Fresh ClientSettings fetch (public endpoint, no login needed,
    // confirmed this project's own earlier session). Without this the
    // engine hits a real, different, earlier crash ("Can't initialize the
    // TaskScheduler before flags have been loaded") before ever reaching
    // the join attempt this test cares about.
    payload.client_settings_body = curl_get(
        "https://clientsettingscdn.roblox.com/v2/settings/application/GoogleAndroidApp");
    payload.client_settings_http_status = payload.client_settings_body.empty() ? 0 : 200;
    std::fprintf(stderr, "diag: fetched ClientSettings, %zu bytes\n",
                 payload.client_settings_body.size());

    // Deliberate: NOT a deep link. Stud's own standing rule (per
    // the user's explicit, repeated instruction) is to never attempt to
    // launch/join an actual Roblox game/experience, only the app UI
    // (bare launch, Lua home screen/app chrome) is in scope. A deep link
    // with a real placeId would route toward a real game join; leaving
    // this at 0 keeps every test here on the bare-launch path only.
    payload.deep_link_place_id = 0;

    std::thread payload_thread([socket_path, payload]() {
        try {
            stud::ipc::serve_launch_payload_once(socket_path, payload);
        } catch (const stud::ipc::IpcError& e) {
            std::fprintf(stderr, "diag: payload serve error: %s\n", e.what());
        }
    });

    std::vector<stud::bionic_runtime::HostBind> extra_binds;
    extra_binds.push_back(
        {std::filesystem::path(so_path).parent_path().string(), /*writable=*/true});
    if (const char* xdg_runtime_dir = std::getenv("XDG_RUNTIME_DIR")) {
        extra_binds.push_back({xdg_runtime_dir, /*writable=*/true});
    }
    {
        std::string apk_dir = std::filesystem::path(apk_path).parent_path().string();
        if (!apk_dir.empty()) {
            extra_binds.push_back({apk_dir, /*writable=*/false});
        }
    }

    stud::bionic_runtime::ProcessBConfig config;
    config.executable_path = process_b_binary;
    config.args = {so_path, "--apk", apk_path, "--ipc-connect", socket_path};
    config.extra_binds = std::move(extra_binds);
    config.working_directory = std::filesystem::path(so_path).parent_path().string();
    config.extra_env.push_back({"STUD_VULKAN_CALL_TRACE", "1"});
    config.extra_env.push_back({"STUD_RENDER_CALL_TRACE", "1"});

    try {
        pid_t pid = stud::bionic_runtime::launch_process_b(config);
        std::fprintf(stderr, "diag: launch_process_b spawned pid=%d\n", static_cast<int>(pid));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "diag: launch_process_b failed: %s\n", e.what());
        payload_thread.join();
        return 1;
    }

    payload_thread.join();
    std::fprintf(stderr, "diag: handoff complete, process B pid launched, exiting driver\n");
    return 0;
}
