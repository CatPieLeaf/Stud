#pragma once

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace stud::jni_bridge {

// Replaces `path` with `text`, or leaves it exactly as it was and returns false.
// Written beside it and renamed over it, so a reader never sees half a file.
//
// Judged after the close: a small file only reaches the disk there, so a stream
// that still looks good before it can be hiding a refused write.
inline bool write_file_atomically(const std::string& path, const std::string& text) {
    const std::string temp = path + ".tmp." + std::to_string(::getpid());
    std::error_code ec;
    std::ofstream out(temp, std::ios::trunc);
    if (!out.is_open()) return false;
    out << text;
    out.close();
    if (out.fail()) {
        std::filesystem::remove(temp, ec);
        return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (!ec) return true;
    std::filesystem::remove(temp, ec);
    return false;
}

}  // namespace stud::jni_bridge
