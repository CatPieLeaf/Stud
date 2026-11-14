// Mods over a client build: applied, and put back exactly as the build came.

#include "mods.h"
#include "check.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

namespace {

void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    f.write(data);
}

QByteArray read(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <scratch-dir>\n", argv[0]);
        return 1;
    }
    const std::string dir = argv[1];
    fresh_dir(dir);
    const QString root = QString::fromStdString(dir);
    ::setenv("XDG_CONFIG_HOME", (dir + "/config").c_str(), 1);

    // A build, as the client manager keeps one.
    stud::ui::client::Build build;
    build.app = root + "/client/version-x/RobloxPlayer.app";
    const QString res = build.app + "/Contents/Resources";
    write(res + "/content/sounds/ouch.ogg", "original ouch");
    write(res + "/content/fonts/families/Arial.json",
          R"({"name":"Arial","faces":[{"name":"Regular","assetId":"rbxasset://fonts/arial.ttf"}]})");

    const QString mods = stud::ui::mods::folder(stud::ui::client::Product::Player);
    write(mods + "/content/sounds/ouch.ogg", "modded ouch");
    write(mods + "/content/textures/new.png", "a new texture");
    write(mods + "/content/fonts/CustomFont.ttf", "font");
    write(root + "/outside.txt", "outside");
    QDir().mkpath(mods + "/content/linked");
    ::symlink((root + "/outside.txt").toLocal8Bit().constData(),
              (mods + "/content/linked/escape.txt").toLocal8Bit().constData());

    QString error;
    check(stud::ui::mods::apply(build, stud::ui::client::Product::Player, &error), "mods apply");
    check(read(res + "/content/sounds/ouch.ogg") == "modded ouch", "a mod replaces a client file");
    check(read(res + "/content/textures/new.png") == "a new texture", "a mod adds a file");
    check(!QFileInfo::exists(res + "/content/linked/escape.txt"), "a symlink in a mod is not followed");
    const QJsonObject family =
        QJsonDocument::fromJson(read(res + "/content/fonts/families/Arial.json")).object();
    check(family["faces"].toArray().at(0).toObject()["assetId"].toString() ==
              "rbxasset://fonts/CustomFont.ttf",
          "the custom font replaces every family's faces");

    // Applying again is the same, not mods stacked on mods.
    check(stud::ui::mods::apply(build, stud::ui::client::Product::Player, &error), "mods apply again");
    check(read(res + "/content/sounds/ouch.ogg") == "modded ouch", "a second apply is the same");

    // Without mods, the build is as it came.
    QDir(mods).removeRecursively();
    check(stud::ui::mods::apply(build, stud::ui::client::Product::Player, &error), "no mods apply");
    check(read(res + "/content/sounds/ouch.ogg") == "original ouch", "the original comes back");
    check(!QFileInfo::exists(res + "/content/textures/new.png"), "an added file goes again");
    check(read(res + "/content/fonts/families/Arial.json").contains("arial.ttf"),
          "the font families come back");

    std::printf("all mods checks passed\n");
    return 0;
}
