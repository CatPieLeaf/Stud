#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace stud::render_client {

// Page-granular dirty tracking for Vulkan's mapped device memory.
//
// Why this exists: a HOST_COHERENT mapping is defined as needing no
// explicit flush -- the app writes and the device sees it. Stud's
// "mapping" is a staging buffer in another process, so every submit has
// to ship whatever the engine wrote. The first version compared the
// staging buffer against a shadow copy to find that out, which was a
// large win over sending everything (25.7GB asked vs 143MB actually
// changed over 1800 submits) but left `memcmp` as ~45% of the engine
// thread's own CPU: it is still O(all mapped memory) on every submit,
// and the engine keeps tens of megabytes mapped for the life of the
// process.
//
// So stop looking for the writes and let the MMU report them. The
// staging pages are mapped read-only; the first write to a page faults,
// the handler records that page and makes it writable, and the flush
// sends only recorded pages before protecting them again. Cost per
// flush becomes proportional to what the engine actually touched rather
// than to what it happens to have mapped.
//
// Standard write-barrier shape (live migration, incremental GC), and
// exact: a page is sent if and only if it was written.
class MappedWriteBarrier {
public:
    MappedWriteBarrier() = default;
    ~MappedWriteBarrier();

    MappedWriteBarrier(const MappedWriteBarrier&) = delete;
    MappedWriteBarrier& operator=(const MappedWriteBarrier&) = delete;
    MappedWriteBarrier(MappedWriteBarrier&& other) noexcept { *this = std::move(other); }
    MappedWriteBarrier& operator=(MappedWriteBarrier&& other) noexcept;

    // Page-aligned allocation the engine writes into. False on failure,
    // in which case the caller must fall back to sending everything.
    bool allocate(std::size_t bytes);
    uint8_t* data() const { return base_; }
    std::size_t size() const { return size_; }
    bool valid() const { return base_ != nullptr; }

    // Every page starts dirty: the host has never seen this memory.
    // After a flush the caller calls this to re-arm the barrier.
    void mark_all_clean_and_protect();

    // Byte ranges written since the last re-arm, merged into runs so a
    // rewritten region travels as one write rather than many.
    std::vector<std::pair<std::size_t, std::size_t>> dirty_runs() const;

    // True when nothing has been written since the last re-arm. Lets a
    // caller skip an allocation entirely rather than walk its pages.
    bool clean() const { return armed_ && dirty_pages_ == 0; }

    // Called from the signal handler. True if the address belonged to
    // this allocation and the fault was handled.
    bool handle_write_fault(void* addr);

    // How many pages one fault unprotects at most. A sequential write
    // through a large mapping otherwise costs one signal plus one
    // mprotect per 4KB, which is what this cap exists to bound -- see
    // the growth logic in handle_write_fault().
    static constexpr std::size_t kMaxFaultWindowPages = 64;

private:
    void release();

    uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pages_ = 0;
    // Not a vector<bool>: written from a signal handler, so it has to be
    // a plain byte array with no allocation and no bit twiddling.
    uint8_t* dirty_ = nullptr;
    bool armed_ = false;
    // How many pages are currently marked dirty. Maintained by the fault
    // handler and the re-arm, so "did anything change since the last
    // flush" is a single comparison instead of a scan over every page of
    // every mapped allocation on every submit.
    std::size_t dirty_pages_ = 0;
    // Sequential-write detection, maintained only by the fault handler.
    // `next_page_` is the page a continuing run would fault on next, and
    // `window_pages_` is how much to unprotect when it does.
    std::size_t next_page_ = 0;
    std::size_t window_pages_ = 1;
};

// Total write faults taken since the process started, across every
// barrier. Reported by STUD_VK_MEM_STATS -- each one is a signal
// delivery plus an mprotect (a VMA operation and a TLB shootdown across
// every thread), so it is the cost this whole mechanism trades against
// the bytes it saves sending.
unsigned long long barrier_fault_count();

// Installs the SIGSEGV write-barrier hook, chaining to whatever handler
// is already installed. Safe to call repeatedly. False means the
// barrier is unavailable and callers keep the compare-based path.
bool install_write_barrier();

// True when the barrier is installed and usable.
bool write_barrier_available();

// Registers/unregisters an allocation with the fault dispatcher.
void register_barrier(MappedWriteBarrier* barrier);
void unregister_barrier(MappedWriteBarrier* barrier);

}  // namespace stud::render_client
