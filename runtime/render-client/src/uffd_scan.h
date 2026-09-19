#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace stud::render_client {

// Kernel-side write tracking for mapped memory: "uffd-scan".
//
// The write barrier beside this file protects staging pages with mprotect
// and records them from a SIGSEGV handler. That works, and it cost three
// lost-write races in one session, all of them interleavings between the
// handler and the flush, plus a signal and an mprotect (a VMA operation
// and a TLB shootdown across every thread) for every page first written.
//
// The kernel does this itself now. Registering a range with userfaultfd in
// write-protect mode and enabling UFFD_FEATURE_WP_ASYNC means a write to a
// protected page is resolved by the kernel: write permission is restored
// and the page is marked written, with nothing delivered to userspace. At
// flush time one PAGEMAP_SCAN ioctl returns the written ranges AND
// re-protects them in the same call.
//
// That single call is the reason to do this rather than only for speed:
// "what changed" and "start watching again" stop being two steps that
// something can happen between, which is exactly what the three bugs were.
//
// Availability is not assumed anywhere. The distribution ships
// vm.unprivileged_userfaultfd = 0 and a root-only /dev/userfaultfd, so the
// descriptor is opened with UFFD_USER_MODE_ONLY, which is the kernel's own
// exemption from that sysctl. Verified working inside Process B's bwrap
// sandbox, through its seccomp filter and user namespace, before this was
// written; see Stud-Analysis/gpu/mapped-memory-dirty-tracking.md.
class UffdScan {
public:
    // One descriptor for the process. False if userfaultfd, the async
    // write-protect feature, or /proc/self/pagemap is unavailable, in
    // which case every caller keeps the mprotect barrier.
    static bool available();

    // Track writes to this range. It must be page aligned.
    static bool register_range(void* base, std::size_t bytes);
    static void unregister_range(void* base, std::size_t bytes);

    // Arm (or re-arm) the whole range without collecting anything.
    static bool protect(void* base, std::size_t bytes);

    // The written ranges within [base+offset, base+offset+len), re-armed
    // in the same kernel call. Offsets are relative to base.
    static bool take_written_and_protect(void* base, std::size_t offset, std::size_t len,
                                         std::vector<std::pair<std::size_t, std::size_t>>& out);

    // How many scans have run, and how many pages they reported. Reported
    // by STUD_VK_MEM_STATS beside the barrier's own fault count, so the
    // two mechanisms can be compared on the same workload.
    static unsigned long long scan_count();
    static unsigned long long pages_reported();
    // What the scans cost. PM_SCAN_WP_MATCHING rewrites page protection
    // and so shoots down TLBs on every core, which is why the total
    // matters and not just the count; see uffd_scan.cpp.
    static unsigned long long scan_time_ns();
    static unsigned long long scan_time_max_ns();
    static unsigned long long slow_scans();
};

}  // namespace stud::render_client
