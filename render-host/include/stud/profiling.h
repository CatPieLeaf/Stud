#pragma once

// Profiling markers, which are nothing at all unless Stud was built with
// -DSTUD_TRACY=ON.
//
// Why these exist: the render host's worst problems are the ones a log
// cannot describe. A stall every few seconds, a join that hangs once in
// ten attempts, a frame that takes two hundred milliseconds and leaves
// no trace of which call did it. Adding printf timers to chase those
// works once, then has to be taken out again, and the timing itself
// changes what is being measured.
//
// Tracy records instead: every marked scope, on every thread, against
// one timeline, and keeps the individual frame rather than an average.
// The frame that stalled is the one to look at, and an average is
// exactly what hides it.
//
// Off by default and not even downloaded then, so an ordinary build and
// a shipped package are unchanged.

#if defined(STUD_WITH_TRACY)

#include <tracy/Tracy.hpp>

// One frame, as the profiler counts them. Marked where Stud presents,
// because that is what a frame means here.
#define STUD_FRAME_MARK() FrameMark

// This scope, named for the function it sits in.
#define STUD_ZONE() ZoneScoped

// This scope, under a name of your own.
#define STUD_ZONE_NAMED(name) ZoneScopedN(name)

// A value worth plotting over time: a queue depth, a byte count.
#define STUD_PLOT(name, value) TracyPlot(name, value)

// A one-off marker on the timeline, for something that happened rather
// than something that took time.
#define STUD_MESSAGE(text) TracyMessageL(text)

#else

#define STUD_FRAME_MARK() do {} while (0)
#define STUD_ZONE() do {} while (0)
#define STUD_ZONE_NAMED(name) do {} while (0)
#define STUD_PLOT(name, value) do {} while (0)
#define STUD_MESSAGE(text) do {} while (0)

#endif
