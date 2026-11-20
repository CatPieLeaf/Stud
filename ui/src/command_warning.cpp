#include "command_warning.h"

#include <QClipboard>
#include <QFontDatabase>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace stud::ui {

QFrame* command_warning(QWidget* parent, const QString& text, const QString& command) {
    auto* box = new QFrame(parent);
    box->setObjectName(QStringLiteral("commandWarning"));
    box->setStyleSheet(QStringLiteral("#commandWarning { background: rgba(255, 193, 7, 0.15);"
                                      " border: 1px solid #ffc107; border-radius: 6px; }"));
    auto* row = new QHBoxLayout(box);
    auto* icon = new QLabel(box);
    icon->setPixmap(box->style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(32, 32));
    row->addWidget(icon, 0, Qt::AlignTop);
    auto* column = new QVBoxLayout();
    auto* label = new QLabel(text, box);
    label->setWordWrap(true);
    column->addWidget(label);
    auto* line = new QLabel(command, box);
    line->setWordWrap(true);
    line->setTextInteractionFlags(Qt::TextSelectableByMouse);
    line->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    column->addWidget(line);
    auto* copy = new QPushButton(QStringLiteral("Copy command"), box);
    QObject::connect(copy, &QPushButton::clicked, box,
                     [command] { QGuiApplication::clipboard()->setText(command); });
    column->addWidget(copy, 0, Qt::AlignRight);
    row->addLayout(column, 1);
    return box;
}

}  // namespace stud::ui
