#include "stud/startup_throttle.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string_view>

#include "stud/engine_teardown.h"

namespace stud::jni_bridge {

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
bool g_render_job_up = false;

}  // namespace

void note_startup_throttle_log_line(const char* text, size_t length) {
    if (text == nullptr) return;
    // Either line means the render job exists. Older engines log the
    // throttle being set in its constructor; current ones log only the
    // constructor itself ("SurfaceController::RenderJob[_:1]::RenderJob"),
    // and waiting for the throttle line alone held every launch for the
    // full bound, with the engine thread and so its frames stopped.
    std::string_view line(text, length);
    while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
    const bool throttle_line = line.find("RenderJob::setStartupThrottle: true") != line.npos;
    const size_t job = line.find("SurfaceController::RenderJob[");
    const bool constructor_line = job != line.npos && line.size() >= 12 &&
                                  line.substr(line.size() - 12) == "]::RenderJob";
    if (!throttle_line && !constructor_line) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_render_job_up = true;
    }
    g_cv.notify_all();
}

void wait_for_render_job() {
    using stud::engine_teardown::kBoundedCallMaxPolls;
    using stud::engine_teardown::kBoundedCallPollMs;
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(g_mutex);
    if (g_render_job_up) return;
    const bool up = g_cv.wait_for(lock,
                                  std::chrono::milliseconds(kBoundedCallMaxPolls * kBoundedCallPollMs),
                                  [] { return g_render_job_up; });
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    std::printf("stud: the home page was ready before the engine's render job; held it %lldms %s\n",
                static_cast<long long>(waited.count()),
                up ? "until the render job came up, so the startup throttle is lifted"
                   : "and gave up, so the engine's own safety timeout lifts the throttle");
    std::fflush(stdout);
}

}  // namespace stud::jni_bridge
