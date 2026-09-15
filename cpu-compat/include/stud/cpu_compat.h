#pragma once

#include <cstdint>

// x86-64 CPU feature compat shim.
//
// Installs a SIGILL handler, detects available CPU features via CPUID at
// startup, and emulates instructions the host CPU lacks in software when
// the engine hits them. Independent of Roblox's own code, libroblox.so
// contains no CPU-feature-check strings of its own (confirmed by static
// analysis), so this is purely Stud's responsibility, matching the same
// SIGILL-handler-based approach both Sober (`libbadcpu`) and mcpelauncher's
// reference architecture document (see the engineering notes' "Core technical
// insight").
//
// Tested scope for this pass: CPUID feature detection (SSSE3/
// SSE4.1/SSE4.2/POPCNT, the exact requirement list both Sober and
// mcpelauncher-manifest independently document), a minimum-requirement
// fatal check (SSE4.1, matching documented precedent), and real decode +
// emulation for the POPCNT instruction specifically, the one most likely
// to actually be hit in practice, since compilers emit it directly for
// __builtin_popcount()/std::popcount() and it's the headline instruction
// in both projects' stated CPU requirements: and BMI1's ANDN, BLSR,
// BLSMSK and BLSI, which compilers emit freely for ordinary bit
// arithmetic and which no CPU before roughly 2013 has.
//
// A note on those four, because getting them subtly wrong is worse than
// not having them: their operands live in VEX.vvvv (stored
// one's-complemented), and BLSR/BLSMSK/BLSI share a single opcode,
// separated only by ModRM.reg. Decode either detail wrongly and they
// return plausible numbers that are simply incorrect, which the engine
// then carries onward: no crash, no signal, just wrong results. That is
// why they are unit-tested against real encodings and real expected
// values rather than only for "did it decode".
//
// MOVBE is emulated too, including its memory addressing (ModRM/SIB/
// disp8/disp32/RIP-relative). It exists only in load and store forms,
// so there was nothing to emulate without that.
//
// LZCNT and TZCNT are NOT emulated, and CANNOT BE by this mechanism.
// This is worth stating plainly, because it looks like an omission and is
// not one:
//
//   lzcnt  ->  f3 0f bd    bsr  ->  0f bd
//   tzcnt  ->  f3 0f bc    bsf  ->  0f bc
//
// They are BSR/BSF with an 0xF3 prefix. A CPU without LZCNT/TZCNT does not
// reject those encodings. It IGNORES the prefix and executes BSR/BSF.
// No #UD, no SIGILL, nothing for a signal handler to catch. Emulating
// them would mean finding and rewriting the instructions in the loaded
// image, which is the byte-patching this project does not do.
//
// The practical consequence, stated honestly: on a CPU lacking them,
// TZCNT and BSF agree for every non-zero input and differ only at zero
// (TZCNT returns the operand width; BSF leaves the destination
// undefined), so the exposure is narrow. LZCNT and BSR disagree for all
// inputs, BSR returns the bit index, LZCNT the count of leading zeros
// so an engine built with LZCNT would compute wrong values silently on
// such a CPU. Nothing in Stud can detect that, and pretending otherwise
// would be worse than saying so here.
//
// Everything else (the rest of BMI2, AVX, and so on) is still NOT
// emulated, the handler reports the instruction clearly and aborts
// rather than silently misbehaving, growing iteratively against real
// SIGILL traps the same way the rest of this project has, rather than
// guessing a full instruction set upfront.
//
// Honest testing limitation: this development host has every relevant ISA
// extension (SSSE3/SSE4.1/SSE4.2/POPCNT all present, confirmed via CPUID),
// so a *real* SIGILL from missing hardware can't be triggered here. Decode
// and emulation logic is unit-tested directly as pure functions against
// real POPCNT instruction bytes (not through an actual trap); the signal
// handler installation/uninstallation is tested separately. True
// end-to-end "host lacks POPCNT, SIGILL fires, gets emulated correctly" is
// unverified on this machine, would need actual older/restricted
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

// Real CPUID leaf 1 ECX bit checks (bit 9/19/20/23 respectively, standard
// Intel/AMD-documented positions, checked).
CpuFeatures detect_cpu_features();

// Matches Sober/mcpelauncher's documented minimum requirement. Returns
// false if the host CPU can't run Stud at all (no software fallback for
// pervasively-used SSE4.1 is practical, unlike POPCNT, which shows up in
// isolated spots).
bool meets_minimum_requirements(const CpuFeatures& features);

// Installs the SIGILL handler. Returns false if sigaction() itself failed
// (a real OS-level error, not a CPU-feature question).
bool install_sigill_handler();

// Restores the default SIGILL disposition, mainly for tests that need a
// clean slate between cases.
void uninstall_sigill_handler();

}  // namespace stud::cpu_compat
