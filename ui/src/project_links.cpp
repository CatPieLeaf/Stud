#include "project_links.h"

#include <QDesktopServices>
#include <QUrl>

#include "mono_icon.h"

namespace stud::ui {

namespace {

QPushButton* link_button(QWidget* parent, const QString& name, const QString& logo, const QString& url,
                         bool labelled) {
    auto* button = new QPushButton(labelled ? name : QString(), parent);
    button->setIcon(mono_resource_icon(logo));
    button->setToolTip(name);
    QObject::connect(button, &QPushButton::clicked, button, [url] { QDesktopServices::openUrl(QUrl(url)); });
    return button;
}

}  // namespace

QPushButton* github_button(QWidget* parent, bool labelled) {
    return link_button(parent, QStringLiteral("GitHub"), QStringLiteral(":/github.png"),
                       QStringLiteral("https://github.com/CatPieLeaf/Stud"), labelled);
}

QPushButton* discord_button(QWidget* parent, bool labelled) {
    return link_button(parent, QStringLiteral("Discord"), QStringLiteral(":/discord.png"),
                       QStringLiteral("https://discord.gg/6dmBTA4cJw"), labelled);
}

}  // namespace stud::ui
