#include "stud/shader_pack.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace stud::shader_pack {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203;
constexpr size_t kHeader = 0x54;
constexpr size_t kNameRecord = 68;
constexpr size_t kNameLength = 64;
constexpr size_t kPermRecord = 64;
constexpr size_t kBlobHeader = 24;
constexpr size_t kPermOffset = 16;
constexpr size_t kPermSize = 20;
constexpr size_t kPermKey = 24;
constexpr size_t kPermName = 30;

// SPIR-V opcodes, decorations and GLSL.std.450 instructions this file reads.
enum Op : uint32_t {
    kOpExtInst = 12,
    kOpCapability = 17,
    kOpTypeFloat = 22,
    kOpTypeVector = 23,
    kOpTypeMatrix = 24,
    kOpTypeImage = 25,
    kOpTypeSampledImage = 27,
    kOpTypeArray = 28,
    kOpTypeRuntimeArray = 29,
    kOpTypeStruct = 30,
    kOpTypePointer = 32,
    kOpConstant = 43,
    kOpConstantComposite = 44,
    kOpConstantNull = 46,
    kOpVariable = 59,
    kOpLoad = 61,
    kOpDecorate = 71,
    kOpMemberDecorate = 72,
    kOpVectorShuffle = 79,
    kOpCompositeExtract = 81,
    kOpFSub = 131,
    kOpFMul = 133,
    kOpVectorTimesScalar = 142,
};
constexpr uint32_t kFirstTypeOp = 19;  // OpTypeVoid
constexpr uint32_t kLastTypeOp = 33;   // OpTypeFunction
constexpr uint32_t kDecorationLocation = 30;
constexpr uint32_t kDecorationBinding = 33;
constexpr uint32_t kDecorationDescriptorSet = 34;
constexpr uint32_t kDecorationOffset = 35;
constexpr uint32_t kStorageInput = 1;
constexpr uint32_t kStorageOutput = 3;
constexpr uint32_t kGlslSqrt = 31;
constexpr uint32_t kGlslFMix = 46;

bool is_sample(uint32_t op) { return op >= 87 && op <= 90; }  // OpImageSample{Implicit,Explicit}Lod, Dref

uint16_t read16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
void write16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, sizeof v); }
void write32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof v); }

