#include "mapped_write_barrier.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace stud::render_client {

namespace {

std::size_t page_size() {
    static const std::size_t sz = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return sz;
}

// Fixed-capacity registry. A signal handler must not take a lock that
// ordinary code could already hold, and must not allocate, so this is
// a flat array of atomics scanned linearly. The engine keeps a handful
// of mappings live at a time, so the scan is short.
constexpr int kMaxBarriers = 256;
std::atomic<MappedWriteBarrier*> g_barriers[kMaxBarriers];

std::atomic<bool> g_installed{false};

// Plain, non-atomic on purpose: written from the signal handler, where
// the existing dirty-page bookkeeping is already non-atomic, and read
// only by a diagnostic. A lost increment costs an inexact count.
unsigned long long g_fault_count = 0;

// Called by Stud's own trap handler before it classifies anything as a
// crash; see stud_set_write_fault_handler in trap_recovery.h for why
// chaining sigaction from here is not enough.
bool dispatch_write_fault(void* addr) {
    for (auto& slot : g_barriers) {
        MappedWriteBarrier* barrier = slot.load(std::memory_order_acquire);
        if (barrier != nullptr && barrier->handle_write_fault(addr)) return true;
    }
    return false;
}

}  // namespace

MappedWriteBarrier::~MappedWriteBarrier() { release(); }

MappedWriteBarrier& MappedWriteBarrier::operator=(MappedWriteBarrier&& other) noexcept {
    if (this == &other) return *this;
    release();
    base_ = other.base_;
    size_ = other.size_;
    pages_ = other.pages_;
    dirty_ = other.dirty_;
    armed_ = other.armed_;
    dirty_pages_ = other.dirty_pages_;
    next_page_ = other.next_page_;
    window_pages_ = other.window_pages_;
    other.base_ = nullptr;
    other.size_ = 0;
    other.pages_ = 0;
    other.dirty_ = nullptr;
    other.armed_ = false;
    other.dirty_pages_ = 0;
    other.next_page_ = 0;
    other.window_pages_ = 1;
    if (base_ != nullptr) {
        unregister_barrier(&other);
        register_barrier(this);
    }
    return *this;
}

void MappedWriteBarrier::release() {
    if (base_ == nullptr) return;
    unregister_barrier(this);
    // Writable again before unmapping, so nothing can fault on the way
    // out.
    ::mprotect(base_, pages_ * page_size(), PROT_READ | PROT_WRITE);
    ::munmap(base_, pages_ * page_size());
    delete[] dirty_;
    base_ = nullptr;
    size_ = 0;
    pages_ = 0;
    dirty_ = nullptr;
    armed_ = false;
    dirty_pages_ = 0;
    next_page_ = 0;
    window_pages_ = 1;
}

bool MappedWriteBarrier::allocate(std::size_t bytes) {
    release();
    if (bytes == 0) return false;
    const std::size_t ps = page_size();
    const std::size_t rounded = ((bytes + ps - 1) / ps) * ps;
    void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    base_ = static_cast<uint8_t*>(p);
    size_ = bytes;
    pages_ = rounded / ps;
    dirty_ = new uint8_t[pages_];
    // Everything starts dirty: the host has never seen any of it.
    std::memset(dirty_, 1, pages_);
    dirty_pages_ = pages_;
    armed_ = false;
    next_page_ = 0;
    window_pages_ = 1;
    register_barrier(this);
    return true;
}

void MappedWriteBarrier::mark_all_clean_and_protect() {
    if (base_ == nullptr) return;
    // Clear before protecting, paired with the handler's protect-then-
    // record order above: a write landing in the gap re-arms its own
    // page and is sent by the next flush. A write that races a submit
    // is already undefined for the application under Vulkan's own
    // host-write visibility rules, so bounding it to "one flush late"
    // is the honest guarantee here.
    const std::size_t ps = page_size();

    // A fresh arm makes protected pages fault again, so whatever
    // sequential run the fault window was tracking is over.
    next_page_ = 0;
    window_pages_ = 1;

    // Re-arm ONLY the pages that were actually written.
    //
    // This used to mprotect() the whole allocation on every flush, and a
    // flush happens on every vkQueueSubmit. Measured in-game: ~22MB of
    // mapped memory asked about per submit to send ~420KB, a 47x
    // overscan, 38.7GB scanned for 818MB transferred over one session.
    // At 4KB pages that is ~5,500 pages re-protected per submit, each
    // one a kernel VMA operation plus a TLB shootdown across every
    // thread, whether or not a single byte changed.
    //
    // Clean pages are still protected from the last arm; nothing wrote
    // to them, which is precisely why they are clean, so re-arming
    // them is pure waste. Only the dirty ones were made writable by the
    // fault handler and need protecting again.
    if (!armed_) {
        // First arm, or recovering from a failure: the whole range's
        // protection is unknown, so it all has to be set.
        std::memset(dirty_, 0, pages_);
        dirty_pages_ = 0;
        if (::mprotect(base_, pages_ * ps, PROT_READ) == 0) {
            armed_ = true;
        } else {
            // Could not arm: treat everything as dirty forever rather
            // than silently dropping the engine's writes.
            std::memset(dirty_, 1, pages_);
            dirty_pages_ = pages_;
            armed_ = false;
        }
        return;
    }

    if (dirty_pages_ == 0) return;  // nothing was written; nothing to re-arm

    for (std::size_t i = 0; i < pages_;) {
        if (dirty_[i] == 0) {
            ++i;
            continue;
        }
        const std::size_t run_start = i;
        while (i < pages_ && dirty_[i] != 0) {
            dirty_[i] = 0;
            ++i;
        }
        if (::mprotect(base_ + run_start * ps, (i - run_start) * ps, PROT_READ) != 0) {
            // A page left writable would have its writes missed, so fall
            // back to the conservative state rather than lose them.
            std::memset(dirty_, 1, pages_);
            dirty_pages_ = pages_;
            armed_ = false;
            return;
        }
    }
    dirty_pages_ = 0;
}

