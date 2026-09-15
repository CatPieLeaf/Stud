// The log export writes a real gzipped tar by hand, so this checks it
// against the format rather than against itself: the gzip framing, the
// ustar headers, their checksums, and every file's contents.
//
// Where a real `tar` is on the machine, it is asked too, an archive
// that only Stud can read is not an export.

#include "log_archive.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QString>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <miniz.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s: %s\n", condition ? "ok" : "FAIL", what);
    if (!condition) ++failures;
}

std::vector<unsigned char> gunzip(const QByteArray& gz) {
    // Skip the 10-byte header this writer emits (no extra fields), then
    // inflate the raw deflate stream.
    if (gz.size() < 18) return {};
    std::size_t out_len = 0;
    void* out = ::tinfl_decompress_mem_to_heap(gz.constData() + 10,
                                               static_cast<std::size_t>(gz.size()) - 10 - 8,
                                               &out_len, 0);
    if (out == nullptr) return {};
    std::vector<unsigned char> result(static_cast<unsigned char*>(out),
                                      static_cast<unsigned char*>(out) + out_len);
    ::mz_free(out);
    return result;
}

unsigned long long from_octal(const char* field, std::size_t width) {
    return std::strtoull(std::string(field, width).c_str(), nullptr, 8);
}

}  // namespace

int main() {
    const QString dir = QDir::temp().filePath(QStringLiteral("stud-log-archive-test"));
    QDir().mkpath(dir);

    QStringList sources;
    for (int i = 0; i < 3; ++i) {
        const QString path = QDir(dir).filePath(QStringLiteral("session-%1.log").arg(i));
        QFile file(path);
        check(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
              "the fixture log opened for writing");
        // Sizes that are deliberately not multiples of 512, so the block
        // padding is actually exercised.
        file.write(QByteArray(100 + i * 517, static_cast<char>('a' + i)));
        file.close();
        sources << path;
    }

    const QString target = QDir(dir).filePath(QStringLiteral("stud-logs.tar.gz"));
    QString error;
    check(stud::ui::write_log_tarball(target, sources, &error), "the archive is written");

    QFile written(target);
    check(written.open(QIODevice::ReadOnly), "the archive exists");
    const QByteArray gz = written.readAll();
    written.close();

    check(gz.size() > 18 && static_cast<unsigned char>(gz.at(0)) == 0x1f &&
              static_cast<unsigned char>(gz.at(1)) == 0x8b &&
              static_cast<unsigned char>(gz.at(2)) == 0x08,
          "it begins with the gzip magic and the deflate method");

    const std::vector<unsigned char> tar = gunzip(gz);
    check(!tar.empty(), "the gzip stream inflates");
    check(tar.size() % 512 == 0, "the tar is a whole number of blocks");

    // The trailer gzip carries: CRC32 and the uncompressed size.
    const unsigned char* trailer = reinterpret_cast<const unsigned char*>(gz.constData()) +
                                   gz.size() - 8;
    const unsigned long stored_crc = static_cast<unsigned long>(trailer[0]) |
                                     (static_cast<unsigned long>(trailer[1]) << 8) |
                                     (static_cast<unsigned long>(trailer[2]) << 16) |
                                     (static_cast<unsigned long>(trailer[3]) << 24);
    const unsigned long stored_size = static_cast<unsigned long>(trailer[4]) |
                                      (static_cast<unsigned long>(trailer[5]) << 8) |
                                      (static_cast<unsigned long>(trailer[6]) << 16) |
                                      (static_cast<unsigned long>(trailer[7]) << 24);
    check(stored_size == tar.size(), "the trailer's size matches what inflated");
    check(stored_crc == ::mz_crc32(MZ_CRC32_INIT, tar.data(), tar.size()),
          "the trailer's CRC32 matches what inflated");

    int files_seen = 0;
    bool every_checksum_ok = true;
    bool every_body_ok = true;
    bool one_directory = false;
    for (std::size_t offset = 0; offset + 512 <= tar.size();) {
        const char* header = reinterpret_cast<const char*>(tar.data()) + offset;
        if (header[0] == '\0') break;  // the end-of-archive blocks

        unsigned sum = 0;
        for (int i = 0; i < 512; ++i) {
            sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(header[i]);
        }
        if (sum != from_octal(header + 148, 8)) every_checksum_ok = false;
        if (std::string(header + 257, 5) != "ustar") every_checksum_ok = false;

        const std::string name(header, strnlen(header, 100));
        const auto size = static_cast<std::size_t>(from_octal(header + 124, 12));
        offset += 512;
        if (header[156] == '5') {
            one_directory = true;
            continue;
        }

        // Its contents must be exactly what was written, byte for byte.
        const QString source = QDir(dir).filePath(
            QString::fromStdString(name.substr(name.find_last_of('/') + 1)));
        QFile original(source);
        original.open(QIODevice::ReadOnly);
        const QByteArray expected = original.readAll();
        if (static_cast<std::size_t>(expected.size()) != size ||
            std::memcmp(tar.data() + offset, expected.constData(), size) != 0) {
            every_body_ok = false;
        }
        ++files_seen;
        offset += (size + 511) / 512 * 512;
    }
    check(one_directory, "everything is under one top-level directory");
    check(files_seen == sources.size(), "every log is in the archive");
    check(every_checksum_ok, "every header is a valid ustar header");
    check(every_body_ok, "every file's contents survive the round trip");

    // And, where there is one, a real tar must agree.
    if (std::system("command -v tar >/dev/null 2>&1") == 0) {
        const std::string command =
            "tar -tzf '" + target.toStdString() + "' >/dev/null 2>&1";
        check(std::system(command.c_str()) == 0, "a real tar reads the archive");
    } else {
        std::printf("ok: no tar on this machine to cross-check with\n");
    }

    QDir(dir).removeRecursively();
    if (failures != 0) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("all log-archive checks passed\n");
    return 0;
}
