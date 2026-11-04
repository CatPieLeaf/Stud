// What a Roblox APK asks of Stud, compared without running it: the entry
// points Stud calls and their signatures, the Java members the engine reaches
// for, and the libc symbols it imports. Everything is read from the file, so
// each read is checked against the file's size.

#include "stud/android_glue.h"
#include "stud/linker.h"

#include <miniz.h>
#include <miniz_zip.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace stud::android_glue {

namespace {

struct EngineEntryPoint {
    const char* symbol;
    const char* method;  // class/name.method
    const char* signature;
    bool required;
};
#include "engine_contract.inc"

constexpr uint32_t kAccNative = 0x100;

class Bytes {
public:
    explicit Bytes(const std::vector<uint8_t>& data) : data_(data) {}

    template <typename T>
    T at(size_t offset) const {
        if (offset > data_.size() || sizeof(T) > data_.size() - offset) {
            throw ExtractError("stud: the file ends before what it says it holds");
        }
        T v{};
        std::memcpy(&v, data_.data() + offset, sizeof(T));
        return v;
    }

    uint32_t uleb(size_t& p) const {
        uint32_t result = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            const uint8_t b = at<uint8_t>(p++);
            result |= static_cast<uint32_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return result;
        }
        throw ExtractError("stud: a malformed number in a DEX file");
    }

    std::string c_string(size_t p) const {
        if (p >= data_.size()) throw ExtractError("stud: a string past the end of the file");
        const auto* begin = reinterpret_cast<const char*>(data_.data()) + p;
        const void* end = std::memchr(begin, 0, data_.size() - p);
        if (end == nullptr) throw ExtractError("stud: an unterminated string");
        return std::string(begin, static_cast<const char*>(end));
    }

    size_t size() const { return data_.size(); }

private:
    const std::vector<uint8_t>& data_;
};

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ExtractError("stud: cannot read '" + path + "'");
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

void read_dex_methods(const std::vector<uint8_t>& dex, DexMethods& out) {
    const Bytes b(dex);
    if (dex.size() < 0x70 || std::memcmp(dex.data(), "dex\n", 4) != 0) {
        throw ExtractError("stud: not a DEX file");
    }
    const auto id_table = [&b](size_t header) {
        return std::pair<uint32_t, uint32_t>{b.at<uint32_t>(header), b.at<uint32_t>(header + 4)};
    };
    const auto [string_count, string_off] = id_table(0x38);
    const auto [type_count, type_off] = id_table(0x40);
    const auto [proto_count, proto_off] = id_table(0x48);
    const auto [field_count, field_off] = id_table(0x50);
    const auto [method_count, method_off] = id_table(0x58);
    const auto [class_count, class_off] = id_table(0x60);

    const auto string_at = [&](uint32_t i) {
        if (i >= string_count) throw ExtractError("stud: a string index past the DEX's table");
        size_t p = b.at<uint32_t>(string_off + size_t{i} * 4);
        b.uleb(p);
        return b.c_string(p);
    };
    const auto type_at = [&](uint32_t i) {
        if (i >= type_count) throw ExtractError("stud: a type index past the DEX's table");
        return string_at(b.at<uint32_t>(type_off + size_t{i} * 4));
    };
    const auto signature_of = [&](uint32_t proto) {
        if (proto >= proto_count) throw ExtractError("stud: a proto index past the DEX's table");
        const size_t at = proto_off + size_t{proto} * 12;
        const uint32_t params = b.at<uint32_t>(at + 8);
        std::string s = "(";
        if (params != 0) {
            const uint32_t n = b.at<uint32_t>(params);
            for (uint32_t i = 0; i < n; ++i) {
                s += type_at(b.at<uint16_t>(params + 4 + size_t{i} * 2));
            }
        }
        return s + ")" + type_at(b.at<uint32_t>(at + 4));
    };

    for (uint32_t c = 0; c < class_count; ++c) {
        const size_t def = class_off + size_t{c} * 32;
        const uint32_t data = b.at<uint32_t>(def + 24);
        if (data == 0) continue;
        std::string name = type_at(b.at<uint32_t>(def));
        if (name.size() < 2) continue;
        name = name.substr(1, name.size() - 2);
        size_t p = data;
        const uint32_t static_fields = b.uleb(p);
        const uint32_t instance_fields = b.uleb(p);
        const uint32_t direct_methods = b.uleb(p);
        const uint32_t virtual_methods = b.uleb(p);
        auto& members = out.members[name];
        for (const uint32_t count : {static_fields, instance_fields}) {
            uint32_t index = 0;
            for (uint32_t i = 0; i < count; ++i) {
                index += b.uleb(p);
                b.uleb(p);
                if (index >= field_count) throw ExtractError("stud: a field index past the table");
                members.insert(string_at(b.at<uint32_t>(field_off + size_t{index} * 8 + 4)));
            }
        }
        for (const uint32_t count : {direct_methods, virtual_methods}) {
            uint32_t index = 0;
            for (uint32_t i = 0; i < count; ++i) {
                index += b.uleb(p);
                const uint32_t flags = b.uleb(p);
                b.uleb(p);
                if (index >= method_count) throw ExtractError("stud: a method index past the table");
                const size_t at = method_off + size_t{index} * 8;
                const std::string method = string_at(b.at<uint32_t>(at + 4));
                members.insert(method);
                if ((flags & kAccNative) != 0) {
                    out.native_signatures[name + "." + method].insert(
                        signature_of(b.at<uint16_t>(at + 2)));
                }
            }
        }
    }
}