std::vector<std::pair<std::size_t, std::size_t>> MappedWriteBarrier::dirty_runs() const {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    if (base_ == nullptr) return runs;
    const std::size_t ps = page_size();
    std::size_t run_start = 0;
    bool in_run = false;
    for (std::size_t i = 0; i < pages_; ++i) {
        if (dirty_[i] != 0 && !in_run) {
            run_start = i;
            in_run = true;
        } else if (dirty_[i] == 0 && in_run) {
            runs.emplace_back(run_start * ps, (i - run_start) * ps);
            in_run = false;
        }
    }
    if (in_run) runs.emplace_back(run_start * ps, (pages_ - run_start) * ps);
    // Never report past the caller's own logical size.
    for (auto& run : runs) {
        if (run.first >= size_) {
            run.second = 0;
        } else if (run.first + run.second > size_) {
            run.second = size_ - run.first;
        }
    }
    return runs;
}

bool MappedWriteBarrier::handle_write_fault(void* addr) {
    if (base_ == nullptr || !armed_) return false;
    auto* p = static_cast<uint8_t*>(addr);
    const std::size_t ps = page_size();
    if (p < base_ || p >= base_ + pages_ * ps) return false;
    const std::size_t page = static_cast<std::size_t>(p - base_) / ps;

    // One page per fault is exact but pathological for the access
    // pattern the engine actually has. A staging buffer is filled by a
    // memcpy, so a 4MB upload took 1024 signals and 1024 mprotect calls
    // and an mprotect is a kernel VMA operation plus a TLB shootdown
    // across every one of the engine's ~67 threads. Measured at Home:
    // 109,638 minor faults a second, on a static screen.
    //
    // So detect a run and unprotect ahead of it. A fault on the page
    // immediately after the last window doubles the window (capped);
    // anything else resets it to one page. The pages opened ahead are
    // marked dirty without having been written yet, which costs sending
    // bytes that did not change, never correctness, since the staging
    // buffer is the authority and re-sending an unchanged page sends
    // identical bytes. The growth only happens once a sequential run is
    // already in progress, which is exactly when those pages are about
    // to be written anyway.
    if (page == next_page_ && window_pages_ < kMaxFaultWindowPages) {
        window_pages_ *= 2;
        if (window_pages_ > kMaxFaultWindowPages) window_pages_ = kMaxFaultWindowPages;
    } else if (page != next_page_) {
        window_pages_ = 1;
    }
    std::size_t run = window_pages_;
    if (page + run > pages_) run = pages_ - page;

    // Order matters, and only one order is safe. Making the pages
    // writable BEFORE recording them means a fault racing a flush is
    // re-recorded after that flush cleared the bit, so the page is sent
    // on the next flush rather than dropped. Recording first would let
    // the flush's own clear erase it and lose the write for good.
    if (::mprotect(base_ + page * ps, run * ps, PROT_READ | PROT_WRITE) != 0) return false;
    for (std::size_t i = page; i < page + run; ++i) {
        if (dirty_[i] == 0) {
            dirty_[i] = 1;
            ++dirty_pages_;
        }
    }
    next_page_ = page + run;
    ++g_fault_count;
    return true;
}

void register_barrier(MappedWriteBarrier* barrier) {
    for (auto& slot : g_barriers) {
        MappedWriteBarrier* expected = nullptr;
        if (slot.compare_exchange_strong(expected, barrier, std::memory_order_acq_rel)) return;
    }
    // Full: this barrier stays unregistered, so its faults are not
    // recognised. allocate() is the only caller and it will behave as an
    // ordinary allocation whose pages are never protected.
    std::fprintf(stderr, "stud: vulkan-client: write-barrier registry full\n");
}

void unregister_barrier(MappedWriteBarrier* barrier) {
    for (auto& slot : g_barriers) {
        MappedWriteBarrier* expected = barrier;
        if (slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) return;
    }
}

bool install_write_barrier() {
    if (g_installed.load(std::memory_order_acquire)) return true;
    // libvulkan.so is its own object and does not link the framework, so
    // the hook is resolved by name at runtime, the same
    // dlsym(RTLD_DEFAULT, ...) pattern Stud already uses for every other
    // cross-object entry point. If it is not there, the barrier stays
    // off and the caller keeps the compare-based path rather than
    // risking normal writes being seen as crashes.
    using SetHandlerFn = void (*)(bool (*)(void*));
    auto* set_handler =
        reinterpret_cast<SetHandlerFn>(::dlsym(RTLD_DEFAULT, "stud_set_write_fault_handler"));
    if (set_handler == nullptr) {
        std::fprintf(stderr,
                     "stud: vulkan-client: write barrier unavailable "
                     "(stud_set_write_fault_handler not found)\n");
        return false;
    }
    set_handler(&dispatch_write_fault);
    g_installed.store(true, std::memory_order_release);
    std::printf("stud: vulkan-client: mapped-memory write barrier installed\n");
    std::fflush(stdout);
    return true;
}

bool write_barrier_available() { return g_installed.load(std::memory_order_acquire); }

unsigned long long barrier_fault_count() { return g_fault_count; }

}  // namespace stud::render_client
