// The check every mapped read and write off the wire goes through.
#include "stud/vulkan_forward.h"

#include <cstdint>
#include <cstdio>

namespace {

using stud::render_host::vk_wire::range_inside;

// A mapping of 100 bytes at offset 50 into its allocation.
static_assert(range_inside(50, 100, 50, 100));
static_assert(range_inside(149, 1, 50, 100));
static_assert(range_inside(150, 0, 50, 100));
static_assert(!range_inside(49, 1, 50, 100));
static_assert(!range_inside(149, 2, 50, 100));
static_assert(!range_inside(151, 0, 50, 100));
// These wrap if added up.
static_assert(!range_inside(60, UINT64_MAX, 50, 100));
static_assert(!range_inside(UINT64_MAX, 2, 50, 100));
static_assert(!range_inside(0, UINT64_MAX, 0, 100));

}  // namespace

int main() {
    std::puts("range_inside: ok");
    return 0;
}
