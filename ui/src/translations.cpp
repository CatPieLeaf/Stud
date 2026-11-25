#include "translations.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>

namespace stud::ui::translation {

namespace {

const QString kDirectory = QStringLiteral(":/i18n");
const QString kPrefix = QStringLiteral("stud_");
const QString kSuffix = QStringLiteral(".qm");

QStringList catalogues() {
    QStringList codes;
    for (const QString& file : QDir(kDirectory).entryList({QStringLiteral("stud_*.qm")}, QDir::Files)) {
        codes << file.mid(kPrefix.size(), file.size() - kPrefix.size() - kSuffix.size());
    }
    codes.sort();
    return codes;
}

// What a catalogue may be for: the code itself, then the language it is a variant of.
QStringList wanted_codes(const std::string& language) {
    QStringList wanted;
    if (language == "system") {
        for (const QString& tag : QLocale::system().uiLanguages()) {
            wanted << QString(tag).replace(QLatin1Char('-'), QLatin1Char('_'));
        }
    } else {
        wanted << QString::fromStdString(language);
    }
    for (const QString& code : QStringList(wanted)) {
        const qsizetype separator = code.indexOf(QLatin1Char('_'));
        if (separator > 0) wanted << code.left(separator);
    }
    return wanted;
}

QString pick(const QStringList& wanted, const QStringList& have) {
    for (const QString& code : wanted) {
        if (have.contains(code)) return code;
    }
    return {};
}

// Qt's own catalogue for the code, which may only be there as a variant of it.
bool load_qt_catalogue(QTranslator& translator, const QString& code) {
    const QString directory = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    QStringList candidates = wanted_codes(code.toStdString());
    for (const QString& extra : wanted_codes("system")) candidates << extra;
    for (const QString& candidate : candidates) {
        const QString path = directory + QStringLiteral("/qtbase_%1.qm").arg(candidate);
        if (translator.load(path)) return true;
    }
    return false;
}

}  // namespace

QStringList languages() { return catalogues(); }

QString language_name(const QString& code) {
    const QLocale locale(code);
    QString name = locale.nativeLanguageName();
    if (name.isEmpty()) return code;
    name[0] = name[0].toUpper();
    if (code.contains(QLatin1Char('_'))) {
        const QString territory = locale.nativeTerritoryName();
        if (!territory.isEmpty()) name += QStringLiteral(" (%1)").arg(territory);
    }
    return name;
}

void install(const std::string& language) {
    const QString code = pick(wanted_codes(language), catalogues());

    static QTranslator catalogue;
    if (!code.isEmpty() && catalogue.load(kDirectory + QStringLiteral("/stud_%1.qm").arg(code))) {
        QCoreApplication::installTranslator(&catalogue);
    }

    static QTranslator qt;
    if (!code.isEmpty() && load_qt_catalogue(qt, code)) {
        QCoreApplication::installTranslator(&qt);
    }
}

}  // namespace stud::ui::translation
