// Reads real metadata out of an APK's own compiled AndroidManifest.xml.
//
// Stud used to report a hardcoded "2.733.988" as the app version, in
// seven separate places, to an engine binary that could be any build,
// exactly the kind of guessed-but-plausible value this project's own
// rules forbid. The real version is sitting in the APK the user chose;
// this reads it.
//
// AndroidManifest.xml inside an APK is Android's binary XML ("AXML"), a
// simple chunked format: a header, a string pool, an optional resource
// map, then element chunks whose attributes carry either a raw string
// index or a typed value. Only what is needed to find one attribute on
// the root <manifest> element is implemented here; anything unexpected
// makes this return an empty string rather than guess.

#include "stud/android_glue.h"

#include <miniz.h>
#include <miniz_zip.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace stud::android_glue {
namespace {

constexpr uint16_t kChunkStringPool = 0x0001;
constexpr uint16_t kChunkStartElement = 0x0102;
constexpr uint32_t kStringPoolUtf8Flag = 1u << 8;
// Android's own typed-value data types (ResourceTypes.h).
constexpr uint8_t kTypeString = 0x03;
constexpr uint8_t kTypeIntDec = 0x10;

uint16_t read_u16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
uint32_t read_u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// Decodes one entry of the string pool. UTF-16 entries are narrowed to
// ASCII, which is all a version name or an element name ever is here;
// any character outside that makes the whole read fail rather than
// silently mangle it.
std::string pool_string(const std::vector<uint8_t>& data, size_t pool_offset, uint32_t count,
                        uint32_t flags, size_t strings_start, uint32_t index) {
    if (index >= count) return {};
    const size_t offsets_at = pool_offset + 28;
    if (offsets_at + (index + 1) * 4 > data.size()) return {};
    const uint32_t rel = read_u32(data.data() + offsets_at + index * 4);
    const size_t at = pool_offset + strings_start + rel;
    if (at + 2 > data.size()) return {};

    if ((flags & kStringPoolUtf8Flag) != 0) {
        size_t p = at;
        // Two length fields (UTF-16 length, then byte length), each one
        // or two bytes depending on the high bit.
        for (int field = 0; field < 2; ++field) {
            if (p >= data.size()) return {};
            if ((data[p] & 0x80) != 0) {
                p += 2;
            } else {
                p += 1;
            }
        }
        size_t len_at = at;
        if ((data[len_at] & 0x80) != 0) len_at += 2; else len_at += 1;
        if (len_at >= data.size()) return {};
        size_t byte_len = (data[len_at] & 0x80) != 0
                              ? ((static_cast<size_t>(data[len_at] & 0x7f) << 8) | data[len_at + 1])
                              : data[len_at];
        if (p + byte_len > data.size()) return {};
        return std::string(reinterpret_cast<const char*>(data.data() + p), byte_len);
    }

    size_t p = at;
    uint32_t len = read_u16(data.data() + p);
    p += 2;
    if ((len & 0x8000) != 0) {
        if (p + 2 > data.size()) return {};
        len = ((len & 0x7fff) << 16) | read_u16(data.data() + p);
        p += 2;
    }
    std::string out;
    out.reserve(len);
    for (uint32_t i = 0; i < len; ++i) {
        if (p + 2 > data.size()) return {};
        const uint16_t ch = read_u16(data.data() + p);
        p += 2;
        if (ch == 0 || ch > 0x7f) return {};
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::vector<uint8_t> read_manifest_bytes(const std::string& apk_path) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, apk_path.c_str(), 0)) return {};
    int index = mz_zip_reader_locate_file(&zip, "AndroidManifest.xml", nullptr, 0);
    if (index < 0) {
        mz_zip_reader_end(&zip);
        return {};
    }
    size_t size = 0;
    void* raw = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(index), &size, 0);
    mz_zip_reader_end(&zip);
    if (raw == nullptr) return {};
    std::vector<uint8_t> out(static_cast<const uint8_t*>(raw),
                             static_cast<const uint8_t*>(raw) + size);
    mz_free(raw);
    return out;
}

}  // namespace

std::string apk_version_name(const std::string& apk_path) {
    // For a bundle, the manifest that carries the app's own version is
    // the base module's, the splits have their own, and they describe
    // the split, not the app.
    std::vector<std::string> sources;
    try {
        sources = resolve_apk_sources_for_metadata(apk_path);
    } catch (...) {
        return {};
    }
    for (const std::string& source : sources) {
        const std::vector<uint8_t> data = read_manifest_bytes(source);
        if (data.size() < 8) continue;

        // Locate the string pool (always the first chunk after the file
        // header) and remember what it takes to read entries from it.
        size_t pool_offset = 0;
        uint32_t pool_count = 0;
        uint32_t pool_flags = 0;
        size_t pool_strings_start = 0;

        size_t at = 8;  // past the file header
        std::string found;
        while (at + 8 <= data.size()) {
            const uint16_t chunk_type = read_u16(data.data() + at);
            const uint32_t chunk_size = read_u32(data.data() + at + 4);
            if (chunk_size < 8 || at + chunk_size > data.size()) break;

            if (chunk_type == kChunkStringPool) {
                pool_offset = at;
                pool_count = read_u32(data.data() + at + 8);
                pool_flags = read_u32(data.data() + at + 16);
                pool_strings_start = read_u32(data.data() + at + 20);
            } else if (chunk_type == kChunkStartElement && pool_count != 0) {
                // Element chunk body: ns(4) name(4) attributeStart(2)
                // attributeSize(2) attributeCount(2) ..., verified
                // against the real manifest of the configured build
                // rather than assumed.
                const uint16_t header_size = read_u16(data.data() + at + 2);
                const size_t body = at + header_size;
                if (body + 14 > data.size()) break;
                const uint16_t attr_start = read_u16(data.data() + body + 8);
                const uint16_t attr_size = read_u16(data.data() + body + 10);
                const uint16_t attr_count = read_u16(data.data() + body + 12);
                const size_t attrs_at = body + attr_start;
                if (attr_size < 20) break;
                for (uint16_t i = 0; i < attr_count; ++i) {
                    const size_t a = attrs_at + static_cast<size_t>(i) * attr_size;
                    if (a + 20 > data.size()) break;
                    const uint32_t name_index = read_u32(data.data() + a + 4);
                    (void)0;
                    if (pool_string(data, pool_offset, pool_count, pool_flags, pool_strings_start,
                                    name_index) != "versionName") {
                        continue;
                    }
                    const uint32_t raw_value = read_u32(data.data() + a + 8);
                    const uint8_t value_type = data[a + 15];
                    const uint32_t value_data = read_u32(data.data() + a + 16);
                    if (raw_value != 0xffffffffu) {
                        found = pool_string(data, pool_offset, pool_count, pool_flags,
                                            pool_strings_start, raw_value);
                    } else if (value_type == kTypeString) {
                        found = pool_string(data, pool_offset, pool_count, pool_flags,
                                            pool_strings_start, value_data);
                    } else if (value_type == kTypeIntDec) {
                        found = std::to_string(value_data);
                    }
                    break;
                }
                // versionName lives on the root <manifest> element, so
                // the first element chunk either has it or the manifest
                // genuinely does not declare one.
                break;
            }
            at += chunk_size;
        }
        if (!found.empty()) return found;
    }
    return {};
}

}  // namespace stud::android_glue