uint32_t float_bits(float f) {
    uint32_t v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

// One RBXS pack, read in place.
class Pack {
public:
    Pack(const std::vector<uint8_t>& data, const char* what) : data_(data) {
        if (data.size() < kHeader || std::memcmp(data.data(), "RBXS", 4) != 0) {
            throw PackError(std::string(what) + " shader pack: not an RBXS pack");
        }
        n1_ = read16(data.data() + 12);
        n2_ = read16(data.data() + 14);
        const size_t names_end = kHeader + size_t{n1_} * kNameRecord;
        if (names_end > data.size()) throw PackError(std::string(what) + " shader pack: truncated");
        for (size_t i = 0; i < n1_; ++i) {
            const char* at = reinterpret_cast<const char*>(data.data() + kHeader + i * kNameRecord);
            names_.emplace_back(at, strnlen(at, kNameLength));
        }
        base_ = find_permutation_table(names_end, what);
    }

    size_t names() const { return n1_; }
    size_t perms() const { return n2_; }
    const std::string& name(size_t i) const { return names_[i]; }
    const std::vector<std::string>& name_list() const { return names_; }
    const uint8_t* perm(size_t k) const { return data_.data() + base_ + k * kPermRecord; }
    uint32_t key(size_t k) const { return read32(perm(k) + kPermKey); }
    const std::string& perm_name(size_t k) const { return names_.at(read16(perm(k) + kPermName)); }
    uint32_t blob_offset(size_t k) const { return read32(perm(k) + kPermOffset); }
    std::string blob(size_t k) const {
        const uint32_t off = blob_offset(k), size = read32(perm(k) + kPermSize);
        return std::string(reinterpret_cast<const char*>(data_.data()) + off, size);
    }
    // The bytes between the name table and the permutation table, copied
    // through as they are.
    std::string middle() const {
        const size_t from = kHeader + size_t{n1_} * kNameRecord;
        return std::string(reinterpret_cast<const char*>(data_.data()) + from, base_ - from);
    }
    const std::vector<uint8_t>& data() const { return data_; }

private:
    bool points_at_spirv(size_t base, size_t k) const {
        const size_t at = base + k * kPermRecord;
        if (at + kPermRecord > data_.size()) return false;
        const uint32_t off = read32(data_.data() + at + kPermOffset);
        const uint32_t size = read32(data_.data() + at + kPermSize);
        if (size == 0 || size_t{off} + size > data_.size() || size < kBlobHeader + 4) return false;
        return read32(data_.data() + off + kBlobHeader) == kSpirvMagic;
    }

    // The two tables between the names and the permutations vary in size,
    // so where the permutation table starts is found rather than
    // computed: it ends where the first blob begins, and it is the one
    // position at which every record points at a SPIR-V module.
    size_t find_permutation_table(size_t names_end, const char* what) const {
        const uint8_t magic[4] = {0x03, 0x02, 0x23, 0x07};
        const auto first = std::search(data_.begin() + static_cast<ptrdiff_t>(names_end), data_.end(),
                                       std::begin(magic), std::end(magic));
        const size_t first_blob = static_cast<size_t>(first - data_.begin());
        const size_t table = size_t{n2_} * kPermRecord + kBlobHeader;
        if (first == data_.end() || first_blob < names_end + table) {
            throw PackError(std::string(what) + " shader pack: no permutation table");
        }
        for (size_t base = first_blob - table + 1; base-- > names_end;) {
            bool all = true;
            for (size_t k = 0; k < n2_ && all; ++k) all = points_at_spirv(base, k);
            if (all) return base;
        }
        throw PackError(std::string(what) + " shader pack: no permutation table");
    }

    const std::vector<uint8_t>& data_;
    uint16_t n1_ = 0;
    uint16_t n2_ = 0;
    std::vector<std::string> names_;
    size_t base_ = 0;
};

std::vector<uint32_t> spirv_words(const std::string& blob) {
    std::vector<uint32_t> w((blob.size() - kBlobHeader) / 4);
    std::memcpy(w.data(), blob.data() + kBlobHeader, w.size() * 4);
    return w;
}

// The blob with its module replaced, the header and any trailing bytes kept.
std::string with_words(const std::string& blob, const std::vector<uint32_t>& w) {
    const size_t old_words = (blob.size() - kBlobHeader) / 4;
    std::string out = blob.substr(0, kBlobHeader);
    out.append(reinterpret_cast<const char*>(w.data()), w.size() * 4);
    out.append(blob, kBlobHeader + old_words * 4, std::string::npos);
    return out;
}

// Word positions of every instruction after the module header.
std::vector<size_t> instructions(const std::vector<uint32_t>& w) {
    std::vector<size_t> out;
    size_t p = 5;
    while (p < w.size()) {
        const uint32_t len = w[p] >> 16;
        if (len == 0 || p + len > w.size()) break;
        out.push_back(p);
        p += len;
    }
    return out;
}

uint32_t opcode(const std::vector<uint32_t>& w, size_t p) { return w[p] & 0xffff; }
uint32_t length(const std::vector<uint32_t>& w, size_t p) { return w[p] >> 16; }

// What one module asks of the engine: per (set, binding) a structural
// fingerprint of the resource, independent of result ids, and the
// capabilities it declares.
struct Contract {
    std::map<std::pair<uint32_t, uint32_t>, std::string> bindings;
    std::vector<std::pair<uint32_t, uint32_t>> binding_list;  // sorted, duplicates kept
    std::set<uint32_t> capabilities;
};

class TypeTable {
public:
    void add(const std::vector<uint32_t>& w, size_t p) {
        types_[w[p + 1]] = {opcode(w, p), std::vector<uint32_t>(w.begin() + p + 2, w.begin() + p + length(w, p))};
    }
    void add_offset(uint32_t type, uint32_t member, uint32_t offset) { offsets_[type][member] = offset; }

    // The full fingerprint the substitution compares.
    std::string shape(uint32_t t) const {
        std::set<uint32_t> active;
        return shape(t, active);
    }

    // The coarser one the stage-interface check compares (no struct
    // members: built-in blocks carry no Location and are not compared).
    std::string io_shape(uint32_t t) const {
        std::set<uint32_t> active;
        return io_shape(t, active);
    }

private:
    static std::string raw(uint32_t op, const std::vector<uint32_t>& ops) {
        std::string s = std::to_string(op) + "(";
        for (uint32_t v : ops) s += std::to_string(v) + ",";
        return s + ")";
    }

    std::string shape(uint32_t t, std::set<uint32_t>& active) const {
        const auto it = types_.find(t);
        if (it == types_.end() || !active.insert(t).second) return "?";
        const auto& [op, ops] = it->second;
        std::string s;
        if (op == kOpTypeStruct) {
            s = "struct(";
            const auto off = offsets_.find(t);
            for (size_t i = 0; i < ops.size(); ++i) {
                std::string at = "-";
                if (off != offsets_.end()) {
                    if (auto m = off->second.find(static_cast<uint32_t>(i)); m != off->second.end()) {
                        at = std::to_string(m->second);
                    }
                }
                s += at + ":" + shape(ops[i], active) + ",";
            }
            s += ")";
        } else if (op == kOpTypePointer && ops.size() >= 2) {
            s = "ptr(" + std::to_string(ops[0]) + "," + shape(ops[1], active) + ")";
        } else if (op == kOpTypeArray && !ops.empty()) {
            s = "array(" + shape(ops[0], active) + ")";
        } else if (op == kOpTypeRuntimeArray && !ops.empty()) {
            s = "rtarray(" + shape(ops[0], active) + ")";
        } else if ((op == kOpTypeVector || op == kOpTypeMatrix) && ops.size() >= 2) {
            s = std::to_string(op) + "(" + shape(ops[0], active) + "," + std::to_string(ops[1]) + ")";
        } else if (op == kOpTypeImage && ops.size() >= 7) {
            s = "image(" + shape(ops[0], active);
            for (size_t i = 1; i < 7; ++i) s += "," + std::to_string(ops[i]);
            s += ")";
        } else if (op == kOpTypeSampledImage && !ops.empty()) {
            s = "sampledimage(" + shape(ops[0], active) + ")";
        } else {
            s = raw(op, ops);
        }
        active.erase(t);
        return s;
    }

    std::string io_shape(uint32_t t, std::set<uint32_t>& active) const {
        const auto it = types_.find(t);
        if (it == types_.end() || !active.insert(t).second) return "?";
        const auto& [op, ops] = it->second;
        std::string s;
        if (op == kOpTypePointer && ops.size() >= 2) {
            s = io_shape(ops[1], active);
        } else if ((op == kOpTypeVector || op == kOpTypeMatrix) && ops.size() >= 2) {
            s = std::to_string(op) + "(" + io_shape(ops[0], active) + "," + std::to_string(ops[1]) + ")";
        } else if (op == kOpTypeArray && !ops.empty()) {
            s = "array(" + io_shape(ops[0], active) + ")";
        } else {
            s = raw(op, ops);
        }
        active.erase(t);
        return s;
    }

    std::unordered_map<uint32_t, std::pair<uint32_t, std::vector<uint32_t>>> types_;
    std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> offsets_;
};

Contract contract_of(const std::vector<uint32_t>& w) {
    Contract c;
    TypeTable types;
    std::unordered_map<uint32_t, uint32_t> binding, dset, var_type;
    std::vector<uint32_t> binding_order;
    for (size_t p : instructions(w)) {
        const uint32_t op = opcode(w, p), len = length(w, p);
        if (op == kOpCapability && len >= 2) {
            c.capabilities.insert(w[p + 1]);
        } else if (op == kOpDecorate && len >= 4) {
            if (w[p + 2] == kDecorationBinding) {
                if (binding.find(w[p + 1]) == binding.end()) binding_order.push_back(w[p + 1]);
                binding[w[p + 1]] = w[p + 3];
            } else if (w[p + 2] == kDecorationDescriptorSet) {
                dset[w[p + 1]] = w[p + 3];
            }
        } else if (op == kOpMemberDecorate && len >= 5 && w[p + 3] == kDecorationOffset) {
            types.add_offset(w[p + 1], w[p + 2], w[p + 4]);
        } else if (op == kOpVariable && len >= 3) {
            var_type[w[p + 2]] = w[p + 1];
        } else if (op >= kFirstTypeOp && op <= kLastTypeOp && len >= 2) {
            types.add(w, p);
        }
    }
    for (uint32_t var : binding_order) {
        const uint32_t set = dset.count(var) ? dset[var] : 0;
        const auto type = var_type.find(var);
        c.bindings[{set, binding[var]}] = types.shape(type == var_type.end() ? 0 : type->second);
        c.binding_list.emplace_back(set, binding[var]);
    }
    std::sort(c.binding_list.begin(), c.binding_list.end());
    return c;
}

// Inputs and outputs by Location, with their types: what a vertex shader
// reads from the vertex data and hands to the fragment shader.
std::vector<std::tuple<uint32_t, uint32_t, std::string>> stage_interface(const std::vector<uint32_t>& w) {
    TypeTable types;
    std::unordered_map<uint32_t, uint32_t> location;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> var;  // id -> (type, storage class)
    for (size_t p : instructions(w)) {
        const uint32_t op = opcode(w, p), len = length(w, p);
        if (op == kOpDecorate && len >= 4 && w[p + 2] == kDecorationLocation) {
            location[w[p + 1]] = w[p + 3];
        } else if (op >= kFirstTypeOp && op <= kLastTypeOp && len >= 2) {
            types.add(w, p);
        } else if (op == kOpVariable && len >= 4) {
            var[w[p + 2]] = {w[p + 1], w[p + 3]};
        }
    }
    std::vector<std::tuple<uint32_t, uint32_t, std::string>> out;
    for (const auto& [id, loc] : location) {
        const auto v = var.find(id);
        if (v == var.end()) continue;
        const uint32_t storage = v->second.second;
        if (storage != kStorageInput && storage != kStorageOutput) continue;
        out.emplace_back(storage, loc, types.io_shape(v->second.first));
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The bookkeeping the three rewrites share.
struct Scan {
    std::unordered_map<uint32_t, uint32_t> binding, dset, loads;
    std::unordered_map<uint32_t, uint32_t> samples;  // sample result -> the sampled image it read
    std::vector<size_t> insts;

    explicit Scan(const std::vector<uint32_t>& w) : insts(instructions(w)) {
        std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> mixes;
        for (size_t p : insts) {
            const uint32_t op = opcode(w, p), len = length(w, p);
            if (op == kOpDecorate && len >= 4) {
                if (w[p + 2] == kDecorationBinding) binding[w[p + 1]] = w[p + 3];
                if (w[p + 2] == kDecorationDescriptorSet) dset[w[p + 1]] = w[p + 3];
            } else if (op == kOpLoad && len >= 4) {
                loads[w[p + 2]] = w[p + 3];
            } else if (is_sample(op) && len >= 5) {
                samples[w[p + 2]] = w[p + 3];
            } else if (op == kOpExtInst && len == 8 && w[p + 4] == kGlslFMix) {
                mixes.push_back({w[p + 2], {w[p + 5], w[p + 6]}});
            }
        }
        // Tiled*HideAllRep blends two samples of the same map and decodes
        // the blend: that blend is read like a sample of the map.
        for (const auto& [result, xy] : mixes) {
            const auto x = samples.find(xy.first), y = samples.find(xy.second);
            if (x != samples.end() && y != samples.end() && load_of(x->second) == load_of(y->second)) {
                samples[result] = x->second;
            }
        }
    }

    std::optional<uint32_t> load_of(uint32_t id) const {
        const auto it = loads.find(id);
        if (it == loads.end()) return std::nullopt;
        return it->second;
    }

    // The variables at set 1, binding `b`.
    std::set<uint32_t> material(uint32_t b) const {
        std::set<uint32_t> out;
        for (const auto& [v, bind] : binding) {
            const auto s = dset.find(v);
            if (s != dset.end() && s->second == 1 && bind == b) out.insert(v);
        }
        return out;
    }

    bool reads(uint32_t sample_result, const std::set<uint32_t>& vars) const {
        const auto s = samples.find(sample_result);
        if (s == samples.end()) return false;
        const auto l = load_of(s->second);
        return l && vars.count(*l) != 0;
    }
};

}  // namespace

// The desktop builds decode the normal maps at set 1 binding 4 (material)
// and 8 (detail) as two-channel maps: `.wy` (alpha and green) and z
// rebuilt. The Android engine binds RGB normal maps there, which its own
// builds read as `.xyz`, so `.wy` reads alpha (always 1) as x. Taking
// `.xy` reads the map the way it is stored. One word pair per site.
size_t rewrite_normal_reads(std::vector<uint32_t>& w) {
    const Scan scan(w);
    std::set<uint32_t> targets = scan.material(4);
    for (uint32_t v : scan.material(8)) targets.insert(v);
    size_t patched = 0;
    for (size_t p : scan.insts) {
        if (opcode(w, p) != kOpVectorShuffle || length(w, p) != 7) continue;
        if (w[p + 3] != w[p + 4] || w[p + 5] != 3 || w[p + 6] != 1) continue;
        if (!scan.reads(w[p + 3], targets)) continue;
        w[p + 5] = 0;
        w[p + 6] = 1;
        ++patched;
    }
    return patched;
}

// The desktop builds rebuild the normal's z as sqrt(clamp(1 - x*x - y*y))
// and, past a distance, fade x and y to 0 but not z. An Android map can
// have x*x + y*y >= 1, so z is 0 and the faded normal is (0,0,0), which
// normalize() turns into NaN, drawn black. The Android builds take z from
// the map; so does this, replacing that Sqrt with `sample.z * 2 - 1`, the
// same constants 2 and 1 the x/y decode just used.
size_t take_normal_z_from_map(std::vector<uint32_t>& w) {
    const Scan scan(w);
    const std::set<uint32_t> normal = scan.material(4);
    std::unordered_map<uint32_t, uint32_t> constant_bits;           // id -> value bits
    std::unordered_map<uint32_t, std::vector<uint32_t>> composites;  // id -> constituents
    for (size_t p : scan.insts) {
        const uint32_t op = opcode(w, p), len = length(w, p);
        if (op == kOpConstant && len == 4) constant_bits[w[p + 2]] = w[p + 3];
        if (op == kOpConstantComposite) composites[w[p + 2]] = {w.begin() + p + 3, w.begin() + p + len};
    }
    // `c` itself, or every constituent of a splat composite, as one constant.
    auto scalar_of = [&](uint32_t c) -> std::optional<uint32_t> {
        if (constant_bits.count(c)) return c;
        const auto it = composites.find(c);
        if (it == composites.end() || it->second.empty()) return std::nullopt;
        const auto& parts = it->second;
        if (std::any_of(parts.begin(), parts.end(), [&](uint32_t x) { return x != parts[0]; })) {
            return std::nullopt;
        }
        if (!constant_bits.count(parts[0])) return std::nullopt;
        return parts[0];
    };

    struct Site {
        uint32_t sample, float_type, two, one;
    };
    std::map<size_t, Site> sites;  // by the Sqrt's position
    for (size_t i = 0; i < scan.insts.size(); ++i) {
        const size_t p = scan.insts[i];
        // The decode starts at `.xy` of a sample of the normal map.
        if (opcode(w, p) != kOpVectorShuffle || length(w, p) != 7) continue;
        if (w[p + 3] != w[p + 4] || w[p + 5] != 0 || w[p + 6] != 1) continue;
        if (!scan.reads(w[p + 3], normal)) continue;
        const uint32_t sample = w[p + 3];
        // Then, following that value: `* 2`, `- 1`, and the Sqrt it feeds.
        std::set<uint32_t> derived{w[p + 2]};
        std::optional<uint32_t> two, one, scaled;
        std::optional<size_t> sqrt_at;
        for (size_t j = i + 1; j < scan.insts.size() && !sqrt_at; ++j) {
            const size_t q = scan.insts[j];
            const uint32_t op = opcode(w, q), len = length(w, q);
            if (len < 3) continue;
            bool uses = false;
            for (size_t o = q + 3; o < q + len; ++o) uses = uses || derived.count(w[o]) != 0;
            if (!uses) continue;
            if (op == kOpVectorTimesScalar && !scaled && w[q + 3] == w[p + 2]) {
                two = scalar_of(w[q + 4]);
                scaled = w[q + 2];
            } else if (op == kOpFSub && scaled && !one && (w[q + 3] == *scaled || w[q + 4] == *scaled)) {
                one = w[q + 3] == *scaled ? scalar_of(w[q + 4]) : scalar_of(w[q + 3]);
            } else if (op == kOpExtInst && len == 6 && w[q + 4] == kGlslSqrt && one) {
                sqrt_at = q;
                continue;
            }
            derived.insert(w[q + 2]);
        }
        if (!two || !one || !sqrt_at) continue;
        if (constant_bits[*two] != float_bits(2.0f) || constant_bits[*one] != float_bits(1.0f)) continue;
        sites[*sqrt_at] = {sample, w[*sqrt_at + 1], *two, *one};
    }
    if (sites.empty()) return 0;

    uint32_t bound = w[3];
    std::vector<uint32_t> out(w.begin(), w.begin() + 5);
    for (size_t p : scan.insts) {
        if (const auto site = sites.find(p); site != sites.end()) {
            const Site& s = site->second;
            const uint32_t z = w[p + 2], t = bound++, u = bound++;
            out.insert(out.end(), {(5u << 16) | kOpCompositeExtract, s.float_type, t, s.sample, 2});
            out.insert(out.end(), {(5u << 16) | kOpFMul, s.float_type, u, t, s.two});
            out.insert(out.end(), {(5u << 16) | kOpFSub, s.float_type, z, u, s.one});
            continue;
        }
        out.insert(out.end(), w.begin() + p, w.begin() + p + length(w, p));
    }
    out[3] = bound;
    w = std::move(out);
    return sites.size();
}

// The desktop Plastic builds add a detail normal from set 1 binding 8 on
// top of the normal map. No Android build reads 1/8, and what the Android
// engine binds there is not a detail map, so the term only adds noise
// (the "crunchy" plastic). Its strength, the first non-constant scale on
// the value read from 1/8 (after the constant `* 2` of the decode), is
// replaced with a float 0 constant of the module.
size_t drop_detail_normal(std::vector<uint32_t>& w) {
    const Scan scan(w);
    const std::set<uint32_t> detail = scan.material(8);
    std::set<uint32_t> float_types, constants;
    std::optional<uint32_t> zero;
    for (size_t p : scan.insts) {
        const uint32_t op = opcode(w, p), len = length(w, p);
        if (op == kOpConstant || op == kOpConstantComposite || op == kOpConstantNull) constants.insert(w[p + 2]);
        if (op == kOpTypeFloat && len >= 3 && w[p + 2] == 32) float_types.insert(w[p + 1]);
        if (!zero && op == kOpConstant && len == 4 && float_types.count(w[p + 1]) && w[p + 3] == 0) zero = w[p + 2];
        if (!zero && op == kOpConstantNull && float_types.count(w[p + 1])) zero = w[p + 2];
    }
    if (!zero) return 0;
    std::set<uint32_t> derived;
    size_t patched = 0;
    for (size_t p : scan.insts) {
        const uint32_t op = opcode(w, p), len = length(w, p);
        if (is_sample(op) && len >= 5) {
            const auto l = scan.load_of(w[p + 3]);
            if (l && detail.count(*l)) {
                derived.insert(w[p + 2]);
                continue;
            }
        }
        if (len < 3) continue;
        if (op == kOpVectorTimesScalar && len >= 5 && derived.count(w[p + 3])) {
            if (w[p + 4] == *zero) continue;  // already dropped
            if (!constants.count(w[p + 4])) {
                w[p + 4] = *zero;
                ++patched;
                continue;
            }
        }
        bool uses = false;
        for (size_t o = p + 3; o < p + len; ++o) uses = uses || derived.count(w[o]) != 0;
        if (uses) derived.insert(w[p + 2]);
    }
    return patched;
}

std::vector<uint8_t> build_desktop_materials(const std::vector<uint8_t>& mobile_bytes,
                                             const std::vector<uint8_t>& desktop_bytes,
                                             BuildReport* report) {
    const Pack mobile(mobile_bytes, "mobile");
    const Pack desktop(desktop_bytes, "desktop");
    BuildReport r;

    using Key = std::pair<std::string, uint32_t>;  // shader name, variant key
    auto keyed = [](const Pack& pack) {
        std::map<Key, size_t> out;
        for (size_t k = 0; k < pack.perms(); ++k) out[{pack.perm_name(k), pack.key(k)}] = k;
        return out;
    };
    const auto mk = keyed(mobile), dk = keyed(desktop);

    // Blobs are shared between permutations, so each is parsed once.
    std::unordered_map<uint32_t, Contract> mobile_contracts, desktop_contracts;
    auto contract = [](const Pack& pack, size_t k, std::unordered_map<uint32_t, Contract>& cache) -> const Contract& {
        auto it = cache.find(pack.blob_offset(k));
        if (it == cache.end()) it = cache.emplace(pack.blob_offset(k), contract_of(spirv_words(pack.blob(k)))).first;
        return it->second;
    };
    std::set<uint32_t> mobile_capabilities;
    for (size_t k = 0; k < mobile.perms(); ++k) {
        const auto& caps = contract(mobile, k, mobile_contracts).capabilities;
        mobile_capabilities.insert(caps.begin(), caps.end());
    }
    auto only_known_capabilities = [&](const Contract& c) {
        return std::includes(mobile_capabilities.begin(), mobile_capabilities.end(), c.capabilities.begin(),
                             c.capabilities.end());
    };

    // Where both packs build a variant, the desktop blob replaces the
    // mobile one when their descriptor contracts differ (the material
    // shaders: they read more of what the engine binds), unless it uses a
    // capability no mobile shader does, or reads a binding both declare as
    // a different kind of image (terrain: 2D arrays where the Android
    // engine binds 2D atlases). Material vertex shaders are paired with
    // their fragment shaders when the stage interface is identical.
    std::map<size_t, std::string> blobs;  // permutation -> blob
    for (const auto& [key, k] : mk) {
        const auto d = dk.find(key);
        if (d == dk.end()) continue;
        const Contract& mi = contract(mobile, k, mobile_contracts);
        const Contract& di = contract(desktop, d->second, desktop_contracts);
        const std::string& name = key.first;
        bool vs_pair = false;
        if (name.rfind("DefaultUnified", 0) == 0 && name.size() >= 2 && name.compare(name.size() - 2, 2, "VS") == 0) {
            const auto mb = spirv_words(mobile.blob(k)), db = spirv_words(desktop.blob(d->second));
            if (mb != db) {
                vs_pair = stage_interface(mb) == stage_interface(db);
                if (!vs_pair) ++r.refused_vertex_io;
            }
        }
        if (mi.bindings == di.bindings && !vs_pair) continue;
        if (!only_known_capabilities(di)) {
            ++r.refused_capability;
            continue;
        }
        const bool image_kind_differs =
            std::any_of(di.bindings.begin(), di.bindings.end(), [&](const auto& b) {
                const auto m = mi.bindings.find(b.first);
                return m != mi.bindings.end() && m->second != b.second &&
                       b.second.find("image") != std::string::npos;
            });
        if (image_kind_differs) {
            ++r.refused_image;
            continue;
        }
        std::string blob = desktop.blob(d->second);
        auto words = spirv_words(blob);
        r.normal_reads += rewrite_normal_reads(words);
        r.normal_z += take_normal_z_from_map(words);
        if (name.rfind("DefaultUnifiedPlastic", 0) == 0) r.detail_dropped += drop_detail_normal(words);
        blobs[k] = with_words(blob, words);
        ++r.substituted;
    }

    std::vector<std::vector<uint8_t>> perms;
    for (size_t k = 0; k < mobile.perms(); ++k) perms.emplace_back(mobile.perm(k), mobile.perm(k) + kPermRecord);

    // Desktop-only variants of shaders the mobile pack has (the outline
    // passes among them), where both packs agree on that shader's base
    // variant's bindings: then they were built against the same engine
    // contract, and what a variant adds is what the engine sets up when
    // it asks for that variant.
    const auto& names = mobile.name_list();
    for (const auto& [key, k] : dk) {
        if (mk.count(key)) continue;
        const auto name_at = std::find(names.begin(), names.end(), key.first);
        if (name_at == names.end()) continue;
        const auto mbase = mk.find({key.first, 0}), dbase = dk.find({key.first, 0});
        if (mbase == mk.end() || dbase == dk.end()) continue;
        if (contract(mobile, mbase->second, mobile_contracts).binding_list !=
            contract(desktop, dbase->second, desktop_contracts).binding_list) {
            continue;
        }
        if (!only_known_capabilities(contract(desktop, k, desktop_contracts))) continue;
        std::vector<uint8_t> rec(desktop.perm(k), desktop.perm(k) + kPermRecord);
        write16(rec.data() + kPermName, static_cast<uint16_t>(name_at - names.begin()));
        perms.push_back(std::move(rec));
        blobs[perms.size() - 1] = desktop.blob(k);
        ++r.grafted;
    }

    // The engine asks for lit-tier keys (top nibble 2, 4 or 8) with bit
    // 0x400, which only turns the normal toward the camera, and neither
    // pack builds them; Windows never asks. Each missing one is answered
    // with its one-sided blob, so those draws are lit instead of failing
    // their lookup.
    std::set<std::pair<uint16_t, uint32_t>> have;
    for (const auto& rec : perms) have.insert({read16(rec.data() + kPermName), read32(rec.data() + kPermKey)});
    const size_t before_aliases = perms.size();
    for (size_t idx = 0; idx < before_aliases; ++idx) {
        const uint16_t ni = read16(perms[idx].data() + kPermName);
        const uint32_t key = read32(perms[idx].data() + kPermKey);
        const uint32_t tier = key >> 28;
        if ((tier != 2 && tier != 4 && tier != 8) || (key & 0x400) || have.count({ni, key | 0x400})) continue;
        std::vector<uint8_t> alias = perms[idx];
        write32(alias.data() + kPermKey, key | 0x400);
        have.insert({ni, key | 0x400});
        const auto own = blobs.find(idx);
        std::string blob = own != blobs.end() ? own->second : mobile.blob(idx);
        perms.push_back(std::move(alias));
        blobs[perms.size() - 1] = std::move(blob);
        ++r.aliases;
    }

    // Written as the mobile pack is laid out: its header (with the new
    // permutation count), name table and middle tables, the permutation
    // records, then each distinct blob once.
    if (perms.size() > UINT16_MAX) throw PackError("combined shader pack: too many permutations");
    const std::string middle = mobile.middle();
    const size_t names_size = mobile.names() * kNameRecord;
    const size_t blob_start = kHeader + names_size + middle.size() + perms.size() * kPermRecord;
    std::vector<uint8_t> out(mobile_bytes.begin(), mobile_bytes.begin() + static_cast<ptrdiff_t>(kHeader + names_size));
    write16(out.data() + 14, static_cast<uint16_t>(perms.size()));
    out.insert(out.end(), middle.begin(), middle.end());
    std::string body;
    std::unordered_map<std::string, uint32_t> placed;
    for (size_t k = 0; k < perms.size(); ++k) {
        const auto own = blobs.find(k);
        const std::string blob = own != blobs.end() ? own->second : mobile.blob(k);
        auto at = placed.find(blob);
        if (at == placed.end()) {
            const size_t offset = blob_start + body.size();
            if (offset + blob.size() > UINT32_MAX) throw PackError("combined shader pack: too large");
            at = placed.emplace(blob, static_cast<uint32_t>(offset)).first;
            body += blob;
        }
        write32(perms[k].data() + kPermOffset, at->second);
        write32(perms[k].data() + kPermSize, static_cast<uint32_t>(blob.size()));
        out.insert(out.end(), perms[k].begin(), perms[k].end());
    }
    out.insert(out.end(), body.begin(), body.end());
    // Read back: every permutation must point at a SPIR-V module.
    (void)Pack(out, "combined");
    if (report != nullptr) *report = r;
    return out;
}

}  // namespace stud::shader_pack
