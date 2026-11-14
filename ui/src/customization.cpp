#include "customization.h"

#include "stud/stud_paths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

namespace stud::ui::customization {

namespace {

// Where each generated part lives in a customization folder, and the files it covers.
struct PartFiles {
    QString directory;
    QStringList files;
};

PartFiles files_of(Part part) {
    switch (part) {
        case Part::Cursor:
            return {QStringLiteral("content/textures/Cursors/KeyboardMouse"),
                    {QStringLiteral("ArrowCursor.png"), QStringLiteral("ArrowFarCursor.png"),
                     QStringLiteral("IBeamCursor.png")}};
        case Part::ShiftLock:
            return {QStringLiteral("content/textures"), {QStringLiteral("MouseLockedCursor.png")}};
        case Part::EmoteWheel:
            return {QStringLiteral("content/textures/ui/Emotes/Large"),
                    {QStringLiteral("SelectedGradient.png"), QStringLiteral("SelectedGradient@2x.png"),
                     QStringLiteral("SelectedGradient@3x.png"), QStringLiteral("SelectedLine.png"),
                     QStringLiteral("SelectedLine@2x.png"), QStringLiteral("SelectedLine@3x.png")}};
        case Part::DeathSound:
            return {QStringLiteral("content/sounds"), {QStringLiteral("ouch.ogg")}};
        case Part::Font:
            return {QStringLiteral("content/fonts"),
                    {QStringLiteral("CustomFont.ttf"), QStringLiteral("CustomFont.otf")}};
    }
    return {};
}

// The build's record of what this module changed, beside the bundle, and the
// originals it replaced.
QString state_dir(const client::Build& build) {
    return QFileInfo(build.app).absolutePath() + QStringLiteral("/.stud-customizations");
}
QString resources(const client::Build& build) { return build.app + QStringLiteral("/Contents/Resources"); }

bool copy_over(const QString& from, const QString& to) {
    QDir().mkpath(QFileInfo(to).absolutePath());
    QFile::remove(to);
    return QFile::copy(from, to);
}

// Puts back every file a previous apply() changed: the original where there
// was one, nothing where there was not.
void restore(const client::Build& build) {
    const QString state = state_dir(build);
    QFile manifest(state + QStringLiteral("/manifest"));
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString& relative : QString::fromUtf8(manifest.readAll()).split('\n', Qt::SkipEmptyParts)) {
            const QString target = resources(build) + QLatin1Char('/') + relative;
            const QString original = state + QStringLiteral("/originals/") + relative;
            if (QFileInfo::exists(original)) {
                copy_over(original, target);
            } else {
                QFile::remove(target);
            }
        }
    }
    QDir(state).removeRecursively();
}

class Applier {
public:
    explicit Applier(const client::Build& build) : build_(build), state_(state_dir(build)) {}

    // Writes `data` (or the file `from`) to `relative`, keeping the original.
    bool put(const QString& relative, const QString& from, const QByteArray* data = nullptr) {
        const QString target = resources(build_) + QLatin1Char('/') + relative;
        if (QFileInfo::exists(target) && !copy_over(target, state_ + QStringLiteral("/originals/") + relative)) {
            return false;
        }
        changed_ << relative;
        if (data != nullptr) {
            QDir().mkpath(QFileInfo(target).absolutePath());
            QSaveFile out(target);
            return out.open(QIODevice::WriteOnly) && out.write(*data) == data->size() && out.commit();
        }
        return copy_over(from, target);
    }

    bool finish() {
        QDir().mkpath(state_);
        QSaveFile manifest(state_ + QStringLiteral("/manifest"));
        return manifest.open(QIODevice::WriteOnly) &&
               manifest.write(changed_.join(QLatin1Char('\n')).toUtf8()) >= 0 && manifest.commit();
    }

    // Recorded as each file changes, so a failure part way can be undone.
    bool record() { return finish(); }

private:
    const client::Build& build_;
    QString state_;
    QStringList changed_;
};

}  // namespace

QString folder(client::Product product) {
    return QString::fromStdString(stud::paths::config_dir()) +
           (product == client::Product::Studio ? QStringLiteral("/studio-customizations") : QStringLiteral("/customizations"));
}

