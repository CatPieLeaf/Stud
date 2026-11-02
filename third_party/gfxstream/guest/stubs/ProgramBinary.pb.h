// Stand-in for the protobuf-generated ProgramBinary.pb.h (ProgramBinary.proto
// in GLESv2_enc), with only what GL2Encoder uses. The bytes are only ever read
// back by this same encoder (glGetProgramBinary -> glProgramBinary), so a
// plain length-prefixed layout replaces protobuf's wire format:
//   u64 format, u64 binary length, binary, u64 index count, indices.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace gfxstream::guest::gles2 {

class ProgramBinaryInfo {
public:
    class GuestProgramInfo {
    public:
        void add_external_sampler_uniform_indices(uint64_t v) { indices_.push_back(v); }
        const std::vector<uint64_t>& external_sampler_uniform_indices() const { return indices_; }
        std::vector<uint64_t> indices_;
    };
    class HostProgramInfo {
    public:
        void set_binary_format(uint64_t v) { format_ = v; }
        uint64_t binary_format() const { return format_; }
        void set_binary(const std::string& v) { binary_ = v; }
        const std::string& binary() const { return binary_; }
        uint64_t format_ = 0;
        std::string binary_;
    };

    GuestProgramInfo* mutable_guest_program_info() { return &guest_; }
    const GuestProgramInfo& guest_program_info() const { return guest_; }
    HostProgramInfo* mutable_host_program_info() { return &host_; }
    const HostProgramInfo& host_program_info() const { return host_; }

    bool SerializeToString(std::string* out) const {
        out->clear();
        put(out, host_.format_);
        put(out, host_.binary_.size());
        out->append(host_.binary_);
        put(out, guest_.indices_.size());
        for (uint64_t v : guest_.indices_) put(out, v);
        return true;
    }

    bool ParseFromString(const std::string& in) {
        size_t at = 0;
        uint64_t len = 0, count = 0;
        if (!get(in, &at, &host_.format_) || !get(in, &at, &len) || in.size() - at < len) {
            return false;
        }
        host_.binary_.assign(in, at, len);
        at += len;
        if (!get(in, &at, &count) || (in.size() - at) / sizeof(uint64_t) < count) return false;
        guest_.indices_.resize(count);
        for (auto& v : guest_.indices_) get(in, &at, &v);
        return at == in.size();
    }

private:
    static void put(std::string* out, uint64_t v) {
        out->append(reinterpret_cast<const char*>(&v), sizeof(v));
    }
    static bool get(const std::string& in, size_t* at, uint64_t* v) {
        if (in.size() - *at < sizeof(*v)) return false;
        std::memcpy(v, in.data() + *at, sizeof(*v));
        *at += sizeof(*v);
        return true;
    }

    GuestProgramInfo guest_;
    HostProgramInfo host_;
};

}  // namespace gfxstream::guest::gles2
