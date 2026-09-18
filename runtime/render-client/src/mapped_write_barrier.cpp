#include "mapped_write_barrier.h"

#include "uffd_scan.h"

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
    speculative_ = other.speculative_;
    armed_ = other.armed_;
    dirty_pages_.store(other.dirty_pages_.load(std::memory_order_acquire),
                       std::memory_order_release);
    next_page_ = other.next_page_;
    window_pages_ = other.window_pages_;
    other.base_ = nullptr;
    other.size_ = 0;
    other.pages_ = 0;
    other.dirty_ = nullptr;
    other.speculative_ = nullptr;
    other.armed_ = false;
    other.dirty_pages_.store(0, std::memory_order_release);
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
    if (uffd_) {
        UffdScan::unregister_range(base_, pages_ * page_size());
        uffd_ = false;
    }
    unregister_barrier(this);
    // Writable again before unmapping, so nothing can fault on the way
    // out.
    ::mprotect(base_, pages_ * page_size(), PROT_READ | PROT_WRITE);
    ::munmap(base_, pages_ * page_size());
    delete[] dirty_;
    base_ = nullptr;
    size_ = 0;
    pages_ = 0;
    delete[] speculative_;
    speculative_ = nullptr;
    dirty_ = nullptr;
    armed_ = false;
    dirty_pages_.store(0, std::memory_order_release);
    next_page_ = 0;
    window_pages_ = 1;
}

bool MappedWriteBarrier::allocate(std::size_t bytes) { return allocate_backed_by(bytes, -1); }

bool MappedWriteBarrier::allocate_backed_by(std::size_t bytes, int fd) {
    release();
    if (bytes == 0) return false;
    const std::size_t ps = page_size();
    const std::size_t rounded = ((bytes + ps - 1) / ps) * ps;
    // MAP_SHARED when a file backs it, so the host's mapping of the same
    // file sees these writes; see allocate_backed_by()'s declaration.
    void* p = fd >= 0 ? ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)
                      : ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return false;
    base_ = static_cast<uint8_t*>(p);
    size_ = bytes;
    pages_ = rounded / ps;
    dirty_ = new std::atomic<uint8_t>[pages_];
    speculative_ = new std::atomic<uint8_t>[pages_];
    for (std::size_t i = 0; i < pages_; ++i) speculative_[i].store(0, std::memory_order_relaxed);
    // Everything starts dirty: the host has never seen any of it.
    for (std::size_t i = 0; i < pages_; ++i) dirty_[i].store(1, std::memory_order_relaxed);
    dirty_pages_.store(pages_, std::memory_order_release);
    armed_ = false;
    next_page_ = 0;
    window_pages_ = 1;
    // uffd-scan first: the kernel tracks the writes, so no page is ever
    // protected against the engine and no fault handler is involved. The
    // fault barrier stays as the fallback, unchanged.
    if (UffdScan::available() && UffdScan::register_range(base_, pages_ * ps) &&
        UffdScan::protect(base_, pages_ * ps)) {
        uffd_ = true;
        armed_ = true;
        // Everything starts written, as it does for the fault barrier: the
        // host has never seen this memory. The first scan reports it all.
        return true;
    }
    if (!register_barrier(this)) {
        // Unprotected and unregistered is safe; protected and
        // unregistered is a crash on the first write.
        ::munmap(base_, pages_ * ps);
        delete[] dirty_;
        base_ = nullptr;
        size_ = 0;
        pages_ = 0;
        dirty_ = nullptr;
        dirty_pages_.store(0, std::memory_order_release);
        return false;
    }
    return true;
}

void MappedWriteBarrier::mark_all_clean_and_protect() {
    mark_clean_and_protect(0, size_);
}

void MappedWriteBarrier::mark_clean_and_protect(std::size_t offset, std::size_t len) {
    if (base_ == nullptr) return;
    if (uffd_) {
        const std::size_t ps = page_size();
        const std::size_t first = (offset / ps) * ps;
        std::size_t span = ((offset - first) + len + ps - 1) / ps * ps;
        if (first + span > pages_ * ps) span = pages_ * ps - first;
        UffdScan::protect(base_ + first, span);
        return;
    }
    std::lock_guard<std::mutex> guard(lock_);
    mark_clean_and_protect_locked(offset, len);
}

std::vector<std::pair<std::size_t, std::size_t>> MappedWriteBarrier::take_dirty_runs_and_protect(
    std::size_t offset, std::size_t len) {
    if (base_ == nullptr) return {};
    if (uffd_) {
        // One ioctl: what was written, and re-armed, with nothing able to
        // happen between the two. This is the whole reason for uffd-scan.
        std::vector<std::pair<std::size_t, std::size_t>> runs;
        if (!UffdScan::take_written_and_protect(base_, offset, len, runs)) {
            // The kernel refused. Report the whole asked range rather than
            // risk dropping a write; it costs bytes, never correctness.
            runs.clear();
            runs.emplace_back(offset, len);
        }
        for (auto& run : runs) {
            if (run.first >= size_) {
                run.second = 0;
            } else if (run.first + run.second > size_) {
                run.second = size_ - run.first;
            }
        }
        return runs;
    }
    std::lock_guard<std::mutex> guard(lock_);
    auto runs = dirty_runs_locked();
    mark_clean_and_protect_locked(offset, len);
    return runs;
}

