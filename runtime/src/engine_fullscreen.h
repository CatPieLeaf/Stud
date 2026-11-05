#pragma once

namespace stud::runtime {

// Roblox's own Fullscreen option, kept in step with Stud's window.
//
// The in-game menu shows the settings value InFullScreen reads, and its option
// runs ToggleFullscreen, which on Windows looks up an action this build never
// registers and so does nothing. Neither is exported.
//
// Both are found in the loaded engine by what they are, and nothing of the
// engine's changes: not one byte of its code, and it never calls into Stud.
//  - The value belongs to a settings object. Stud reads it and, when it differs
//    from what the window really is, calls the engine's own setter, so the menu
//    tells the truth and hears about a change. Stud never writes the value.
//  - The option is noticed by counting how many times that function has run,
//    which the CPU's debug registers do from outside, through perf_event_open.
//    Stud reads the count and asks for the window to change.
//
// Each part switches itself off, said once in the log, on a build where the
// code does not have the expected shape, or where the system refuses the
// counter (kernel.perf_event_paranoid above 2, or a sandbox that blocks it, as
// Flatpak's does). The menu then does what it did before.

// Finds both, given any address inside the engine. Before the engine starts
// any thread: threads created earlier are not counted.
void init_engine_fullscreen(const void* inside_engine);

// One tick of the main loop. Sets the engine's value to `window_fullscreen`,
// and says whether the in-game option was used since the last tick.
bool engine_fullscreen_tick(bool window_fullscreen);

}  // namespace stud::runtime
