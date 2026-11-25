#pragma once

#include <QString>
#include <QStringList>

#include <string>

namespace stud::ui::translation {

// Locale codes of the catalogues built into the binary, such as "pt".
QStringList languages();

// A language's name in that language, for the selector; the code when unknown.
QString language_name(const QString& code);

// Installs the catalogue for `language` ("system" follows the desktop) plus Qt's
// own translations for it. Call before anything builds a window: a missing
// catalogue, or English, leaves the strings as they are.
void install(const std::string& language);

}  // namespace stud::ui::translation
