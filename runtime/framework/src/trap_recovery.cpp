#include "stud/trap_recovery.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <string>

#include <jnivm/object.h>
#include <jnivm/throwable.h>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pthread.h>
#include <atomic>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

namespace stud::jni_bridge {

namespace {
// Set once by note_shutting_down(); read only from a signal handler.
std::atomic<bool> g_shutting_down{false};
}  // namespace

void note_shutting_down() { g_shutting_down.store(true, std::memory_order_relaxed); }

}  // namespace stud::jni_bridge

// Resolved by name from the render client, which is compiled into five
// separate shared objects and cannot link against this executable. See
// Client::report_death() for why it is called from there.
extern "C" void stud_note_shutting_down() { stud::jni_bridge::note_shutting_down(); }

namespace stud::jni_bridge {

namespace {


// Thread-local, not a plain global, only the thread that armed a
// checkpoint gets its SIGTRAP/SIGABRT/SIGSEGV redirected; a signal
// delivered to a different thread while this is armed sees a null
// checkpoint here and falls through to real default behavior.
thread_local sigjmp_buf* g_armed_checkpoint = nullptr;

// Opt-in, per-thread relaxation of the stack-overflow-shaped SIGSEGV
// heuristic below. Deliberately narrow and explicit at the call site
// (arm_abort_trap_tolerating_wild_sigsegv()) rather than loosening the
// heuristic itself: the delta check exists to keep genuinely wild/null-
// pointer bugs fatal (so they get root-caused, not silently papered
// over) everywhere except the few call sites, like a best-effort
// background probe of not-yet-fully-wired Roblox internals, where a
// crash is expected to be survivable and shouldn't take the whole
// process down.
thread_local bool g_tolerate_wild_sigsegv = false;

// 512KiB per-thread alt signal stack, needed because the SIGSEGV
// recovery case (a stack-overflow guard-page hit) means the faulting
// thread's own stack is, by definition, exhausted, the handler can't
// run on it without immediately double-faulting.
thread_local void* g_sigaltstack_mem = nullptr;

void ensure_sigaltstack_for_current_thread() {
    constexpr size_t kAltStackSize = 1 << 19;
    if (g_sigaltstack_mem == nullptr) {
        // Deliberately never freed, one small allocation per thread
        // for the process's whole life.
        g_sigaltstack_mem = std::malloc(kAltStackSize);
    }
    stack_t ss{};
    ss.ss_sp = g_sigaltstack_mem;
    ss.ss_size = kAltStackSize;
    ss.ss_flags = 0;
    // Real fix for a genuine bug: siglongjmp() out of a handler running
    // on the altstack never goes through a real sigreturn, so the
    // kernel's own per-thread SS_ONSTACK bookkeeping is never cleared.
    // Every subsequent signal on that thread would then be delivered on
    // whatever the CURRENT (possibly exhausted) stack is instead of the
    // dedicated altstack. Re-issuing sigaltstack() on every arm (cheap,
    // no allocation) clears the stale flag each time.
    ::sigaltstack(&ss, nullptr);
}

// In-process diagnostic that doesn't depend on a debugger ever attaching
// correctly to this process: this session's own investigation found
// that an external debugger never catches this binary's own exec, because
// real bionic's linker64 loads it via its own internal ELF-mapping
// logic (the same code path as a manual dlopen()), never a second,
// real execve(), so a debugger only ever sees linker64's own (symbol-less)
// image, and named/source breakpoints for this binary's own code never
// resolve. Rather than keep fighting that, this reads /proc/self/maps
// directly (plain, synchronous file I/O, not strictly async-signal-
// safe, but this handler already accepts that same tradeoff for
// snprintf() above; a "good enough for a diagnostic that's about to
// crash the process anyway" call, not production-hardened code) and
// resolves a raw address to "which mapped file + what offset into it",
// the same real, load-bearing signal an offline look
// already used successfully (cross-referencing a raw file
// offset against the ELF headers and symbols), so a wild/fatal crash's
// real origin can be root-caused offline afterward without needing a
// live debugger session at all.
void describe_address(const char* label, uintptr_t addr) {
    // A process with libroblox.so (116MB+) and its full dependency
    // closure mapped has a /proc/self/maps well past a few KB, a real,
    // confirmed-live bug this session: an earlier 4KB stack buffer
    // silently truncated before reaching the mappings this actually
    // needed to resolve, so every real address came back "no mapping
    // found". `static`, not stack-local, so this doesn't eat into the
    // handler's own (size-limited) alt signal stack.
    static char buf[1 << 20];  // 1MiB
    char small[192];
    int fd = ::open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        int n = std::snprintf(small, sizeof(small),
                               "STUD_TRAP:   %s=0x%lx (failed to open /proc/self/maps)\n", label,
                               static_cast<unsigned long>(addr));
        ::syscall(SYS_write, 2, small, n > 0 ? n : 0);
        return;
    }
    ssize_t total = 0;
    ssize_t n;
    while (total < static_cast<ssize_t>(sizeof(buf)) - 1 &&
           (n = ::read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += n;
    }
    ::close(fd);
    buf[total > 0 ? total : 0] = '\0';

    // Parse each line: "start-end perms offset dev inode path".
    const char* line = buf;
    while (line != nullptr && *line != '\0') {
        const char* eol = std::strchr(line, '\n');
        unsigned long start = 0, end = 0, file_off = 0;
        char path[512] = {};
        // sscanf is not async-signal-safe either, same accepted
        // tradeoff noted above.
        std::sscanf(line, "%lx-%lx %*4s %lx %*s %*s %511s", &start, &end, &file_off, path);
        if (addr >= start && addr < end) {
            unsigned long lib_relative_off = file_off + (addr - start);
            char out[640];
            int len = std::snprintf(out, sizeof(out), "STUD_TRAP:   %s=0x%lx -> %s + 0x%lx\n", label,
                                     static_cast<unsigned long>(addr), path, lib_relative_off);
            ::syscall(SYS_write, 2, out, len > 0 ? len : 0);
            return;
        }
        line = eol != nullptr ? eol + 1 : nullptr;
    }
    int len = std::snprintf(small, sizeof(small), "STUD_TRAP:   %s=0x%lx (no mapping found)\n", label,
                             static_cast<unsigned long>(addr));
    ::syscall(SYS_write, 2, small, len > 0 ? len : 0);
}

// Same real /proc/self/maps parse as describe_address(), but just a
// yes/no answer, used to gate a real memory dump (below) to only
// addresses already confirmed real and mapped, never a blind
// dereference of stack garbage.
bool addr_is_mapped(uintptr_t addr) {
    static char buf[1 << 20];
    int fd = ::open("/proc/self/maps", O_RDONLY);
    if (fd < 0) return false;
    ssize_t total = 0;
    ssize_t n;
    while (total < static_cast<ssize_t>(sizeof(buf)) - 1 &&
           (n = ::read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
        total += n;
    }
    ::close(fd);
    buf[total > 0 ? total : 0] = '\0';
    const char* line = buf;
    while (line != nullptr && *line != '\0') {
        const char* eol = std::strchr(line, '\n');
        unsigned long start = 0, end = 0;
        std::sscanf(line, "%lx-%lx", &start, &end);
        if (addr >= start && addr < end) return true;
        line = eol != nullptr ? eol + 1 : nullptr;
    }
    return false;
}

// Frame-pointer walk from the fault context's own RBP; real, not a
// guess: Debug builds (this project's own default, -O0) preserve real
// frame pointers, so [rbp] = saved caller RBP and [rbp+8] = real return
// address, standard x86-64 SysV convention. Bounded and defensive (each
// step's rbp must strictly increase and land in a plausible stack
// range) since a corrupted/optimized frame can break this chain:
// real, useful data as far as it goes, not proof beyond that point.
void print_backtrace(uintptr_t start_rbp) {
    uintptr_t rbp = start_rbp;
    for (int i = 0; i < 24 && rbp != 0; ++i) {
        // Bug found in testing (the engineering notes), same class as the
        // one already fixed in stud_trap_handler()'s own near_null rbp
        // dump loop: this doc comment used to claim "if this faults...
        // a hard crash simply ends the walk, which is an acceptable,
        // honest stopping point"; that's wrong. A second SIGSEGV
        // while still inside this signal handler, with SIGSEGV blocked
        // (sigaction's default), does not "end the walk"; it
        // terminates the whole process, confirmed live via a real KDE
        // crash-handler notification for exactly this call site on a
        // corrupted-past-some-depth frame chain (the LuaAppDM/
        // initializeLuaAppWithLoggedInUser crash's own real stack
        // shape). addr_is_mapped() (already used elsewhere in this
        // file for the identical reason) makes this safe.
        if (!addr_is_mapped(rbp) || !addr_is_mapped(rbp + sizeof(uintptr_t))) break;
        volatile uintptr_t* p = reinterpret_cast<uintptr_t*>(rbp);
        uintptr_t saved_rbp = p[0];
        uintptr_t return_addr = p[1];
        if (return_addr == 0) break;
        char label[32];
        std::snprintf(label, sizeof(label), "frame[%d]", i);
        describe_address(label, return_addr);
        if (saved_rbp <= rbp) break;  // must strictly grow toward higher addresses
        rbp = saved_rbp;
    }
}

std::atomic<StudWriteFaultHandler> g_write_fault_handler{nullptr};

extern "C" void stud_set_write_fault_handler(StudWriteFaultHandler handler) {
    g_write_fault_handler.store(handler, std::memory_order_release);
}

extern "C" void stud_trap_handler(int sig, siginfo_t* info, void* ucontext_raw) {
    // Before anything else, including the breadcrumb below: is this an
    // expected write-barrier fault rather than a crash at all?
    //
    // The render client write-protects its mapped-memory staging pages
    // so the MMU reports which ones the engine wrote (see
    // render-client/src/mapped_write_barrier.h). Those faults are
    // routine and can run into the tens of thousands per minute, so
    // this has to come first, classifying one as a crash killed the
    // render thread (an invisible window), and merely logging one is
    // enough to drown the log and the frame budget.
    if (sig == SIGSEGV) {
        auto write_fault = g_write_fault_handler.load(std::memory_order_acquire);
        if (write_fault != nullptr && info != nullptr && info->si_addr != nullptr &&
            write_fault(info->si_addr)) {
            return;
        }
    }

    // Temporary raw-syscall breadcrumb (bypasses stdio buffering
    // entirely, so it's visible even if the process dies immediately
    // after), confirms definitively whether this handler is even
    // entered for a given crash, vs. rejected by the SIGSEGV delta
    // heuristic below, vs. never installed/overwritten by something
    // else (e.g. libroblox.so's own bundled Crashpad). Remove once the
    // real crash this session is chasing is root-caused.
    {
        char buf[160];
        int n = std::snprintf(buf, sizeof(buf), "STUD_TRAP: entered sig=%d armed=%d tid=%ld\n", sig,
                               g_armed_checkpoint != nullptr, static_cast<long>(::syscall(SYS_gettid)));
        ::syscall(SYS_write, 2, buf, n > 0 ? n : 0);
    }

    if (sig == SIGSEGV) {
        // Two independent, evidence-based recoverable shapes:
        //   - a fault landing close to the current stack pointer (a
        //     characterized stack-overflow-in-Roblox's-own-code shape).
        //   - a fault reading through a genuinely null (or near-null)
        //     pointer (fault_addr under one page): a well-understood,
        //     narrow crash class distinct from an arbitrary wild write
        //     elsewhere in memory; real, confirmed-live instance root-
        //     caused by reading the real
        //     libroblox.so (see engine_v2_bridge.cpp's
        //     nativeAppBridgeV2StartAppWithParams call site): a null
        //     object read inside a non-essential internal telemetry-
        //     logging helper. Global rather than the thread-scoped
        //     opt-in this used to be, confirmed via a real, reproduced
        //     run that Roblox's own call crashes on a different,
        //     Roblox-spawned worker thread than the one that armed the
        //     checkpoint, so a thread_local "tolerate" flag set on the
        //     calling thread never actually applies at the real fault
        //     site.
        //   Anything else (a real wild-pointer bug, neither stack-
        //   adjacent nor near-null) falls straight through to the
        //   default, fatal disposition below.
        auto* uctx = static_cast<ucontext_t*>(ucontext_raw);
        auto fault_rsp = static_cast<uintptr_t>(uctx->uc_mcontext.gregs[REG_RSP]);
        auto fault_addr = reinterpret_cast<uintptr_t>(info->si_addr);
        uintptr_t stack_delta = fault_addr > fault_rsp ? fault_addr - fault_rsp : fault_rsp - fault_addr;
        bool near_stack = stack_delta <= (1u << 20);
        // Gap found in testing (the engineering notes, "the new, current,
        // fully deterministic blocker" entry): a real, ordinary null
        // `this` deref through a LARGE struct-field offset, confirmed
        // live: a member read through a null `this`,
        // faults at addr=0x115c8 (71624), well past the old 4096-byte
        // threshold, so this whole crash class fell through to the
        // fatal, unrecovered disposition below instead of getting the
        // same safe per-thread recovery every other near-null fault
        // already gets. Real C++ objects in this binary are large enough
        // that a null-base deref can land anywhere up to a few hundred
        // KB past 0; bumped to match the already-established near_stack
        // magnitude (1MB) in this same file; every real, live-observed
        // heap/ASLR pointer in this process sits at 0x7xxx_xxxx_xxxx or
        // above, so there's no real risk of misclassifying a genuine
        // wild pointer this way.
        bool near_null = fault_addr < (1u << 20);
        {
            char buf[192];
            int n = std::snprintf(
                buf, sizeof(buf), "STUD_TRAP: sigsegv addr=%p rsp=%p near_stack=%d near_null=%d\n",
                info->si_addr, reinterpret_cast<void*>(fault_rsp), near_stack, near_null);
            ::syscall(SYS_write, 2, buf, n > 0 ? n : 0);
        }
        // Prefer letting the faulting instruction SUCCEED over unwinding out
        // of it. A null-base read is what a real device produces for an empty
        // slot in one of the engine's own handler tables, and the engine's
        // very next instruction branches on the value being zero, so mapping
        // a read-only zero page under the fault and retrying is closer to real
        // behaviour than abandoning the call.
        //
        // Live-caught reason this exists: pressing Play walks an 8-slot
        // broadcast table, matches a legitimately-empty slot, and calls it with
        // a null `this`. Recovering by unwinding
        // yanked the engine out of submitStartGameTask on its own thread, and
        // Stud froze with the game half-started. Reading zero there lets the
        // engine take its "no handler" branch and carry on.
        //
        // Deliberately narrow: reads only (a second fault on the same page is
        // a write, and falls through to the old behaviour), only in the
        // near-null range, only above the kernel's own mmap_min_addr, and each
        // page announced once. It cannot hide a wild pointer; those are not
        // near-null, but it does turn a genuine null-object READ into a zero,
        // which is a real trade and the reason it says so out loud.
        if (near_null) {
            const uintptr_t page_size = 4096;
            const uintptr_t page = fault_addr & ~(page_size - 1);
            // The kernel refuses anything below mmap_min_addr; 0x10000 is the
            // standard value and re-reading /proc here is not async-signal-safe.
            if (page >= 0x10000) {
                static std::atomic<uintptr_t> mapped_pages[16];
                bool already = false;
                for (auto& slot : mapped_pages) {
                    if (slot.load(std::memory_order_relaxed) == page) already = true;
                }
                if (!already) {
                    long r = ::syscall(SYS_mmap, reinterpret_cast<void*>(page), page_size,
                                       PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                    if (r == static_cast<long>(page)) {
                        for (auto& slot : mapped_pages) {
                            uintptr_t expected = 0;
                            if (slot.compare_exchange_strong(expected, page)) break;
                        }
                        char buf[192];
                        int n = std::snprintf(
                            buf, sizeof(buf),
                            "STUD_TRAP: near-null READ at %p, mapped a zero page and retrying "
                            "(the engine's own empty-handler-slot path; see trap_recovery.cpp)\n",
                            info->si_addr);
                        ::syscall(SYS_write, 2, buf, n > 0 ? n : 0);
                        return;  // retry the faulting instruction
                    }
                }
            }
        }

        if (near_null) {
            // Recovered, but still real, useful evidence of exactly
            // where, printed even on the recovered path (not just the
            // fatal one below) since a near-null fault is rare enough
            // not to spam, and knowing precisely which real libroblox.so
            // function faulted (not just "some near-null SIGSEGV
            // happened") is exactly the kind of real evidence this
            // project's own debugging discipline calls for instead of
            // guessing.
            auto* uctx2 = static_cast<ucontext_t*>(ucontext_raw);
            auto fault_pc = static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RIP]);
            describe_address("pc", fault_pc);

            // A `call *reg` through a NULL pointer faults with pc==0
            // *after* the CPU has already pushed the return address,
            // so the top of the stack is the instruction immediately
            // following the faulting indirect call, i.e. the real call
            // site. The rbp chain below cannot show this (the callee
            // never got to build a frame), so read it directly. This is
            // the only way to name which indirect call went through a
            // null pointer; guarded by addr_is_mapped() like every
            // other dereference in this signal handler.
            if (fault_pc == 0 && addr_is_mapped(fault_rsp)) {
                auto pushed_return = *reinterpret_cast<uintptr_t*>(fault_rsp);
                describe_address("null-call-site (return addr at *rsp)", pushed_return);
            }

            print_backtrace(static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RBP]));

            // Live stack scan for the caller's own saved callee-
            // registers (x86-64 SysV: `push` after `push rbp; mov
            // rsp,rbp` stores each at successive rbp-8, rbp-16, ...),
            // safe (only reads already-valid stack memory, no risk of a
            // secondary fault) and far more grounded than trying to
            // reason about which register held what from a static
            // reading it alone (the analysis's own control-flow simplification
            // for this exact function produced explicit "removing
            // unreachable block"/"possible PIC construction" warnings
            // this session, not fully trustworthy here). Each slot is
            // run through describe_address(): a real heap/lib pointer
            // resolves to a real mapped region, stack garbage or a
            // small integer won't: lets real evidence, checked,
            // say which saved register is the struct pointer whose
            // field faulted.
            // Bug found in testing (the engineering notes): this loop used
            // to dereference `stack_addr` with no bounds check at all,
            // unlike the `value` read a few lines below it, which
            // already correctly gates on addr_is_mapped(). If `rbp`
            // itself was corrupted by the original fault, this raw read
            // could fault a SECOND time while still inside the signal
            // handler, and a second SIGSEGV while the first is being
            // handled is unrecoverable: the process dies outright,
            // before ever reaching the real siglongjmp() recovery below.
            // This silently defeated trap_recovery's own entire purpose
            // for any crash where rbp wasn't a valid stack address:
            // confirmed live: a real, known, "recovered" crash
            // (initializeLuaAppWithLoggedInUser's null-field fault) was
            // actually taking the whole process down every single time,
            // despite the handler printing "armed=1" and appearing to
            // proceed normally right up to this point.
            auto rbp = static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RBP]);
            for (int slot = 1; slot <= 8; ++slot) {
                uintptr_t stack_addr = rbp - static_cast<uintptr_t>(slot) * 8;
                if (!addr_is_mapped(stack_addr)) continue;
                auto value = *reinterpret_cast<uintptr_t*>(stack_addr);
                char label[32];
                std::snprintf(label, sizeof(label), "rbp-%d", slot * 8);
                describe_address(label, value);

                // Live struct-field dump for anything that
                // resolved to real, mapped memory, reads are safe
                // (addr_is_mapped() already confirmed it; heap pages
                // are contiguous readable memory even a bit past one
                // logical allocation's own bounds). Prints 0x400..0x448
                // as raw 8-byte fields so the *actual* live value at
                // the offset that faulted (0x430) is visible directly,
                // not inferred from reading the code.
                if (value != 0 && addr_is_mapped(value + 0x448)) {
                    for (uintptr_t off = 0x400; off <= 0x448; off += 8) {
                        auto field = *reinterpret_cast<uintptr_t*>(value + off);
                        char fbuf[64];
                        int fn = std::snprintf(fbuf, sizeof(fbuf),
                                                "STUD_TRAP:     [%s+0x%lx] = 0x%lx\n", label, off, field);
                        ::syscall(SYS_write, 2, fbuf, fn > 0 ? fn : 0);
                    }
                }
            }
        }
        // Bug found in testing: g_tolerate_wild_sigsegv (set by
        // arm_abort_trap_tolerating_wild_sigsegv(), see its own doc
        // comment) was defined and armed/disarmed correctly but never
        // actually CONSULTED anywhere in this classification, a
        // genuinely wild-shaped fault (neither near_stack nor near_null)
        // always took the unconditional fatal exit below regardless of
        // whether the calling thread had opted in to tolerate exactly
        // this. Confirmed live via the real "instantiate controllers"
        // crash (the engineering notes): a real, evidence-confirmed (CFI +
        // the engine's own code) C++ exception-unwind landing pad on a
        // Roblox-spawned background worker thread, garbage %rax, neither
        // near_stack nor near_null, exactly the class this opt-in
        // exists for, silently unable to ever take effect.
        if (!near_stack && !near_null && !g_tolerate_wild_sigsegv) {
            auto* uctx2 = static_cast<ucontext_t*>(ucontext_raw);
            describe_address("pc", static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RIP]));
            print_backtrace(static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RBP]));
            ::signal(sig, SIG_DFL);
            ::raise(sig);
            return;
        }
        if (g_tolerate_wild_sigsegv && !near_stack && !near_null) {
            char buf[96];
            int n = std::snprintf(buf, sizeof(buf),
                                   "STUD_TRAP: wild-shaped fault tolerated (opted in), "
                                   "attempting recovery\n");
            ::syscall(SYS_write, 2, buf, n > 0 ? n : 0);
        }
    }

    if (g_armed_checkpoint != nullptr) {
        sigjmp_buf* target = g_armed_checkpoint;
        g_armed_checkpoint = nullptr;
        siglongjmp(*target, 1);
    }

    // Caught in testing: structural gap (the engineering notes): g_armed_
    // checkpoint is thread_local by design (a checkpoint only makes
    // sense to resume on the same thread's own stack, siglongjmp
    // across threads is not something sigsetjmp/siglongjmp support at
    // all, it would resume a different thread's saved register/stack
    // state while running on this thread's signal stack, corrupting
    // both). But several real, already-observed near_null crashes
    // (initializeLuaAppWithLoggedInUser, the UpdateSurfaceApp/Game
    // async handler) fault on Roblox's own internal "RBX Main
    // task-queue worker" thread, a thread OUR code never called
    // call_trapping_abort()/run_bounded_v2_call() on directly, so it
    // never armed a checkpoint of its own. Confirmed live: this exact
    // path used to fall through to the default SIGSEGV disposition
    // below, which is fatal for the *whole process* (verified via a
    // real KDE crash-handler notification for the crashing thread),
    // not just the one worker thread, silently defeating recovery
    // for every async, off-calling-thread instance of this crash
    // class, independent of the earlier unbounded-stack-read fix.
    //
    // Pragmatic fix: for a fault already classified recoverable-
    // shaped (near_null) but with no per-thread checkpoint to jump
    // back to, terminate only the faulting thread (pthread_exit(),
    // never returning) instead of the whole process. This is the same
    // real tradeoff Android's own crash-reporting infra and other
    // real signal-handler-based recovery systems make: pthread_exit()
    // is not strictly on POSIX's async-signal-safe list, but the
    // alternative here is a guaranteed, total process death for a
    // fault this project has already root-caused as "a queued async
    // task's own null-field bug," not a corrupted process. A real
    // device survives losing one internal worker thread; it does not
    // survive this process dying outright.
    if (sig == SIGSEGV) {
        auto* uctx3 = static_cast<ucontext_t*>(ucontext_raw);
        auto fault_addr3 = reinterpret_cast<uintptr_t>(info->si_addr);
        if (fault_addr3 < (1u << 12)) {
            char buf[128];
            int n = std::snprintf(buf, sizeof(buf),
                                   "STUD_TRAP: no checkpoint on this thread for a near_null "
                                   "fault, exiting only this thread, not the process\n");
            ::syscall(SYS_write, 2, buf, n > 0 ? n : 0);
            (void)uctx3;
            ::pthread_exit(nullptr);
        }
    }

    // Nothing armed on the receiving thread, and not a recognized
    // recoverable shape, print real diagnostics (PC + a frame-pointer
    // backtrace, both resolved to "which mapped file + what offset" via
    // /proc/self/maps, see describe_address()'s own doc comment for why
    // this exists instead of relying on an external debugger) before restoring this
    // signal's real default disposition and re-raising, so a genuinely
    // unexpected trap/abort behaves exactly as it would with no handler
    // installed, rather than looping back into this same handler
    // forever.
    {
        auto* uctx2 = static_cast<ucontext_t*>(ucontext_raw);
        describe_address("pc", static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RIP]));
        print_backtrace(static_cast<uintptr_t>(uctx2->uc_mcontext.gregs[REG_RBP]));
    }
    // Shutting down: a fault here IS the teardown.
    //
    // Closing Stud runs LeaveGame/DestroyApp while the engine's own ~19
    // threads are still live, so one of them touching a DataModel being
    // destroyed underneath it is expected, not a defect. Re-raising made
    // systemd write a 178 MB core dump of a process one line from
    // _exit(0), reported as "random linker64 errors when I close Stud",
    // with si_code SI_TKILL naming this raise() as the source rather than
    // any hardware fault.
    //
    // Only the fatal path is short-circuited: a recoverable fault during
    // teardown still recovers, so shutdown work that can finish, does.
    if (g_shutting_down.load(std::memory_order_relaxed)) {
        ::_exit(0);
    }
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// Confirmed-live reason this re-installs on EVERY arm rather than
// once (std::call_once, this file's own earlier version): libroblox.so
// bundles Google's Crashpad crash-reporting library (confirmed via its
// own string table, ".../CrashCallback/android/
// ClientCrashpadCallback.inl"), Crashpad installs its OWN SIGSEGV/
// SIGABRT/SIGTRAP/etc handlers, lazily, likely triggered by one of the
// real JNI calls already observed succeeding (nativeAppBridgeAppStart,
// JNI_OnLoad), i.e. chronologically AFTER this handler's own one-time
// install. Whichever sigaction() call happens LAST wins (plain
// replacement, no chaining), Crashpad's later install silently
// overwrites this one, matching the real, observed symptom (a crash
// that should have been trapped instead terminates the process
// unrecovered, with "libc: failed to connect to tombstoned", bionic's
// own separate, additional crash-reporting attempt, also visible).
// Re-installing immediately before every risky call (cheap: three
// sigaction() syscalls, same "re-arm every time" pattern already used
// for the per-thread altstack above) guarantees this handler is the
// active one at the moment each call actually runs, regardless of
// what Crashpad (or anything else) installs in between.
void ensure_trap_handler_installed() {
    struct sigaction sa {};
    sa.sa_sigaction = &stud_trap_handler;
    ::sigemptyset(&sa.sa_mask);
    // SA_ONSTACK: harmless for SIGTRAP/SIGABRT, load-bearing for
    // SIGSEGV (the faulting thread's own stack may be exhausted).
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGTRAP, &sa, nullptr);
    ::sigaction(SIGSEGV, &sa, nullptr);
}

}  // namespace

