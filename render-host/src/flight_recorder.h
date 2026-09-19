#pragma once

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

// Records one event. Cheap enough for every submit and every barrier:
// one clock read, one atomic increment, four stores. No allocation, no
// formatting, no lock held across anything that can block.
void record(Event e, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0);

// Writes the recent history out, newest last, with timestamps relative
// to now. Goes to stdout and, when STUD_FLIGHT_RECORDER_PATH is set, to
// that file as well -- a freeze is exactly when a terminal is least
// likely to be where the output is wanted.
//
// Rate limited internally: a device loss tends to arrive with a burst of
// other failures behind it, and twenty identical dumps would bury the
// first one, which is the only one that matters.
void dump(const char* why);

// Whether anything is being recorded. Off unless STUD_FLIGHT_RECORDER is
// set, so an ordinary session pays nothing but a predictable branch.
bool enabled();

}  // namespace stud::render_host::fr
