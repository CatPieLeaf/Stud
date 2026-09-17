#include "uffd_scan.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// The kernel headers in the NDK sysroot are not guaranteed to carry these,
// and a missing define would silently disable the whole mechanism rather
// than fail to build, so they are spelled out. Values are UAPI and fixed.
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1u << 16)
#endif
#ifndef UFFD_FEATURE_WP_UNPOPULATED
#define UFFD_FEATURE_WP_UNPOPULATED (1u << 13)
#endif
#ifndef PAGE_IS_WRITTEN
#define PAGE_IS_WRITTEN (1u << 1)
#endif
#ifndef PM_SCAN_WP_MATCHING
#define PM_SCAN_WP_MATCHING (1u << 0)
#endif
#ifndef PM_SCAN_CHECK_WPASYNC
#define PM_SCAN_CHECK_WPASYNC (1u << 1)
#endif

#ifndef PAGEMAP_SCAN
struct stud_page_region {
    uint64_t start;
    uint64_t end;
    uint64_t categories;
};
struct stud_pm_scan_arg {
    uint64_t size;
    uint64_t flags;
    uint64_t start;
    uint64_t end;
    uint64_t walk_end;
    uint64_t vec;
    uint64_t vec_len;
    uint64_t max_pages;
    uint64_t category_inverted;
    uint64_t category_mask;
    uint64_t category_anyof_mask;
    uint64_t return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct stud_pm_scan_arg)
using PageRegion = stud_page_region;
using PmScanArg = stud_pm_scan_arg;
#else
using PageRegion = page_region;
using PmScanArg = pm_scan_arg;
#endif

