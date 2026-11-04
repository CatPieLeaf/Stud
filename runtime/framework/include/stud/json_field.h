#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Reading one field out of a flat MessageBus payload. These are small
// objects the engine writes, not documents, so a parser would be more than
// they need; what has to be right is the escapes, and telling a key from a
// value that happens to spell it.
namespace stud::jni_bridge {

// Where the value of `key` starts, or npos.
inline size_t json_value_position(const std::string& json, const std::string& key) {
    if (key.empty()) return std::string::npos;
    const std::string needle = "\"" + key + "\"";
    for (size_t pos = json.find(needle); pos != std::string::npos;
         pos = json.find(needle, pos + 1)) {
        size_t next = pos + needle.size();
        while (next < json.size() && (json[next] == ' ' || json[next] == '\t')) ++next;
        if (next >= json.size() || json[next] != ':') continue;
        ++next;
        while (next < json.size() && (json[next] == ' ' || json[next] == '\t')) ++next;
        return next < json.size() ? next : std::string::npos;
    }
    return std::string::npos;
}

inline void json_append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// The four hex digits of a \u escape at `pos`, or -1.
inline int64_t json_hex4(const std::string& json, size_t pos) {
    if (pos + 4 > json.size()) return -1;
    uint32_t value = 0;
    for (size_t i = pos; i < pos + 4; ++i) {
        const char c = json[i];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32_t>(c - 'a') + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32_t>(c - 'A') + 10;
        } else {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

// The string value of `key`; empty when it is missing or is not a string.
inline std::string json_string_field(const std::string& json, const std::string& key) {
    size_t pos = json_value_position(json, key);
    if (pos == std::string::npos || json[pos] != '"') return {};
    ++pos;
    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] != '\\' || pos + 1 >= json.size()) {
            out.push_back(json[pos++]);
            continue;
        }
        const char escape = json[pos + 1];
        pos += 2;
        switch (escape) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                int64_t unit = json_hex4(json, pos);
                if (unit < 0) return out;
                pos += 4;
                // A character past U+FFFF is written as two escapes.
                if (unit >= 0xD800 && unit < 0xDC00 && pos + 6 <= json.size() &&
                    json[pos] == '\\' && json[pos + 1] == 'u') {
                    const int64_t low = json_hex4(json, pos + 2);
                    if (low >= 0xDC00 && low < 0xE000) {
                        unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                        pos += 6;
                    }
                }
                json_append_utf8(out, static_cast<uint32_t>(unit));
                break;
            }
            default: out.push_back(escape); break;
        }
    }
    return out;
}

inline bool json_true_field(const std::string& json, const std::string& key) {
    const size_t pos = json_value_position(json, key);
    return pos != std::string::npos && json.compare(pos, 4, "true") == 0;
}

// For a string value, control characters included: JSON does not allow them
// raw, and a URL handed in from outside can carry anything.
inline std::string json_escaped(const std::string& value) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (byte < 0x20) {
            out += "\\u00";
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0xF]);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace stud::jni_bridge
