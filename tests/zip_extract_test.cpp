// The client download's unpacking: Studio's frameworks need symbolic links,
// and a downloaded archive must not write, or link, outside its folder.

#include "mac_client.h"
#include "check.h"

#include <miniz.h>

#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace {

struct Entry {
    std::string name;
    std::string data;
    uint32_t mode;  // Unix mode, file type included
};

void put16(std::string& out, uint16_t v) { out.append({char(v & 0xff), char(v >> 8)}); }
void put32(std::string& out, uint32_t v) {
    put16(out, uint16_t(v & 0xffff));
    put16(out, uint16_t(v >> 16));
}

// A zip of stored entries, written by hand: miniz's writer cannot set the
// Unix mode that marks a symbolic link.
void write_zip(const std::string& path, const std::vector<Entry>& entries) {
    std::string body, central;
    for (const Entry& e : entries) {
        const uint32_t crc = uint32_t(mz_crc32(MZ_CRC32_INIT,
                                               reinterpret_cast<const unsigned char*>(e.data.data()),
                                               e.data.size()));
        const uint32_t offset = uint32_t(body.size());
        put32(body, 0x04034b50);
        put16(body, 20);
        put16(body, 0);
        put16(body, 0);  // stored
        put32(body, 0);  // time, date
        put32(body, crc);
        put32(body, uint32_t(e.data.size()));
        put32(body, uint32_t(e.data.size()));
        put16(body, uint16_t(e.name.size()));
        put16(body, 0);
        body += e.name + e.data;

        put32(central, 0x02014b50);
        put16(central, (3 << 8) | 20);  // made on Unix
        put16(central, 20);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, crc);
        put32(central, uint32_t(e.data.size()));
        put32(central, uint32_t(e.data.size()));
        put16(central, uint16_t(e.name.size()));
        put32(central, 0);  // extra, comment
        put32(central, 0);  // disk, internal attributes
        put32(central, e.mode << 16);
        put32(central, offset);
        central += e.name;
    }
    std::string end;
    put32(end, 0x06054b50);
    put32(end, 0);
    put16(end, uint16_t(entries.size()));
    put16(end, uint16_t(entries.size()));
    put32(end, uint32_t(central.size()));
    put32(end, uint32_t(body.size()));
    put16(end, 0);
    std::ofstream(path, std::ios::binary) << body << central << end;
}

bool unpacks(const std::string& scratch, const char* name, const std::vector<Entry>& entries) {
    const std::string zip = scratch + "/" + name + ".zip";
    write_zip(zip, entries);
    QString error;
    return stud::ui::client::extract_zip(QString::fromStdString(zip),
                                         QString::fromStdString(scratch + "/" + name), &error);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    fresh_dir(dir);

    const uint32_t file = S_IFREG | 0644;
    const uint32_t link = S_IFLNK | 0777;

    // A framework's links, which Studio needs to load.
    check(unpacks(dir, "framework",
                  {{"App.app/Frameworks/Qt.framework/Versions/A/Qt", "binary", file},
                   {"App.app/Frameworks/Qt.framework/Versions/Current", "A", link},
                   {"App.app/Frameworks/Qt.framework/Qt", "Versions/Current/Qt", link}}),
          "a framework's relative links unpack");
    const QFileInfo current(QString::fromStdString(dir + "/framework/App.app/Frameworks/Qt.framework/Qt"));
    check(current.isSymLink() && current.exists(), "the framework's link resolves to its binary");

    check(!unpacks(dir, "absolute", {{"App.app/escape", "/etc/passwd", link}}),
          "a link to an absolute path is refused");
    check(!unpacks(dir, "outside", {{"App.app/escape", "../../../outside", link}}),
          "a link that climbs out of the folder is refused");
    check(!QFileInfo::exists(QString::fromStdString(dir + "/outside/App.app/escape")),
          "a refused link is not made");
    check(!unpacks(dir, "path", {{"../escaped-file", "data", file}}),
          "an entry named outside the folder is refused");
    check(!QFileInfo::exists(QString::fromStdString(dir + "/escaped-file")),
          "the refused entry is not written");

    // Last: a cancel holds for the rest of the process.
    stud::ui::client::cancel();
    check(!unpacks(dir, "cancelled", {{"App.app/file", "data", file}}), "a cancelled unpack fails");
    check(!QFileInfo::exists(QString::fromStdString(dir + "/cancelled/App.app/file")),
          "a cancelled unpack writes nothing");

    std::printf("all zip extract checks passed\n");
    return 0;
}