void MappedWriteBarrier::mark_clean_and_protect_locked(std::size_t offset, std::size_t len) {
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
        // Only a whole-allocation arm can do this: it clears every page's
        // dirty bit, which for a partial flush would drop exactly the
        // writes this function exists to preserve. Leave them dirty; the
        // next full flush sends them and arms properly.
        if (offset != 0 || len < size_) return;
        // First arm, or recovering from a failure: the whole range's
        // protection is unknown, so it all has to be set.
        for (std::size_t i = 0; i < pages_; ++i) dirty_[i].store(0, std::memory_order_relaxed);
        dirty_pages_.store(0, std::memory_order_release);
        if (::mprotect(base_, pages_ * ps, PROT_READ) == 0) {
            armed_ = true;
        } else {
            // Could not arm: treat everything as dirty forever rather
            // than silently dropping the engine's writes.
            for (std::size_t p = 0; p < pages_; ++p) dirty_[p].store(1, std::memory_order_relaxed);
            dirty_pages_.store(pages_, std::memory_order_release);
            armed_ = false;
        }
        return;
    }

    if (dirty_pages_.load(std::memory_order_acquire) == 0) return;  // nothing written

    // Only the pages this flush actually sent.
    std::size_t first = offset / ps;
    std::size_t last = (offset + len + ps - 1) / ps;
    if (first > pages_) first = pages_;
    if (last > pages_) last = pages_;

    std::size_t cleaned = 0;
    for (std::size_t i = first; i < last;) {
        if (dirty_[i].load(std::memory_order_acquire) == 0) {
            ++i;
            continue;
        }
        const std::size_t run_start = i;
        while (i < last && dirty_[i].load(std::memory_order_acquire) != 0) ++i;
        // Protect BEFORE clearing, never after. Clearing first leaves the
        // page writable with nothing recording it: a write landing in that
        // window is not sent by this flush, does not fault (still
        // writable), and so is never sent at all. Protecting first means
        // any write from here on faults and re-marks the page, and the
        // caller sends after this returns, so a write that beat the
        // mprotect is still in the bytes that go out.
        if (::mprotect(base_ + run_start * ps, (i - run_start) * ps, PROT_READ) != 0) {
            // A page left writable would have its writes missed, so fall
            // back to the conservative state rather than lose them.
            for (std::size_t i = 0; i < pages_; ++i) dirty_[i].store(1, std::memory_order_relaxed);
            dirty_pages_.store(pages_, std::memory_order_release);
            armed_ = false;
            return;
        }
        for (std::size_t j = run_start; j < i; ++j) {
            if (dirty_[j].exchange(0, std::memory_order_acq_rel) != 0) ++cleaned;
        }
    }
    if (cleaned != 0) dirty_pages_.fetch_sub(cleaned, std::memory_order_acq_rel);
}

std::vector<std::pair<std::size_t, std::size_t>> MappedWriteBarrier::dirty_runs() const {
    if (base_ == nullptr) return {};
    if (uffd_) {
        // Reading without re-arming is exactly the split this mechanism
        // exists to remove, so it is not offered: callers use
        // take_dirty_runs_and_protect().
        return {{0, size_}};
    }
    std::lock_guard<std::mutex> guard(lock_);
    return dirty_runs_locked();
}

std::vector<std::pair<std::size_t, std::size_t>> MappedWriteBarrier::dirty_runs_locked() const {
    std::vector<std::pair<std::size_t, std::size_t>> runs;
    if (base_ == nullptr) return runs;
    const std::size_t ps = page_size();
    std::size_t run_start = 0;
    bool in_run = false;
    for (std::size_t i = 0; i < pages_; ++i) {
        if (dirty_[i].load(std::memory_order_acquire) != 0 && !in_run) {
            run_start = i;
            in_run = true;
        } else if (dirty_[i].load(std::memory_order_acquire) == 0 && in_run) {
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
    const auto* p = static_cast<uint8_t*>(addr);
    const std::size_t ps = page_size();
    if (p < base_ || p >= base_ + pages_ * ps) return false;
    std::lock_guard<std::mutex> guard(lock_);
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
        // Only the page that faulted was written; the rest are a guess
        // that the run continues, which the flush checks rather than pays
        // for. See speculative_.
        if (speculative_ != nullptr) {
            speculative_[i].store(i == page ? 0 : 1, std::memory_order_release);
        }
        if (dirty_[i].exchange(1, std::memory_order_acq_rel) == 0) {
            dirty_pages_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    next_page_ = page + run;
    ++g_fault_count;
    return true;
}

bool register_barrier(MappedWriteBarrier* barrier) {
    for (auto& slot : g_barriers) {
        MappedWriteBarrier* expected = nullptr;
        if (slot.compare_exchange_strong(expected, barrier, std::memory_order_acq_rel)) {
            return true;
        }
    }
    // Full: this barrier's faults would reach nobody. Its pages still get
    // protected by the first arm, so every write to them would be
    // classified as a crash and kill the writing thread. Say so and let
    // allocate() fail, which puts the mapping on the plain staging path.
    std::fprintf(stderr, "stud: vulkan-client: write-barrier registry full\n");
    return false;
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
