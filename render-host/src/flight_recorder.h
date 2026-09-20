#pragma once

#include <atomic>
#include <cstdint>

// A ring buffer of what just happened, dumped the moment something goes
// wrong.
//
// Every log this investigation has produced describes the AFTERMATH: a
// device already lost, a fence already stuck, a present that already took
// twelve seconds. None of them say what the seconds before looked like,
// and that is the part that identifies a cause. Reconstructing it from
// ordinary logging is not an option either -- printing every submit and
// every barrier costs more than the thing being measured and changes the
// timing that is under suspicion.
//
// So events go into a fixed ring in memory, costing a timestamp and four
// stores, and NOTHING is printed until a trigger fires. Then the whole
// recent history is written out at once, with timestamps relative to the
// trigger, which is the view nobody has had.
//
// Triggers: a lost device, a fence that will not signal, a slow fence
// wait, a slow present, a failed submit.
namespace stud::render_host::fr {

// Deliberately coarse. Anything recorded per draw or per command would
// swamp the ring and hide the frame structure; these are the events a
// hang or a stall is made of.
enum class Event : uint32_t {
    Submit,        // a=fence, b=first command buffer, c=command buffer count
    WaitBegin,     // a=first fence, b=count, c=timeout ms (0 = forever)
    WaitEnd,       // a=first fence, b=VkResult, c=microseconds waited
    ResetFences,   // a=first fence, b=count
    AcquireBegin,  // a=swapchain
    AcquireEnd,    // a=swapchain, b=VkResult, c=microseconds
    PresentBegin,  // a=swapchain, b=image index
    PresentEnd,    // a=VkResult, b=microseconds total, c=microseconds in driver
    Barrier,       // a=image, b=oldLayout, c=newLayout
    SwapchainNew,  // a=swapchain, b=width, c=height
    SwapchainGone, // a=swapchain
    DeviceNew,     // a=device
    DeviceLost,    // a=0
    UpscaleSubmit, // a=image index, b=VkResult
    MappedFlush,   // a=bytes, b=mappings scanned, c=microseconds
    Note,          // a=arbitrary, b=arbitrary, c=arbitrary
};

// Whether anything is being recorded, as an inline read.
//
// This is on the hot path -- vk_cmd_record replays every barrier the
// engine sends, thousands per frame -- so when the recorder is off the
// cost has to be a single predictable branch and nothing else. It used
// to be an out-of-line call into record(), which then checked a local
// static and returned; that is a call, a guard-variable load and a
// branch per barrier, paid by every shipped copy of Stud forever.
//
// Set once at startup from STUD_FLIGHT_RECORDER. Relaxed because it
// never changes after that and nothing orders against it.
namespace detail {
extern std::atomic<bool> g_on;
}

inline bool enabled() { return detail::g_on.load(std::memory_order_relaxed); }

// Out of line: only reached when recording is actually on.
void record_event(Event e, uint64_t a, uint64_t b, uint64_t c);

// Records one event. Cheap enough for every submit and every barrier
// WHEN ON: one clock read, one atomic increment, four stores. When off,
// one branch.
inline void record(Event e, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0) {
    if (!enabled()) return;
    record_event(e, a, b, c);
}

// Writes the recent history out, newest last, with timestamps relative
// to now. Goes to stdout and, when STUD_FLIGHT_RECORDER_PATH is set, to
// that file as well -- a freeze is exactly when a terminal is least
// likely to be where the output is wanted.
//
// Rate limited internally: a device loss tends to arrive with a burst of
// other failures behind it, and twenty identical dumps would bury the
// first one, which is the only one that matters.
void dump(const char* why);

}  // namespace stud::render_host::fr
