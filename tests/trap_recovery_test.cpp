// Calls into the engine nest: a Roblox thread runs its whole body inside one
// bracket and makes ordinary trapped calls inside it. An inner call that traps
// must hand the outer bracket back, or the thread's next fault kills the
// process. The bug this guards against only shows in an optimised build, where
// a local changed after sigsetjmp() reads as zero once siglongjmp() lands, so
// this target is always built with -O2.

#include "stud/trap_recovery.h"

#include "check.h"

#include <csignal>

using namespace stud::jni_bridge;

namespace {

__attribute__((noinline)) void aborts() { std::raise(SIGABRT); }
__attribute__((noinline)) void returns() { asm(""); }

__attribute__((noinline)) void inside_the_outer_bracket(sigjmp_buf* outer) {
    check(call_trapping_abort(returns), "an inner call that returns reports that");
    check(current_abort_trap().checkpoint == outer, "and the outer bracket is still armed");

    check(!call_trapping_abort(aborts), "an inner call that aborts is trapped");
    check(current_abort_trap().checkpoint == outer, "and it gives the outer bracket back");

    int result = 0;
    const auto gives_seven = [] { return 7; };
    check(call_trapping_abort_with_result(+gives_seven, result) && result == 7,
          "a call with a result hands it over");
    check(!call_trapping_abort_tolerating_wild_sigsegv(aborts), "a tolerant inner call traps too");
    check(current_abort_trap().checkpoint == outer, "and leaves the outer bracket armed as well");
    check(!current_abort_trap().tolerate_wild_sigsegv, "with the outer bracket's own tolerance");
}

}  // namespace

int main() {
    check(current_abort_trap().checkpoint == nullptr, "nothing is armed to begin with");
    sigjmp_buf outer;
    const TrapArm previous = current_abort_trap();
    if (sigsetjmp(outer, 1) != 0) check(false, "the outer bracket should never be jumped to");
    arm_abort_trap(outer);
    inside_the_outer_bracket(&outer);
    disarm_abort_trap(previous);
    check(current_abort_trap().checkpoint == nullptr, "and disarming leaves nothing armed");
    return 0;
}
