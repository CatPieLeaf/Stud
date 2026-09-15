#include "stud/property_area.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

namespace stud::bionic_runtime {

namespace {

// Confirmed in the engine constants (see property_area.h's own doc
// comment), both written together as one real 8-byte immediate in the
// actual extracted libc.so's own prop_area::map_prop_area_rw.
constexpr uint32_t kPropAreaMagic = 0x504f5250;
constexpr uint32_t kPropAreaVersion = 0xfc6ed0ab;
constexpr size_t kHeaderSize = 128;  // bytes_used_(4)+serial_(4)+magic_(4)+version_(4)+reserved_[28*4]
constexpr size_t kPropValueMax = 92;
constexpr size_t kPropInfoFixedSize = 4 + kPropValueMax;  // serial + value[92]

// Confirmed in the engine ordering (prop_area::find_prop_trie_node, this
// project's own reading of the actual extracted libc.so): siblings at
// each trie level are compared by LENGTH first (shorter = "less"), and
// only by lexicographic byte content when lengths are equal, NOT the
// plain lexicographic order std::less<std::string> would give (e.g.
// "b" sorts before "ab" here, the reverse of plain string comparison).
// Getting this wrong wouldn't crash anything (real bionic's own search
// just silently fails to find a real node built with the wrong
// ordering, degrading to today's "no properties" status quo) but would
// silently make this whole fix a no-op, so it's worth getting exactly
// right rather than assuming plain string order is close enough.
struct ByLengthThenLex {
    bool operator()(const std::string& a, const std::string& b) const {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    }
};

// Real in-memory trie build tree, keyed by dotted-name segment at each
// level, serialized to the real on-disk format afterward. The
// comparator above keeps children sorted the same way real bionic's own
// find_prop_trie_node compares siblings, which serialize_children()
// below turns into a real, valid (if not perfectly balanced, doesn't
// need to be for correctness, only a handful of real properties ever go
// through this) binary search tree via recursive midpoint selection.
struct TrieBuildNode {
    std::string token;
    bool has_value = false;
    std::string value;
    std::map<std::string, std::unique_ptr<TrieBuildNode>, ByLengthThenLex> children;
};

// Bump allocator over a growable byte buffer; every real prop_area
// offset (trie-node/prop-info positions) is relative to data_ (i.e. to
// this buffer's own start), matching the real, confirmed in the engine
// semantics documented in property_area.h.
class Arena {
public:
    // Reserves `size` bytes, zero-initialized, 4-byte aligned (matches
    // every real field in this format being a uint32_t/atomic_uint32_t).
    // Returns the real offset (from the arena's own start = data_) the
    // reservation begins at.
    uint32_t reserve(size_t size) {
        size_t aligned = (bytes_.size() + 3) & ~size_t(3);
        bytes_.resize(aligned + size, 0);
        return static_cast<uint32_t>(aligned);
    }

    void write_u32(uint32_t offset, uint32_t value) {
        std::memcpy(bytes_.data() + offset, &value, sizeof(value));
    }

    void write_bytes(uint32_t offset, const void* data, size_t size) {
        std::memcpy(bytes_.data() + offset, data, size);
    }

