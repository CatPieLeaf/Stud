#pragma once

#include <cstdint>

// x86-64 CPU feature compat shim.
//
// Installs a SIGILL handler, detects available CPU features via CPUID at
// startup, and emulates instructions the host CPU lacks in software when
// the engine hits them. Independent of Roblox's own code -- libroblox.so
// contains no CPU-feature-check strings of its own (confirmed by static
// analysis), so this is purely Stud's responsibility, matching the same
// SIGILL-handler-based approach both Sober (`libbadcpu`) and mcpelauncher's
// reference architecture document (see the engineering notes' "Core technical
// insight").
//
// Real, tested scope for this pass: CPUID feature detection (SSSE3/
// SSE4.1/SSE4.2/POPCNT -- the exact requirement list both Sober and
// mcpelauncher-manifest independently document), a minimum-requirement
// fatal check (SSE4.1, matching documented precedent), and real decode +
// emulation for the POPCNT instruction specifically -- the one most likely
// to actually be hit in practice, since compilers emit it directly for
// __builtin_popcount()/std::popcount() and it's the headline instruction
// in both projects' stated CPU requirements. Other instructions (MOVBE,
// LZCNT, TZCNT, BMI1 ANDN/BLSI/BLSMSK/BLSR) are NOT emulated yet -- the
// handler reports them clearly and aborts rather than silently
// misbehaving, growing iteratively against real SIGILL traps the same way
// the rest of this project has, rather than guessing a full instruction
// set upfront.
//
// Honest testing limitation: this development host has every relevant ISA
// extension (SSSE3/SSE4.1/SSE4.2/POPCNT all present, confirmed via CPUID),
// so a *real* SIGILL from missing hardware can't be triggered here. Decode
// and emulation logic is unit-tested directly as pure functions against
// real POPCNT instruction bytes (not through an actual trap); the signal
// handler installation/uninstallation is tested separately. True
// end-to-end "host lacks POPCNT, SIGILL fires, gets emulated correctly" is
// unverified on this machine -- would need actual older/restricted
// hardware, or QEMU with CPU features masked, to exercise for real.
//
// See the engineering notes, milestone M5.

namespace stud::cpu_compat {

struct CpuFeatures {
    bool ssse3 = false;
    bool sse4_1 = false;
    bool sse4_2 = false;
    bool popcnt = false;
};

// Real CPUID leaf 1 ECX bit checks (bit 9/19/20/23 respectively -- standard
// Intel/AMD-documented positions, not guessed).
CpuFeatures detect_cpu_features();

// Matches Sober/mcpelauncher's documented minimum requirement. Returns
// false if the host CPU can't run Stud at all (no software fallback for
// pervasively-used SSE4.1 is practical -- unlike POPCNT, which shows up in
// isolated spots).
bool meets_minimum_requirements(const CpuFeatures& features);

// Installs the SIGILL handler. Returns false if sigaction() itself failed
// (a real OS-level error, not a CPU-feature question).
bool install_sigill_handler();

// Restores the default SIGILL disposition -- mainly for tests that need a
// clean slate between cases.
void uninstall_sigill_handler();

}  // namespace stud::cpu_compat
