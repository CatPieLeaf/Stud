// Shared pNext-chain node sizing, compiled into BOTH processes so they
// can never disagree about a struct's length. See vulkan_forward.h.
//
// The table is generated (tools/gen_vulkan_registry.py), so a struct the
// engine starts using is already in it. These are spec-fixed layouts; the
// wire also carries the size so a genuine mismatch is caught rather than
// silently misread.

#include "stud/vulkan_forward.h"

#include <algorithm>
#include <iterator>

namespace stud::render_host::vk_wire {

uint32_t chain_node_size(uint32_t s_type) {
    const auto* end = std::end(kChainNodeSizes);
    const auto* it = std::lower_bound(
        std::begin(kChainNodeSizes), end, s_type,
        [](const ChainNodeSize& n, uint32_t value) { return n.s_type < value; });
    return it != end && it->s_type == s_type ? it->size : 0;
}

}  // namespace stud::render_host::vk_wire