bool apply(const client::Build& build, client::Product product, QString* error) {
    restore(build);
    const QDir customizations(folder(product));
    if (!customizations.exists()) return true;

    Applier applier(build);
    const QString root = customizations.absolutePath() + QLatin1Char('/');
    QDirIterator it(customizations.absolutePath(), QDir::Files | QDir::NoSymLinks | QDir::Hidden,
                    QDirIterator::Subdirectories);
    QString font;
    while (it.hasNext()) {
        const QString path = it.next();
        const QString relative = path.mid(root.size());
        if (relative.startsWith(QLatin1String("..")) || QDir::cleanPath(relative) != relative) continue;
        if (relative == QLatin1String("content/fonts/CustomFont.ttf") ||
            relative == QLatin1String("content/fonts/CustomFont.otf")) {
            font = QFileInfo(relative).fileName();
        }
        if (!applier.put(relative, path)) {
            applier.record();
            restore(build);
            *error = QStringLiteral("could not apply the customization file %1").arg(relative);
            return false;
        }
    }

    // Every font family the customizations do not replace themselves, pointed at the
    // custom font.
    if (!font.isEmpty()) {
        const QString asset = QStringLiteral("rbxasset://fonts/") + font;
        const QDir families(resources(build) + QStringLiteral("/content/fonts/families"));
        for (const QString& name : families.entryList({QStringLiteral("*.json")}, QDir::Files)) {
            const QString relative = QStringLiteral("content/fonts/families/") + name;
            if (QFileInfo::exists(root + relative)) continue;
            QFile in(families.filePath(name));
            if (!in.open(QIODevice::ReadOnly)) continue;
            QJsonObject family = QJsonDocument::fromJson(in.readAll()).object();
            in.close();
            QJsonArray faces = family.value(QStringLiteral("faces")).toArray();
            if (faces.isEmpty()) continue;
            for (QJsonValueRef face : faces) {
                QJsonObject f = face.toObject();
                f.insert(QStringLiteral("assetId"), asset);
                face = f;
            }
            family.insert(QStringLiteral("faces"), faces);
            const QByteArray data = QJsonDocument(family).toJson(QJsonDocument::Compact);
            if (!applier.put(relative, QString(), &data)) {
                applier.record();
                restore(build);
                *error = QStringLiteral("could not point %1 at the custom font").arg(name);
                return false;
            }
        }
    }
    if (!applier.finish()) {
        restore(build);
        *error = QStringLiteral("could not record the applied customizations");
        return false;
    }
    return true;
}

bool use_file(Part part, const QString& file, QString* error) {
    const PartFiles p = files_of(part);
    const QString dir = folder(client::Product::Player) + QLatin1Char('/') + p.directory;
    remove(part);
    QStringList targets = p.files;
    if (part == Part::Font) {
        targets = {file.endsWith(QLatin1String(".otf"), Qt::CaseInsensitive) ? p.files.at(1) : p.files.at(0)};
    } else if (part == Part::Cursor) {
        targets = {p.files.at(0), p.files.at(1)};  // the text cursor stays Roblox's
    }
    for (const QString& name : targets) {
        if (!copy_over(file, dir + QLatin1Char('/') + name)) {
            *error = QStringLiteral("could not copy %1 into the customizations folder").arg(QFileInfo(file).fileName());
            return false;
        }
    }
    return true;
}

bool recolour(Part part, const QColor& colour, QString* error) {
    const auto build = client::installed(client::Product::Player);
    if (!build) {
        *error = QStringLiteral("Roblox is not installed yet; open it once first.");
        return false;
    }
    const PartFiles p = files_of(part);
    const QString dir = folder(client::Product::Player) + QLatin1Char('/') + p.directory;
    remove(part);
    for (const QString& name : p.files) {
        // From the copy Stud kept if a customization replaced it, else the client's own.
        const QString relative = p.directory + QLatin1Char('/') + name;
        QString source = state_dir(*build) + QStringLiteral("/originals/") + relative;
        if (!QFileInfo::exists(source)) source = resources(*build) + QLatin1Char('/') + relative;
        QImage image(source);
        if (image.isNull()) continue;
        image = image.convertToFormat(QImage::Format_ARGB32);
        // Multiplied in: white becomes the colour, black outlines stay black,
        // and greys shade between; each pixel keeps its own alpha.
        for (int y = 0; y < image.height(); ++y) {
            auto* line = reinterpret_cast<QRgb*>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = qAlpha(line[x]);
                if (alpha == 0) continue;
                line[x] = qRgba(qRed(line[x]) * colour.red() / 255, qGreen(line[x]) * colour.green() / 255,
                                qBlue(line[x]) * colour.blue() / 255, alpha);
            }
        }
        QDir().mkpath(dir);
        if (!image.save(dir + QLatin1Char('/') + name, "PNG")) {
            *error = QStringLiteral("could not write %1").arg(name);
            return false;
        }
    }
    return true;
}

void remove(Part part) {
    const PartFiles p = files_of(part);
    for (const QString& name : p.files) {
        QFile::remove(folder(client::Product::Player) + QLatin1Char('/') + p.directory + QLatin1Char('/') + name);
    }
}

bool present(Part part) {
    const PartFiles p = files_of(part);
    for (const QString& name : p.files) {
        if (QFileInfo::exists(folder(client::Product::Player) + QLatin1Char('/') + p.directory +
                              QLatin1Char('/') + name)) {
            return true;
        }
    }
    return false;
}

}  // namespace stud::ui::customization