    const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

// Real prop_info layout: serial(4) value[92], then name bytes (not
// null-terminated in the file, matching prop_trie_node's own
// convention. Real code re-derives the name from the trie path, not
// from this copy, so its exact length here isn't load-bearing, but a
// real name is included anyway for any real tooling that dumps the
// area to inspect).
uint32_t serialize_prop_info(Arena& arena, const std::string& full_name, const std::string& value) {
    uint32_t info_offset = arena.reserve(kPropInfoFixedSize + full_name.size());
    // Confirmed in the engine encoding (SystemProperties::Read, this
    // project's own reading of the actual extracted libc.so): the
    // top 8 bits of serial are the real value length; bit 0 selects the
    // short-inline-value path (vs. a long-property callback this
    // project never needs; see property_area.h's own doc comment).
    uint32_t serial = static_cast<uint32_t>(value.size()) << 24;
    arena.write_u32(info_offset + 0, serial);
    // value[92], real null-terminated C string within the fixed field.
    arena.write_bytes(info_offset + 4, value.data(), value.size());
    // (the rest of value[92] is already zero from Arena::reserve's own
    // zero-init, giving a real null terminator for free)
    arena.write_bytes(info_offset + kPropInfoFixedSize, full_name.data(), full_name.size());
    return info_offset;
}

// Turns a sorted list of same-level siblings into a real, valid binary
// search tree via recursive midpoint selection (balance doesn't matter
// for correctness here, only that left/right correctly reflect
// name-comparison order the same way real prop_area::find_prop_trie_node
// itself navigates). Returns the offset of the subtree's own root node,
// or 0 if the range is empty.
uint32_t serialize_siblings(Arena& arena, const std::vector<const TrieBuildNode*>& siblings, size_t lo,
                             size_t hi, const std::string& prefix);

uint32_t serialize_children_of(Arena& arena, const TrieBuildNode& node, const std::string& prefix) {
    if (node.children.empty()) return 0;
    std::vector<const TrieBuildNode*> siblings;
    siblings.reserve(node.children.size());
    for (const auto& [name, child] : node.children) {
        siblings.push_back(child.get());
    }
    return serialize_siblings(arena, siblings, 0, siblings.size(), prefix);
}

uint32_t serialize_siblings(Arena& arena, const std::vector<const TrieBuildNode*>& siblings, size_t lo,
                             size_t hi, const std::string& prefix) {
    if (lo >= hi) return 0;
    size_t mid = lo + (hi - lo) / 2;
    const TrieBuildNode& node = *siblings[mid];
    std::string full_name = prefix.empty() ? node.token : prefix + "." + node.token;

    // Reserve this node's own slot first (so its offset is known before
    // recursing), then patch in prop/left/right/children afterward,
    // matches the "allocate, then fill in real cross-references" bump-
    // allocator pattern the whole format needs, since offsets can only
    // point at already-reserved space.
    uint32_t node_offset = arena.reserve(20 + node.token.size());
    arena.write_u32(node_offset + 0, static_cast<uint32_t>(node.token.size()));
    arena.write_bytes(node_offset + 20, node.token.data(), node.token.size());

    uint32_t left = serialize_siblings(arena, siblings, lo, mid, prefix);
    uint32_t right = serialize_siblings(arena, siblings, mid + 1, hi, prefix);
    uint32_t children = serialize_children_of(arena, node, full_name);
    uint32_t prop = node.has_value ? serialize_prop_info(arena, full_name, node.value) : 0;

    arena.write_u32(node_offset + 4, prop);
    arena.write_u32(node_offset + 8, left);
    arena.write_u32(node_offset + 12, right);
    arena.write_u32(node_offset + 16, children);
    return node_offset;
}

}  // namespace

bool write_property_area(const std::string& output_path,
                          const std::vector<std::pair<std::string, std::string>>& properties) {
    if (properties.empty()) {
        return false;
    }

    // Build the real in-memory trie tree from the flat dotted-name list.
    TrieBuildNode root;
    for (const auto& [name, value] : properties) {
        if (name.empty() || value.size() > kPropValueMax - 1) {
            // Honest failure; see property_area.h's own doc
            // comment: never silently truncate/corrupt a value into a
            // wrong-but-plausible-looking one.
            return false;
        }
        TrieBuildNode* cur = &root;
        size_t pos = 0;
        while (pos <= name.size()) {
            size_t dot = name.find('.', pos);
            std::string token = name.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            auto [it, inserted] = cur->children.try_emplace(token, std::make_unique<TrieBuildNode>());
            it->second->token = token;
            cur = it->second.get();
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
        cur->has_value = true;
        cur->value = value;
    }

    // Real prop_area header (128 bytes), then the trie starting at
    // data_+0, the arena's own offset 0 IS the real root, no separate
    // "root pointer" field exists in the format (confirmed via this
    // project's own reading of it: prop_area::find_property starts its walk
    // from data_ directly).
    Arena arena;
    // Real root level: root.children are the actual top-level property
    // segments (e.g. "net" for "net.dns1"), root itself carries no
    // value.
    serialize_children_of(arena, root, "");

    size_t data_size = arena.bytes().size();
    size_t total_size = kHeaderSize + data_size;

    std::vector<uint8_t> file(total_size, 0);
    uint32_t bytes_used = static_cast<uint32_t>(data_size);
    std::memcpy(file.data() + 0, &bytes_used, 4);
    // serial_ (offset 4) left at 0; no real concurrent writer exists
    // for this static, generated-once file.
    std::memcpy(file.data() + 8, &kPropAreaMagic, 4);
    std::memcpy(file.data() + 12, &kPropAreaVersion, 4);
    // reserved_[28] (offset 16..127) already zero.
    std::memcpy(file.data() + kHeaderSize, arena.bytes().data(), data_size);

    std::string tmp_path = output_path + ".tmp";
    FILE* f = std::fopen(tmp_path.c_str(), "wb");
    if (f == nullptr) {
        return false;
    }
    size_t written = std::fwrite(file.data(), 1, file.size(), f);
    bool ok = (written == file.size()) && (std::fclose(f) == 0);
    if (!ok) {
        std::remove(tmp_path.c_str());
        return false;
    }
    if (std::rename(tmp_path.c_str(), output_path.c_str()) != 0) {
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

}  // namespace stud::bionic_runtime
