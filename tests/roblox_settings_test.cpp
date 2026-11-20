// Roblox's settings file: one element read and changed, the rest untouched.

#include "roblox_settings.h"
#include "check.h"

#include <cstdio>

namespace rs = stud::ui::roblox_settings;

int main() {
    const QString xml = QStringLiteral(
        "<Properties>\n"
        "\t<bool name=\"CameraYInverted\">false</bool>\n"
        "\t<float name=\"MasterVolume\">0.5</float>\n"
        "\t<float name=\"MasterVolumeStudio\">1</float>\n"
        "</Properties>\n");

    const auto volume = rs::read(xml, QStringLiteral("MasterVolume"));
    expect(volume && volume->first == "float" && volume->second == "0.5", "reads type and value");
    expect(!rs::read(xml, QStringLiteral("Missing")), "an absent element reads as nothing");

    const QString changed = rs::write(xml, QStringLiteral("MasterVolume"), QStringLiteral("0.8"));
    expect(rs::read(changed, QStringLiteral("MasterVolume"))->second == "0.8", "writes the value");
    expect(rs::read(changed, QStringLiteral("MasterVolumeStudio"))->second == "1",
           "a name that starts the same is left alone");
    expect(QString(changed).replace(QStringLiteral("0.8"), QStringLiteral("0.5")) == xml,
           "nothing else changes");
    expect(rs::write(xml, QStringLiteral("Missing"), QStringLiteral("1")) == xml,
           "an absent element is not added");

    return finish("roblox_settings_test");
}
