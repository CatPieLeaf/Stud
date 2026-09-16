// The write barrier's one promise: a byte written into the mapping is
// reported by a flush. Everything else it does is an optimisation.
//
// Two of the three bugs this file was written after were invisible to
// inspection and would have been caught here in seconds: a partial flush
// clearing the whole allocation's dirty state, and a dirty-page counter
// raced between the fault handler and the flush until it read zero with
// pages still dirty. Both lose writes silently, which downstream is a
// shader reading whatever was there before.
//
// The barrier resolves its fault hook through dlsym(RTLD_DEFAULT,
// "stud_set_write_fault_handler"), which Stud's trap handler normally
// provides. This test provides it instead and drives a real SIGSEGV
// handler, so the mechanism under test is the real one, faults included.

#include "../runtime/render-client/src/mapped_write_barrier.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

bool (*g_handler)(void*) = nullptr;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

void segv_handler(int, siginfo_t* info, void*) {
    if (g_handler != nullptr && g_handler(info->si_addr)) return;
    // Not ours: a real fault, and continuing would spin on it forever.
    std::fprintf(stderr, "FAILED: unclaimed SIGSEGV at %p\n", info->si_addr);
    std::_Exit(1);
}

void install() {
    struct sigaction sa {};
    sa.sa_sigaction = segv_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    check(::sigaction(SIGSEGV, &sa, nullptr) == 0, "install SIGSEGV handler");
}

// Bytes the barrier said were written, gathered from one flush.
std::vector<uint8_t> reported(stud::render_client::MappedWriteBarrier& b,
                              std::size_t offset, std::size_t len,
                              std::vector<uint8_t>& host) {
    // Same order the client uses, and it has to be: snapshot, re-arm,
    // then read. Reading first leaves a window where a write lands after
    // its page was read and before it was protected again, and that write
    // is then cleared without ever being reported.
    const auto runs = b.take_dirty_runs_and_protect(offset, len);
    for (const auto& run : runs) {
        std::size_t start = run.first;
        std::size_t end = run.first + run.second;
        if (end <= offset || start >= offset + len) continue;
        if (start < offset) start = offset;
        if (end > offset + len) end = offset + len;
        std::memcpy(host.data() + start, b.data() + start, end - start);
    }
    return host;
}

// A partial flush must not discard what lies outside it.
void partial_flush_keeps_the_rest() {
    stud::render_client::MappedWriteBarrier b;
    check(b.allocate(64 * 1024), "allocate");
    const std::size_t ps = 4096;
    std::vector<uint8_t> host(64 * 1024, 0);

    b.mark_all_clean_and_protect();
    b.data()[0] = 0xAA;              // inside the flushed range
    b.data()[10 * ps] = 0xBB;        // outside it
    reported(b, 0, ps, host);
    check(host[0] == 0xAA, "the flushed range is sent");

    // The write outside the range was not sent, so it must still be
    // pending rather than silently dropped.
    check(!b.clean(), "a write outside a partial flush stays dirty");
    reported(b, 0, b.size(), host);
    check(host[10 * ps] == 0xBB, "it is sent by the next full flush");
    std::printf("  partial flush keeps what it did not send: ok\n");
}

// Every write reported, under real concurrent faulting.
void concurrent_writes_are_all_reported() {
    stud::render_client::MappedWriteBarrier b;
    const std::size_t bytes = 1 << 20;
    check(b.allocate(bytes), "allocate");
    std::vector<uint8_t> host(bytes, 0);
    b.mark_all_clean_and_protect();

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> written{0};
    std::vector<std::thread> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back([&, w] {
            std::size_t i = static_cast<std::size_t>(w) * 4096;
            while (!stop.load(std::memory_order_relaxed)) {
                b.data()[i % bytes] = static_cast<uint8_t>(0x40 + w);
                i += 4093;  // prime stride: crosses pages unpredictably
                written.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // Flush underneath them, which is what races the fault handler.
    for (int i = 0; i < 200; ++i) reported(b, 0, b.size(), host);
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : writers) t.join();

    // Quiesced: one last flush must leave nothing behind, and every byte
    // the mapping holds must have reached the host copy.
    reported(b, 0, b.size(), host);
    // clean() is a fault-barrier property: it answers from a counter the
    // handler maintains. The kernel keeps no such count and asking costs a
    // scan, so uffd-scan reports "not clean" always and the flush finds
    // nothing. The invariant that matters is the next check, and it is the
    // same for both.
    if (!b.kernel_tracked()) {
        check(b.clean(), "nothing is left dirty once writing has stopped");
    }
    std::size_t bad = 0, first = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
        if (host[i] != b.data()[i]) {
            if (bad == 0) first = i;
            ++bad;
        }
    }
    if (bad != 0) {
        std::fprintf(stderr, "  %zu byte(s) differ, first at %zu (page %zu, offset in page %zu)\n",
                     bad, first, first / 4096, first % 4096);
        std::fprintf(stderr, "  host=0x%02x barrier=0x%02x clean=%d\n", host[first],
                     b.data()[first], static_cast<int>(b.clean()));
    }
    check(bad == 0, "every concurrent write reached the host copy");
    std::printf("  %zu concurrent writes, all reported: ok\n",
                written.load(std::memory_order_relaxed));
}

}  // namespace

// The hook the barrier looks for. Real name, real signature.
extern "C" void stud_set_write_fault_handler(bool (*handler)(void*)) { g_handler = handler; }

int main() {
    install();
    std::printf("backend: %s\n",
                std::getenv("STUD_VK_UFFD_SCAN") != nullptr ? "uffd-scan (kernel)" : "fault barrier");
    check(stud::render_client::install_write_barrier(), "install the barrier");
    partial_flush_keeps_the_rest();
    concurrent_writes_are_all_reported();
    std::printf("mapped_write_barrier_test: ok\n");
    return 0;
}