ElfSymbols read_elf_symbols(const std::vector<uint8_t>& elf) {
    const Bytes b(elf);
    if (elf.size() < 64 || std::memcmp(elf.data(), "\x7f" "ELF\x02", 5) != 0) {
        throw ExtractError("stud: not a 64-bit ELF file");
    }
    constexpr uint32_t kShtDynsym = 11;
    const uint64_t sections = b.at<uint64_t>(0x28);
    const uint16_t section_size = b.at<uint16_t>(0x3a);
    const uint16_t section_count = b.at<uint16_t>(0x3c);
    ElfSymbols result;
    for (uint32_t i = 0; i < section_count; ++i) {
        const size_t header = sections + size_t{i} * section_size;
        if (b.at<uint32_t>(header + 4) != kShtDynsym) continue;
        const uint64_t offset = b.at<uint64_t>(header + 0x18);
        const uint64_t size = b.at<uint64_t>(header + 0x20);
        const uint32_t link = b.at<uint32_t>(header + 0x28);
        const uint64_t strings = b.at<uint64_t>(sections + size_t{link} * section_size + 0x18);
        constexpr size_t kSymbolSize = 24;
        for (size_t at = offset; at + kSymbolSize <= offset + size; at += kSymbolSize) {
            const uint32_t name = b.at<uint32_t>(at);
            const uint8_t info = b.at<uint8_t>(at + 4);
            const uint16_t index = b.at<uint16_t>(at + 6);
            if (name == 0) continue;
            std::string symbol = b.c_string(strings + name);
            const bool weak = (info >> 4) == 2;
            if (index != 0) {
                result.defined.insert(std::move(symbol));
            } else if (!weak) {
                result.imported.insert(std::move(symbol));
            }
        }
        break;
    }
    return result;
}

ApkCheckReport check_apk_contents(const DexMethods& dex, const ElfSymbols& engine,
                                  const std::vector<uint8_t>& engine_bytes,
                                  const std::set<std::string>* provided) {
    ApkCheckReport report;
    const auto exported = [&engine](const std::string& s) { return engine.defined.count(s) != 0; };
    for (const EngineEntryPoint& e : kEngineEntryPoints) {
        const std::string symbol = e.symbol;
        std::string bumped;
        for (std::vector<std::string> next = linker::LoadedLibrary::next_names(symbol); !next.empty();) {
            const std::string name = next.front();
            next.erase(next.begin());
            if (!exported(name)) continue;
            bumped = name;
            next = linker::LoadedLibrary::next_names(name);
        }
        if (!exported(symbol)) {
            std::string line = "the engine no longer has " + std::string(e.method);
            if (!bumped.empty()) line += "; it has " + bumped;
            (e.required ? report.blocking : report.warnings).push_back(line);
            continue;
        }
        if (!bumped.empty()) {
            report.notes.push_back(std::string(e.method) + " also has a newer form, " + bumped);
        }
        const auto declared = dex.native_signatures.find(e.method);
        if (declared != dex.native_signatures.end() && declared->second.count(e.signature) == 0) {
            const std::string line = std::string(e.method) + " changed from " + e.signature +
                                     " to " + *declared->second.begin();
            (e.required ? report.blocking : report.warnings).push_back(line);
        }
    }

    // A Java member is one the engine wants when its own strings name it.
    const std::string_view text(reinterpret_cast<const char*>(engine_bytes.data()),
                                engine_bytes.size());
    const std::set<std::string_view> known(std::begin(kStudJavaMembers), std::end(kStudJavaMembers));
    for (const char* cls : kStudJavaClasses) {
        const auto it = dex.members.find(cls);
        if (it == dex.members.end()) {
            report.notes.push_back(std::string("the APK has no class ") + cls);
            continue;
        }
        for (const std::string& member : it->second) {
            if (member.size() < 6 || known.count(member) != 0 || member.front() == '<') continue;
            if (dex.native_signatures.count(std::string(cls) + "." + member) != 0) continue;
            std::string needle(1, '\0');
            needle += member;
            needle.push_back('\0');
            if (text.find(needle) != std::string_view::npos) {
                report.notes.push_back(std::string("the engine may want ") + cls + "." + member);
            }
        }
    }

    if (provided != nullptr) {
        for (const std::string& symbol : engine.imported) {
            if (provided->count(symbol) == 0) {
                report.blocking.push_back("nothing Stud bundles provides " + symbol);
            }
        }
    }
    return report;
}

ApkCheckReport check_apk(const std::string& apk_path, const std::string& engine_path,
                         const std::vector<std::string>& library_dirs) {
    DexMethods dex;
    for (const std::string& source : resolve_apk_sources_for_metadata(apk_path)) {
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, source.c_str(), 0)) continue;
        const mz_uint files = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < files; ++i) {
            char name[64];
            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
            const std::string_view entry(name);
            if (entry.rfind("classes", 0) != 0 || entry.size() < 4 ||
                entry.substr(entry.size() - 4) != ".dex") {
                continue;
            }
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (data == nullptr) continue;
            const auto* bytes = static_cast<const uint8_t*>(data);
            const std::vector<uint8_t> copy(bytes, bytes + size);
            mz_free(data);
            read_dex_methods(copy, dex);
        }
        mz_zip_reader_end(&zip);
    }

    const std::vector<uint8_t> engine_bytes = read_file(engine_path);
    const ElfSymbols engine = read_elf_symbols(engine_bytes);

    std::set<std::string> provided;
    for (const std::string& dir : library_dirs) {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.path().string().find(".so") == std::string::npos ||
                !entry.is_regular_file()) {
                continue;
            }
            try {
                const ElfSymbols library = read_elf_symbols(read_file(entry.path().string()));
                provided.insert(library.defined.begin(), library.defined.end());
            } catch (const ExtractError&) {
                // A linker script, or something else that is no library.
            }
        }
    }
    return check_apk_contents(dex, engine, engine_bytes, library_dirs.empty() ? nullptr : &provided);
}

}  // namespace stud::android_glue
