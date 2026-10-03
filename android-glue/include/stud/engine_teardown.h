#pragma once

// How long Process B gives the engine to shut down, in one place, so the
// UI's wait on a restart or quit is the same number Process B works to.
//
// Every engine lifecycle call is bounded (engine_v2_bridge.cpp's
// run_bounded_v2_call), and Process B's teardown is the calls listed in
// run_engine_v2_teardown(): LeaveGame, then DestroyApp. After them it
// exits, whether they returned or not.
namespace stud::engine_teardown {

inline constexpr int kBoundedCallPollMs = 50;
inline constexpr int kBoundedCallMaxPolls = 160;
inline constexpr int kTeardownBoundedCalls = 2;  // LeaveGame, DestroyApp

inline constexpr int kTeardownLimitMs =
    kTeardownBoundedCalls * kBoundedCallMaxPolls * kBoundedCallPollMs;

}  // namespace stud::engine_teardown
