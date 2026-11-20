// Roblox's code signature: the installed client passes as Roblox signed it,
// and a changed resource, an added file, a changed code page or the wrong
// identifier each fail. Needs an uncustomized client on disk (Stud's own, or the .app named
// by STUD_TEST_CLIENT), so it reports skipped where there is none, as in CI.

#include <QCoreApplication>
#include "check.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

#include "code_signature.h"
#include "stud/stud_paths.h"

namespace {

void expect(bool ok, const char* what, const QString& detail) {
    ::expect(ok, std::string(what) + " " + detail.toStdString());
}

QString installed_client() {
    if (const QString forced = qEnvironmentVariable("STUD_TEST_CLIENT"); !forced.isEmpty()) return forced;
    const QDir clients(QString::fromStdString(stud::paths::data_dir()) + QStringLiteral("/client"));
    for (const QString& version : clients.entryList({QStringLiteral("version-*")}, QDir::Dirs)) {
        // Customizations replace files Roblox sealed; the launch checks the seal before applying them.
        if (QFileInfo::exists(clients.filePath(version) + QStringLiteral("/.stud-customizations"))) continue;
        const QString app = clients.filePath(version) + QStringLiteral("/RobloxPlayer.app");
        if (QFileInfo(app + QStringLiteral("/Contents/_CodeSignature/CodeResources")).exists()) return app;
    }
    return {};
}

// Flip one byte of `path` at `at`, and give back a way to put it back.
QByteArray flip(const QString& path, qint64 at) {
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite)) return {};
    QByteArray original = file.readAll();
    QByteArray changed = original;
    changed[at] = char(changed[at] ^ 0x01);
    file.seek(0);
    file.write(changed);
    return original;
}

void restore(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) file.write(bytes);
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString client = installed_client();
    if (client.isEmpty()) {
        std::printf("skipped: no Roblox client without customizations on disk\n");
        return 77;
    }
    const QString identifier = qEnvironmentVariable("STUD_TEST_IDENTIFIER", QStringLiteral("com.roblox.RobloxPlayer"));
    const QString as_installed = stud::ui::codesign::verify_app(client, identifier);
    expect(as_installed.isEmpty(), "the installed client passes:", as_installed);
    expect(!stud::ui::codesign::verify_app(client, QStringLiteral("com.example.Other")).isEmpty(),
           "the wrong identifier fails");

    QTemporaryDir dir;
    const QString copy = dir.filePath(QFileInfo(client).fileName());
    if (QProcess::execute(QStringLiteral("cp"), {QStringLiteral("-a"), client, copy}) != 0) {
        std::fprintf(stderr, "FAILED: could not copy the client\n");
        return 1;
    }
    expect(stud::ui::codesign::verify_app(copy, identifier).isEmpty(), "an exact copy passes");

    // A sealed resource changed.
    QDirIterator resources(copy + QStringLiteral("/Contents/Resources"), QDir::Files, QDirIterator::Subdirectories);
    QString resource;
    while (resources.hasNext()) {
        resource = resources.next();
        if (QFileInfo(resource).size() > 0) break;
    }
    const QByteArray resource_bytes = flip(resource, 0);
    const QString changed = stud::ui::codesign::verify_app(copy, identifier);
    expect(changed.contains(QStringLiteral("was changed")), "a changed resource fails:", changed);
    restore(resource, resource_bytes);

    // A file added beside the sealed ones.
    const QString added = copy + QStringLiteral("/Contents/Resources/added.lua");
    QFile extra(added);
    if (extra.open(QIODevice::WriteOnly)) extra.write("print(1)");
    extra.close();
    const QString with_added = stud::ui::codesign::verify_app(copy, identifier);
    expect(with_added.contains(QStringLiteral("was added")), "an added file fails:", with_added);
    QFile::remove(added);

    // A page of the executable changed, past its headers.
    const QString executable = copy + QStringLiteral("/Contents/MacOS/") + QFileInfo(client).completeBaseName();
    const QByteArray executable_bytes = flip(executable, 0x10000);
    const QString patched = stud::ui::codesign::verify_app(copy, identifier);
    expect(patched.contains(QStringLiteral("code page")), "a changed code page fails:", patched);
    restore(executable, executable_bytes);

    // Nested code changed.
    const QString helper = copy + QStringLiteral("/Contents/MacOS/RobloxCrashHandler");
    const QByteArray helper_bytes = flip(helper, 0x10000);
    const QString nested = stud::ui::codesign::verify_app(copy, identifier);
    expect(!nested.isEmpty(), "changed nested code fails:", nested);
    restore(helper, helper_bytes);

    expect(stud::ui::codesign::verify_app(copy, identifier).isEmpty(), "the restored copy passes again");
    return finish("code_signature_test");
}
