#include "log_archive.h"

#include "log_scrub.h"

#include <QFile>
#include <QFileInfo>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <miniz.h>

namespace stud::ui {

namespace {

constexpr int kBlock = 512;

// Written by hand instead of running tar on a user-chosen path from a possibly
// AppImage environment; ustar is small and fixed-width.
// The slash splitting `name` into ustar's prefix (155 bytes) and name (100) fields:
// npos when it fits the name field, 0 when it can't be split.
std::size_t tar_split(const std::string& name) {
    if (name.size() <= 100) return std::string::npos;
    for (std::size_t slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
        if (slash <= 155 && slash > 0 && name.size() - slash - 1 <= 100 && name.size() - slash - 1 > 0) return slash;
    }
    return 0;
}

void append_header(std::vector<char>& out, const std::string& name, std::size_t size,
                   std::time_t mtime, char type) {
    const std::size_t start = out.size();
    out.resize(start + kBlock, '\0');
    char* header = out.data() + start;

    auto put = [&](std::size_t offset, const std::string& text, std::size_t field) {
        std::memcpy(header + offset, text.data(), std::min(text.size(), field));
    };
    auto octal = [](unsigned long long value, int width) {
        // Numeric fields end in NUL, leaving width - 1 digits.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%0*llo", width - 1, value);
        return std::string(buf);
    };

    const std::size_t split = tar_split(name);
    if (split == std::string::npos) {
        put(0, name, 100);
    } else {
        put(0, name.substr(split + 1), 100);
        put(345, name.substr(0, split), 155);
    }
    put(100, octal(type == '5' ? 0755 : 0644, 8), 8);
    put(108, octal(0, 8), 8);
    put(116, octal(0, 8), 8);
    put(124, octal(size, 12), 12);
    put(136, octal(static_cast<unsigned long long>(mtime), 12), 12);
    std::memset(header + 148, ' ', 8);  // checksum field is blank while summing
    header[156] = type;
    put(257, "ustar", 6);
    put(263, "00", 2);
    put(265, "stud", 32);
    put(297, "stud", 32);

    unsigned sum = 0;
    for (int i = 0; i < kBlock; ++i) sum += static_cast<unsigned char>(header[i]);
    const std::string checksum = octal(sum, 7);
    std::memcpy(header + 148, checksum.data(), checksum.size());
    header[154] = '\0';
    header[155] = ' ';
}

void append_padded(std::vector<char>& out, const QByteArray& data) {
    out.insert(out.end(), data.constData(), data.constData() + data.size());
    const std::size_t remainder = static_cast<std::size_t>(data.size()) % kBlock;
    if (remainder != 0) out.resize(out.size() + (kBlock - remainder), '\0');
}

// gzip around miniz's raw deflate: 10-byte header, stream, then little-endian
// CRC32 and size. miniz only writes zlib's wrapper.
bool gzip_to_file(const QString& target, const std::vector<char>& payload, QString* error) {
    std::size_t compressed_size = 0;
    void* compressed = ::tdefl_compress_mem_to_heap(
        payload.data(), payload.size(), &compressed_size, TDEFL_DEFAULT_MAX_PROBES);
    if (compressed == nullptr) {
        if (error != nullptr) *error = QStringLiteral("could not compress the logs");
        return false;
    }

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ::mz_free(compressed);
        if (error != nullptr) *error = file.errorString();
        return false;
    }

    const unsigned char header[10] = {0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03};
    bool ok = file.write(reinterpret_cast<const char*>(header), sizeof(header)) ==
              static_cast<qint64>(sizeof(header));
    ok = ok && file.write(static_cast<const char*>(compressed),
                          static_cast<qint64>(compressed_size)) ==
                   static_cast<qint64>(compressed_size);
    ::mz_free(compressed);

    const mz_ulong crc = ::mz_crc32(MZ_CRC32_INIT,
                                    reinterpret_cast<const unsigned char*>(payload.data()),
                                    payload.size());
    auto put32 = [&](unsigned long value) {
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(value & 0xff), static_cast<unsigned char>((value >> 8) & 0xff),
            static_cast<unsigned char>((value >> 16) & 0xff),
            static_cast<unsigned char>((value >> 24) & 0xff)};
        return file.write(reinterpret_cast<const char*>(bytes), 4) == 4;
    };
    ok = ok && put32(static_cast<unsigned long>(crc));
    ok = ok && put32(static_cast<unsigned long>(payload.size() & 0xffffffffu));
    file.close();
    if (!ok) {
        file.remove();
        if (error != nullptr) *error = QStringLiteral("could not write %1").arg(target);
        return false;
    }
    return true;
}

}  // namespace

bool write_log_tarball(const QString& target, const QStringList& files, QString* error,
                       const QStringList& client_logs) {
    if (files.isEmpty()) {
        if (error != nullptr) *error = QStringLiteral("there are no logs to export");
        return false;
    }

    // One top-level folder named after the archive, so unpacking leaves no scatter of files.
    QString root = QFileInfo(target).fileName();
    for (const QString& suffix : {QStringLiteral(".gz"), QStringLiteral(".tar")}) {
        if (root.endsWith(suffix)) root.chop(suffix.size());
    }
    if (root.isEmpty()) root = QStringLiteral("stud-logs");

    const std::time_t now = std::time(nullptr);
    std::vector<char> tar;
    append_header(tar, (root + QStringLiteral("/")).toStdString(), 0, now, '5');

    for (const QString& path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;  // vanished mid-export
        const QByteArray contents = file.readAll();
        file.close();
        const std::string name =
            (root + QStringLiteral("/") + QFileInfo(path).fileName()).toStdString();
        // Skipped rather than written under a truncated name.
        if (tar_split(name) == 0) continue;
        append_header(tar, name, static_cast<std::size_t>(contents.size()),
                      QFileInfo(path).lastModified().toSecsSinceEpoch(), '0');
        append_padded(tar, contents);
    }

    if (!client_logs.isEmpty()) {
        append_header(tar, (root + QStringLiteral("/roblox/")).toStdString(), 0, now, '5');
    }
    for (const QString& path : client_logs) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QByteArray contents;
        for (const QByteArray& line : file.readAll().split('\n')) {
            const std::string scrubbed =
                scrub_client_log_line(std::string_view(line.constData(), line.size()));
            contents.append(scrubbed.data(), static_cast<qsizetype>(scrubbed.size()));
            contents.append('\n');
        }
        const std::string name =
            (root + QStringLiteral("/roblox/") + QFileInfo(path).fileName()).toStdString();
        if (tar_split(name) == 0) continue;
        append_header(tar, name, static_cast<std::size_t>(contents.size()),
                      QFileInfo(path).lastModified().toSecsSinceEpoch(), '0');
        append_padded(tar, contents);
    }

    tar.resize(tar.size() + 2 * kBlock, '\0');
    return gzip_to_file(target, tar, error);
}

}  // namespace stud::ui