namespace stud::render_client {

namespace {

std::size_t page_size() {
    static const std::size_t sz = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
    return sz;
}

struct Kernel {
    int uffd = -1;
    int pagemap = -1;
    bool ok = false;
};

Kernel& kernel() {
    static Kernel k = [] {
        Kernel out;
        // The default. The kernel records which pages were written, so the
        // engine is never write-protected: no signal per page, no
        // mprotect, and no TLB shootdown across the engine's ~67 threads
        // for every one of them. The fault barrier remains as the
        // fallback for a kernel without WP_ASYNC, and
        // STUD_VK_NO_UFFD_SCAN=1 forces that path back for an A/B.
        if (std::getenv("STUD_VK_NO_UFFD_SCAN") != nullptr) return out;

        // UFFD_USER_MODE_ONLY is what makes this work unprivileged where
        // vm.unprivileged_userfaultfd is 0: a descriptor that can only ever
        // see faults from user mode needs no CAP_SYS_PTRACE.
        const int fd = static_cast<int>(
            ::syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        if (fd < 0) {
            std::fprintf(stderr, "stud: vulkan-client: uffd-scan unavailable (userfaultfd: %s)\n",
                         std::strerror(errno));
            return out;
        }
        uffdio_api api{};
        api.api = UFFD_API;
        // WP_UNPOPULATED matters as much as WP_ASYNC here, and it is easy to
        // miss: without it, write-protection does not apply to pages that
        // have never been populated. A staging buffer is fresh anonymous
        // memory, so its pages are exactly that, and the very first write to
        // one would install a PTE nobody protected and go unrecorded. Taken
        // from the kernel's own pagemap_ioctl selftest, which asks for both.
        api.features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED;
        if (::ioctl(fd, UFFDIO_API, &api) != 0 ||
            (api.features & UFFD_FEATURE_WP_ASYNC) == 0 ||
            (api.features & UFFD_FEATURE_WP_UNPOPULATED) == 0) {
            std::fprintf(stderr,
                         "stud: vulkan-client: uffd-scan unavailable (this kernel offers "
                         "WP_ASYNC=%d WP_UNPOPULATED=%d; both are required)\n",
                         (api.features & UFFD_FEATURE_WP_ASYNC) != 0,
                         (api.features & UFFD_FEATURE_WP_UNPOPULATED) != 0);
            ::close(fd);
            return out;
        }
        const int pm = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        if (pm < 0) {
            std::fprintf(stderr, "stud: vulkan-client: uffd-scan unavailable (pagemap: %s)\n",
                         std::strerror(errno));
            ::close(fd);
            return out;
        }
        out.uffd = fd;
        out.pagemap = pm;
        out.ok = true;
        std::printf("stud: vulkan-client: uffd-scan write tracking enabled "
                    "(kernel-side, no fault handler)\n");
        std::fflush(stdout);
        return out;
    }();
    return k;
}

std::atomic<unsigned long long> g_scans{0};
std::atomic<unsigned long long> g_pages{0};

}  // namespace

bool UffdScan::available() { return kernel().ok; }

bool UffdScan::register_range(void* base, std::size_t bytes) {
    Kernel& k = kernel();
    if (!k.ok || base == nullptr || bytes == 0) return false;
    // Transparent huge pages would make the kernel report a whole 2MB page
    // as written for a 4KB store, which is the over-sending this mechanism
    // exists to avoid. Staging buffers are large anonymous mappings, which
    // is exactly what THP collapses. Wine's own kernel write-watch does the
    // same thing for the same reason.
    ::madvise(base, bytes, MADV_NOHUGEPAGE);

    uffdio_register reg{};
    reg.range.start = reinterpret_cast<uint64_t>(base);
    reg.range.len = bytes;
    reg.mode = UFFDIO_REGISTER_MODE_WP;
    if (::ioctl(k.uffd, UFFDIO_REGISTER, &reg) != 0) {
        std::fprintf(stderr, "stud: vulkan-client: uffd-scan register failed: %s\n",
                     std::strerror(errno));
        return false;
    }
    // The kernel reports which operations this registration actually
    // supports, and write-protect is the only one that matters here.
    // Checking it is what the selftest does, and a registration that
    // cannot be write-protected would track nothing at all.
    if ((reg.ioctls & (1ull << _UFFDIO_WRITEPROTECT)) == 0) {
        std::fprintf(stderr,
                     "stud: vulkan-client: uffd-scan registration does not support "
                     "write-protect\n");
        uffdio_range range{};
        range.start = reg.range.start;
        range.len = reg.range.len;
        ::ioctl(k.uffd, UFFDIO_UNREGISTER, &range);
        return false;
    }
    return true;
}

void UffdScan::unregister_range(void* base, std::size_t bytes) {
    Kernel& k = kernel();
    if (!k.ok || base == nullptr || bytes == 0) return;
    uffdio_range range{};
    range.start = reinterpret_cast<uint64_t>(base);
    range.len = bytes;
    ::ioctl(k.uffd, UFFDIO_UNREGISTER, &range);
}

bool UffdScan::protect(void* base, std::size_t bytes) {
    Kernel& k = kernel();
    if (!k.ok || base == nullptr || bytes == 0) return false;
    uffdio_writeprotect wp{};
    wp.range.start = reinterpret_cast<uint64_t>(base);
    wp.range.len = bytes;
    wp.mode = UFFDIO_WRITEPROTECT_MODE_WP;
    return ::ioctl(k.uffd, UFFDIO_WRITEPROTECT, &wp) == 0;
}

bool UffdScan::take_written_and_protect(void* base, std::size_t offset, std::size_t len,
                                        std::vector<std::pair<std::size_t, std::size_t>>& out) {
    Kernel& k = kernel();
    out.clear();
    if (!k.ok || base == nullptr || len == 0) return false;
    const std::size_t ps = page_size();
    auto* start = static_cast<uint8_t*>(base) + offset;
    const std::size_t span = ((len + ps - 1) / ps) * ps;

    // One scan can report at most this many separate runs; anything past
    // that is picked up by the next scan, because the pages it did not
    // reach keep their written state. walk_end says where it stopped.
    std::vector<PageRegion> regions(256);
    uint64_t cursor = reinterpret_cast<uint64_t>(start);
    const uint64_t end = cursor + span;

    while (cursor < end) {
        PmScanArg arg{};
        arg.size = sizeof(arg);
        // WP_MATCHING re-protects exactly the pages this call reports, in
        // the same kernel operation: the set that is cleared is the set
        // that was returned, with no window between them. CHECK_WPASYNC
        // refuses the scan if the range is not actually registered async,
        // rather than quietly reporting nothing.
        arg.flags = PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC;
        arg.start = cursor;
        arg.end = end;
        arg.vec = reinterpret_cast<uint64_t>(regions.data());
        arg.vec_len = regions.size();
        arg.category_mask = PAGE_IS_WRITTEN;
        arg.return_mask = PAGE_IS_WRITTEN;

        const long n = ::ioctl(k.pagemap, PAGEMAP_SCAN, &arg);
        if (n < 0) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr, "stud: vulkan-client: uffd-scan PAGEMAP_SCAN failed: %s\n",
                             std::strerror(errno));
            }
            return false;
        }
        for (long i = 0; i < n; ++i) {
            const uint64_t region_start = regions[static_cast<std::size_t>(i)].start;
            const uint64_t region_end = regions[static_cast<std::size_t>(i)].end;
            if (region_end <= region_start) continue;
            const std::size_t rel =
                static_cast<std::size_t>(region_start - reinterpret_cast<uint64_t>(base));
            out.emplace_back(rel, static_cast<std::size_t>(region_end - region_start));
            g_pages.fetch_add((region_end - region_start) / ps, std::memory_order_relaxed);
        }
        g_scans.fetch_add(1, std::memory_order_relaxed);
        if (arg.walk_end <= cursor) break;  // no progress: stop rather than spin
        cursor = arg.walk_end;
        if (n < static_cast<long>(regions.size())) break;  // the range is exhausted
    }
    return true;
}

unsigned long long UffdScan::scan_count() { return g_scans.load(std::memory_order_relaxed); }
unsigned long long UffdScan::pages_reported() { return g_pages.load(std::memory_order_relaxed); }

}  // namespace stud::render_client
