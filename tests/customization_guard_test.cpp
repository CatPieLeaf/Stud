// The customizations crash guard, per run: a crashing first run with a changed
// set puts the last good set back and sets the new files aside; a clean run
// makes the set the last good one.

#include <QCoreApplication>
#include "check.h"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

#include "customization.h"

namespace {

using stud::ui::client::Product;
namespace guard = stud::ui::customization;

void write(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(path.left(path.lastIndexOf('/')));
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "FAILED: could not write %s\n", qPrintable(path));
        ++g_failures;
        return;
    }
    out.write(bytes);
}

QByteArray read(const QString& path) {
    QFile in(path);
    return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir dir;
    ::setenv("XDG_CONFIG_HOME", qPrintable(dir.path() + "/config"), 1);
    ::setenv("XDG_DATA_HOME", qPrintable(dir.path() + "/data"), 1);
    const QString folder = guard::folder(Product::Player);
    const QString aside = guard::disabled_folder(Product::Player);

    // The first run with the guard: what is there counts as good, even if it crashes.
    write(folder + "/content/textures/a.png", "old a");
    guard::begin_run(Product::Player);
    expect(guard::end_run(Product::Player, true).isEmpty(), "the set already there is never set aside");
    expect(read(folder + "/content/textures/a.png") == "old a", "the set already there stays");

    // A changed set whose first run crashes: back to the last good set.
    write(folder + "/content/textures/a.png", "new a");
    write(folder + "/content/sounds/b.ogg", "new b");
    guard::begin_run(Product::Player);
    const QStringList set_aside = guard::end_run(Product::Player, true);
    expect(set_aside.size() == 2, "both the changed and the added file are set aside");
    expect(read(folder + "/content/textures/a.png") == "old a", "the last good version comes back");
    expect(!QFile::exists(folder + "/content/sounds/b.ogg"), "the added file leaves the folder");
    expect(read(aside + "/content/textures/a.png") == "new a", "the changed file is kept aside");
    expect(read(aside + "/content/sounds/b.ogg") == "new b", "the added file is kept aside");

    // A changed set whose first run ends cleanly becomes the last good set.
    write(folder + "/content/fonts/c.ttf", "new c");
    guard::begin_run(Product::Player);
    expect(guard::end_run(Product::Player, false).isEmpty(), "a clean run sets nothing aside");
    guard::begin_run(Product::Player);
    expect(guard::end_run(Product::Player, true).isEmpty(),
           "a crash with an unchanged set is not the customizations' doing");
    expect(read(folder + "/content/fonts/c.ttf") == "new c", "the good set survives that crash");

    // Only the very next run is the trial.
    write(folder + "/content/fonts/d.ttf", "new d");
    guard::begin_run(Product::Player);
    expect(guard::end_run(Product::Player, false).isEmpty(), "the first run went well");
    guard::begin_run(Product::Player);
    expect(guard::end_run(Product::Player, true).isEmpty(), "a later crash leaves d alone");
    expect(read(folder + "/content/fonts/d.ttf") == "new d", "d stays");

    return finish("customization guard");
}
