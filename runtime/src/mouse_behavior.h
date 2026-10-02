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
// This reads the same field, found the way the engine's own code finds
// it: the exported function is decoded at startup, with a real x86
// decoder, by what its instructions do. The walk follows which register
// holds the getter's object, follows calls that are handed that object
// (2.740 moved the whole body into a helper), and takes the chain of loads
// that ends in the comparison with LockCenter. Register choice, encodings,
// inlining and the lock taken around the read do not matter to it.
//
// Found is then not trusted. The same exported predicate keeps answering
// for LockCenter, so every poll checks the field against it
// (verify_mouse_behavior); the first real disagreement switches the field
// off for the run and the cursor follows the predicate alone. A byte
// pattern that matched the wrong instructions is exactly how the cursor
// started teleporting after 2.740: that cannot drive the cursor again.
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
// be built, has been switched off, or the read failed. Every fault is
// trapped.
MouseBehavior read_mouse_behavior();

// Checks a field reading against the engine's own LockCenter predicate,
// read around it. A disagreement switches the field off for the run.
// Returns whether the field may still be used.
bool verify_mouse_behavior(MouseBehavior field, bool predicate_locks_center);

}  // namespace stud::runtime
