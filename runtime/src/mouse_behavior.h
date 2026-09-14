#pragma once

#include "stud/linker.h"

namespace stud::runtime {

// The engine's own MouseBehavior, which is the one fact Stud's cursor
// handling cannot work without and which the engine does not export.
//
// The Lua camera sets MouseBehavior = LockCurrentPosition for as long as
// it is rotating the view, and the engine then PINS its own cursor and
// ignores every position it is given. Over an in-game UI that never
// happens and the engine follows the positions instead. The two look
// identical from outside, same button, same motion, so a cursor rule
// that cannot tell them apart is wrong for one of them, always: freezing
// the cursor pins it on a UI, and letting it follow makes it jump at the
// end of a rotation. Every attempt at a heuristic (travel, speed, flick)
// has been measured and failed, because a slow orbit and a UI drag are
// genuinely the same gesture.
//
// libroblox exports exactly one predicate over this state,
// nativeGetMainWindowIsMouseLockedCenter, and its whole body is:
//
//     get the object from its singleton getter
//     load its input subsystem
//     compare MouseBehavior with LockCenter
//
// So it answers only for LockCenter (first person, shift lock) and says
// nothing about LockCurrentPosition, which is the case that matters.
//
// This reads the same field, and finds it the same way the engine's own
// code does: by DECODING that exported function at startup, the
// getter's address and argument, the guard object it takes, and the two
// field displacements, out of whichever libroblox.so is actually
// loaded. Nothing here is a constant taken from one build; if
// the function's shape changes, the decode fails, the probe reports
// "unknown", and the caller falls back to its own behaviour. That is the
// honest reading of this project's version-agnostic rule for a fact that
// exists nowhere else: no hardcoded offsets, a validity check, and a
// working fallback.
//
// Real MouseBehavior values, from Roblox's own published enum.
enum class MouseBehavior : int {
    kUnknown = -1,
    kDefault = 0,
    kLockCenter = 1,
    kLockCurrentPosition = 2,
};

// Decodes the probe out of the loaded engine. Safe to call once the
// library is loaded; says in the log whether it worked and what it found.
bool init_mouse_behavior_probe(const stud::linker::LoadedLibrary& lib);

// The engine's current MouseBehavior, or kUnknown if the probe could not
// be built or the read failed. Takes the same guard the engine's own
// predicate takes around the read, and every fault is trapped.
MouseBehavior read_mouse_behavior();

}  // namespace stud::runtime
