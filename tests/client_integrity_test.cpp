// The client's code manifest notices a changed code file, and ignores what
// Stud changes inside the bundle on purpose: FFlags and customizations.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

#include "mac_client.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(path.left(path.lastIndexOf('/')));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "FAILED: could not write %s\n", qPrintable(path));
        ++failures;
        return;
    }
    out.write(bytes);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    const QString bundle = dir.path() + QStringLiteral("/RobloxPlayer.app");
    const QString contents = bundle + QStringLiteral("/Contents");

    expect(stud::ui::client::code_manifest(bundle).isEmpty(), "a bundle with no code has no manifest");

    write(contents + "/Info.plist", "plist");
    write(contents + "/MacOS/RobloxPlayer", "the player");
    write(contents + "/MacOS/libmimalloc.3.dylib", "a library");
    write(contents + "/MacOS/RobloxMenuBar.app/Contents/MacOS/RobloxMenuBar", "a helper");
    write(contents + "/Resources/content/textures/a.png", "texture");
    const QByteArray shipped = stud::ui::client::code_manifest(bundle);
    expect(!shipped.isEmpty(), "a bundle with code has a manifest");
    expect(shipped.contains("Contents/MacOS/RobloxMenuBar.app/Contents/MacOS/RobloxMenuBar"),
           "nested code is covered");
    expect(!shipped.contains("Resources"), "content is not covered");

    write(contents + "/MacOS/ClientSettings/ClientAppSettings.json", "{\"FFlag\": true}");
    write(contents + "/Resources/content/textures/a.png", "a customization");
    expect(stud::ui::client::code_manifest(bundle) == shipped,
           "FFlags and customizations leave the manifest as it was");

    write(contents + "/MacOS/RobloxPlayer", "the player, patched");
    expect(stud::ui::client::code_manifest(bundle) != shipped, "a changed executable is noticed");
    write(contents + "/MacOS/RobloxPlayer", "the player");
    expect(stud::ui::client::code_manifest(bundle) == shipped, "the same bytes give the same manifest");

    write(contents + "/MacOS/injected.dylib", "something added");
    expect(stud::ui::client::code_manifest(bundle) != shipped, "an added file is noticed");
    QFile::remove(contents + "/MacOS/injected.dylib");

    write(contents + "/Info.plist", "plist, edited");
    expect(stud::ui::client::code_manifest(bundle) != shipped, "a changed Info.plist is noticed");

    if (failures == 0) std::printf("client integrity: ok\n");
    return failures == 0 ? 0 : 1;
}