void arm_abort_trap(sigjmp_buf& checkpoint) {
    ensure_trap_handler_installed();
    ensure_sigaltstack_for_current_thread();
    g_armed_checkpoint = &checkpoint;
}

void arm_abort_trap_tolerating_wild_sigsegv(sigjmp_buf& checkpoint) {
    arm_abort_trap(checkpoint);
    g_tolerate_wild_sigsegv = true;
}

void disarm_abort_trap() {
    g_armed_checkpoint = nullptr;
    g_tolerate_wild_sigsegv = false;
}

bool clear_pending_jni_exception(JNIEnv* env, const char* context) {
    if (env == nullptr || !env->ExceptionCheck()) {
        return false;
    }
    // Name the exception before dropping it. Silently clearing hid a
    // real, load-bearing failure for a long time: every Djinni
    // `setPlatformImpl()` call was recorded as succeeding while actually
    // throwing here, so no platform implementation was ever installed
    // (the engineering notes). An exception worth clearing is worth reading.
    jthrowable pending = env->ExceptionOccurred();
    std::string detail;
    if (pending != nullptr) {
        // jnivm's own ThrowNew() stores the real message as a
        // std::exception_ptr on its Throwable (vm.cpp), not as a Java
        // field, so rethrow-and-catch is the only way to read it.
        // Worth the trouble: this is the message libroblox's own Djinni
        // glue passes when it refuses to install a platform impl.
        auto* obj = reinterpret_cast<jnivm::Object*>(pending);
        auto* throwable = dynamic_cast<jnivm::Throwable*>(obj);
        if (throwable != nullptr && throwable->except) {
            try {
                std::rethrow_exception(throwable->except);
            } catch (const std::exception& e) {
                detail = e.what();
            } catch (...) {
                detail = "(non-std exception)";
            }
        }
    }
    std::fprintf(stderr,
                  "stud: %s: clearing a pending JNI exception left over from this call%s%s\n",
                  context, detail.empty() ? "" : ", message: ", detail.c_str());
    env->ExceptionClear();
    return true;
}

}  // namespace stud::jni_bridge
