#pragma once

#include <cstddef>

// The engine's startup render throttle, and the race that left it on.
//
// While the app starts, the engine renders its render job at about one
// frame a second (`SurfaceController::RenderJob::setStartupThrottle: true`,
// set when the render job is created) and lifts it when the home page is
// loaded ("Restoring rendering frequency to normal", after onGameLoaded and
// after HOME_PAGE_INTERACTIVE). That assumes the render job exists by then.
// Under Stud the home page can be ready first: the restores then find no
// render job (`setStartupThrottle: renderJob is null`), are dropped, and
// the render job is born throttled with nothing left to lift it but the
// engine's own ten-second safety timeout. That was the one frame a second
// at launch, on some launches and not others.
//
// The HOME_PAGE_INTERACTIVE restore runs on the engine thread right after
// that thread hands the notification to Stud's Java side. So Stud holds
// the notification until the render job exists, and the restore that
// follows it lands.
namespace stud::jni_bridge {

// Every engine log line; notes when the render job has come up.
void note_startup_throttle_log_line(const char* text, size_t length);

// Returns once the engine's render job exists, or after the bound every
// engine lifecycle step gets (stud/engine_teardown.h). At once if it
// already does.
void wait_for_render_job();

}  // namespace stud::jni_bridge
