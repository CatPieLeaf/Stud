#include "chooser.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QToolButton>
#include <QVBoxLayout>

namespace stud::ui {

namespace {

// The app's own icon once it is installed; Stud's until then, or where Qt
// cannot read an .icns (it needs the qt6-qtimageformats plugin).
QIcon icon_of(client::Product product) {
    if (const auto build = client::installed(product)) {
        const QIcon icon(build->app + QStringLiteral("/Contents/Resources/AppIcon.icns"));
        if (!icon.availableSizes().isEmpty()) return icon;
    }
    return QIcon(QStringLiteral(":/stud-logo-color.png"));
}

}  // namespace

std::optional<client::Product> choose_product() {
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Stud"));
    auto* layout = new QVBoxLayout(&dialog);

    auto* question = new QLabel(QStringLiteral("What do you want to open?"), &dialog);
    QFont font = question->font();
    font.setPointSizeF(font.pointSizeF() * 1.4);
    question->setFont(font);
    question->setAlignment(Qt::AlignCenter);
    layout->addWidget(question);

    std::optional<client::Product> chosen;
    auto* buttons = new QHBoxLayout();
    for (const client::Product product : {client::Product::Studio, client::Product::Player}) {
        auto* button = new QToolButton(&dialog);
        button->setText(client::display_name(product));
        button->setIcon(icon_of(product));
        // As large as the question is wide, in the style's own icon steps.
        const int side = question->sizeHint().width() / 2;
        button->setIconSize(QSize(side, side));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setAutoRaise(true);
        QObject::connect(button, &QToolButton::clicked, &dialog, [&chosen, &dialog, product] {
            chosen = product;
            dialog.accept();
        });
        buttons->addWidget(button);
    }
    layout->addLayout(buttons);
    dialog.setFixedSize(dialog.sizeHint());
    dialog.exec();
    return chosen;
}

}  // namespace stud::ui
