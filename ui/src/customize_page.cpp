#include "customize_page.h"

#include "customization.h"
#include "mono_icon.h"
#include "open_location.h"

#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace stud::ui {

namespace {

using customization::Part;

struct Spec {
    Part part;
    const char* name;
    const char* filter;  // nullptr when there is no file to choose
    bool colour;
    // Bloxstrap's classic versions: "" for one, or each variant of it.
    QStringList classic;
    // For parts without a picture of their own.
    const char* icon;
    QStyle::StandardPixmap fallback;
};

struct Group {
    const char* title;
    std::vector<Spec> parts;
};

const std::vector<Group>& groups() {
    static const std::vector<Group> all = {
        {"Pointer",
         {{Part::Cursor, "Cursor", "Images (*.png)", true, {"2006", "2013"}, nullptr, QStyle::SP_ArrowUp},
          {Part::ShiftLock, "Shift lock", "Images (*.png)", true, {}, nullptr, QStyle::SP_ArrowUp}}},
        {"Interface",
         {{Part::EmoteWheel, "Emote wheel", nullptr, true, {}, nullptr, QStyle::SP_DesktopIcon},
          {Part::Font, "Game font", "Fonts (*.ttf *.otf)", false, {}, nullptr, QStyle::SP_FileIcon},
          {Part::MonoFont, "Code font", "Fonts (*.ttf *.otf)", false, {}, nullptr, QStyle::SP_FileIcon}}},
        {"Sounds",
         {{Part::DeathSound, "Death sound", "Sounds (*.ogg)", false, {}, "audio-x-generic", QStyle::SP_MediaVolume},
          {Part::CharacterSounds, "Character sounds", nullptr, false, {""}, "audio-x-generic",
           QStyle::SP_MediaVolume}}},
        {"Avatar",
         {{Part::AvatarBackground, "Avatar background", nullptr, false, {""}, "preferences-desktop-wallpaper",
           QStyle::SP_DesktopIcon}}},
    };
    return all;
}

// Classic cursors fetched for their menu, kept only while Stud runs.
QHash<QString, QIcon>& classic_previews() {
    static QHash<QString, QIcon> previews;
    return previews;
}

QString classic_label(const Spec& spec, const QString& variant) {
    const QString name = QString::fromLatin1(spec.name).toLower();
    if (!variant.isEmpty()) return QStringLiteral("Bloxstrap's %1 %2").arg(variant, name);
    return QStringLiteral("Bloxstrap's classic %1").arg(name);
}

// Only the drawn part: Roblox's cursors sit in a mostly empty square, so scaling the
// whole square to a preview leaves the arrow a few pixels tall.
QImage visible_part(const QImage& image) {
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    QRect drawn;
    for (int y = 0; y < pixels.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
        for (int x = 0; x < pixels.width(); ++x) {
            if (qAlpha(line[x]) != 0) drawn |= QRect(x, y, 1, 1);
        }
    }
    return drawn.isEmpty() ? image : image.copy(drawn);
}

// One part: its preview, what it has now, and what can change it.
class Card : public QWidget {
public:
    Card(const Spec& spec, QWidget* parent) : QWidget(parent), spec_(spec) {
        const int side = style()->pixelMetric(QStyle::PM_LargeIconSize);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(0, 0, 0, 0);

        preview_ = new QLabel(this);
        preview_->setFixedSize(side, side);
        preview_->setAlignment(Qt::AlignCenter);
        // Cursors and the emote highlight are white and black: a mid tone shows both.
        preview_->setAutoFillBackground(true);
        QPalette backdrop = preview_->palette();
        backdrop.setColor(QPalette::Window, backdrop.color(QPalette::Mid));
        preview_->setPalette(backdrop);
        row->addWidget(preview_);

        auto* text = new QVBoxLayout();
        text->setSpacing(0);
        auto* name = new QLabel(QString::fromLatin1(spec.name), this);
        QFont bold = name->font();
        bold.setBold(true);
        name->setFont(bold);
        text->addWidget(name);
        auto* applied = new QHBoxLayout();
        swatch_ = new QLabel(this);
        applied->addWidget(swatch_);
        state_ = new QLabel(this);
        QPalette muted = state_->palette();
        muted.setColor(QPalette::WindowText, muted.color(QPalette::PlaceholderText));
        state_->setPalette(muted);
        applied->addWidget(state_, 1);
        text->addLayout(applied);
        error_ = new QLabel(this);
        error_->setWordWrap(true);
        error_->setStyleSheet(QStringLiteral("color:#d62f2f;"));
        error_->hide();
        text->addWidget(error_);
        row->addLayout(text, 1);

        auto* change = new QToolButton(this);
        change->setText(QStringLiteral("Change"));
        change->setPopupMode(QToolButton::InstantPopup);
        change->setToolButtonStyle(Qt::ToolButtonTextOnly);
        change->setMenu(menu());
        row->addWidget(change);

        reset_ = new QToolButton(this);
        reset_->setIcon(mono_icon("edit-undo", QStyle::SP_DialogResetButton));
        reset_->setToolTip(QStringLiteral("Back to Roblox's default"));
        connect(reset_, &QToolButton::clicked, this, [this] {
            customization::remove(spec_.part);
            finished(QString());
        });
        row->addWidget(reset_);
        refresh();
    }

    void refresh() {
        const customization::Applied now = customization::applied(spec_.part);
        if (!busy_) state_->setText(now.text);
        swatch_->setVisible(now.colour.isValid());
        if (now.colour.isValid()) {
            const int size = state_->fontMetrics().height();
            QPixmap colour(size, size);
            colour.fill(now.colour);
            swatch_->setPixmap(colour);
        }
        reset_->setEnabled(customization::present(spec_.part));
        draw_preview();
    }

private:
    QMenu* menu() {
        auto* menu = new QMenu(this);
        if (spec_.filter != nullptr) {
            menu->addAction(mono_icon("document-open", QStyle::SP_DialogOpenButton), QStringLiteral("Choose file..."),
                            this, [this] {
                                const QString file = QFileDialog::getOpenFileName(
                                    this, QString::fromLatin1(spec_.name), QString(), QString::fromLatin1(spec_.filter));
                                if (file.isEmpty()) return;
                                QString error;
                                finished(customization::use_file(spec_.part, file, &error) ? QString() : error);
                            });
        }
        if (spec_.colour) {
            menu->addAction(mono_icon("color-picker", QStyle::SP_DesktopIcon), QStringLiteral("Colour..."), this, [this] {
                const QColor last = customization::applied(spec_.part).colour;
                const QColor picked =
                    QColorDialog::getColor(last.isValid() ? last : QColor(Qt::white), this, QString::fromLatin1(spec_.name));
                if (!picked.isValid()) return;
                QString error;
                finished(customization::recolour(spec_.part, picked, &error) ? QString() : error);
            });
        }
        if (!spec_.classic.isEmpty()) {
            if (!menu->isEmpty()) menu->addSeparator();
            for (const QString& variant : spec_.classic) {
                QAction* action = menu->addAction(classic_label(spec_, variant), this, [this, variant] { download(variant); });
                action->setToolTip(QStringLiteral("Downloads it from Bloxstrap's GitHub repository."));
                if (spec_.part == Part::Cursor) classic_actions_.insert(variant, action);
            }
            menu->setToolTipsVisible(true);
            connect(menu, &QMenu::aboutToShow, this, [this] { show_classic_previews(); });
        }
        return menu;
    }

    // The classic cursors only exist on Bloxstrap's side until one is chosen.
    void show_classic_previews() {
        for (auto it = classic_actions_.cbegin(); it != classic_actions_.cend(); ++it) {
            const QString variant = it.key();
            QPointer<QAction> action = it.value();
            if (classic_previews().contains(variant)) {
                action->setIcon(classic_previews().value(variant));
                continue;
            }
            customization::fetch_classic_cursor(variant, [variant, action](const QImage& image) {
                if (image.isNull()) return;
                const QIcon icon(QPixmap::fromImage(visible_part(image)));
                classic_previews().insert(variant, icon);
                if (action) action->setIcon(icon);
            });
        }
    }

    void download(const QString& variant) {
        busy_ = true;
        state_->setText(QStringLiteral("Downloading from Bloxstrap..."));
        QPointer<Card> self = this;
        customization::download_classic(spec_.part, variant, [self](const QString& error) {
            if (!self) return;
            self->busy_ = false;
            self->finished(error);
        });
    }

    void finished(const QString& error) {
        error_->setText(error);
        error_->setVisible(!error.isEmpty());
        refresh();
    }

    void draw_preview() {
        const QSize box = preview_->size();
        if (spec_.part == Part::Font || spec_.part == Part::MonoFont) {
            if (font_id_ >= 0) QFontDatabase::removeApplicationFont(font_id_);
            font_id_ = -1;
            QFont sample = spec_.part == Part::MonoFont ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : font();
            const QString file = customization::font_file(spec_.part);
            if (!file.isEmpty()) {
                font_id_ = QFontDatabase::addApplicationFont(file);
                const QStringList families = QFontDatabase::applicationFontFamilies(font_id_);
                if (!families.isEmpty()) sample.setFamily(families.first());
            }
            sample.setPixelSize(box.height());
            sample.setPixelSize(box.height() * box.height() / QFontMetrics(sample).height());
            preview_->setFont(sample);
            preview_->setText(QStringLiteral("Aa"));
            return;
        }
        const QImage picture = customization::image(spec_.part);
        const QPixmap pixmap = picture.isNull()
                                   ? mono_icon(spec_.icon != nullptr ? spec_.icon : "", spec_.fallback).pixmap(box)
                                   : QPixmap::fromImage(visible_part(picture))
                                         .scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        preview_->setPixmap(pixmap);
    }

    Spec spec_;
    QLabel* preview_;
    QLabel* swatch_;
    QLabel* state_;
    QLabel* error_;
    QToolButton* reset_;
    QHash<QString, QAction*> classic_actions_;
    int font_id_ = -1;
    bool busy_ = false;
};

}  // namespace

CustomizePage::CustomizePage(std::function<void()> import, QWidget* parent) : QWidget(parent) {
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    auto* note = new QLabel(
        QStringLiteral("These change Roblox Player, and take effect the next time it starts."), this);
    note->setWordWrap(true);
    page->addWidget(note);

    for (const Group& group : groups()) {
        auto* box = new QGroupBox(QString::fromLatin1(group.title), this);
        auto* cards = new QVBoxLayout(box);
        for (const Spec& spec : group.parts) {
            auto* card = new Card(spec, box);
            cards->addWidget(card);
            refreshers_.push_back([card] { card->refresh(); });
        }
        page->addWidget(box);
    }

    auto* actions = new QHBoxLayout();
    auto* importOther = new QPushButton(QStringLiteral("Import from Bloxstrap, Fishstrap or Froststrap..."), this);
    importOther->setIcon(mono_icon("document-import", QStyle::SP_DialogOpenButton));
    importOther->setToolTip(QStringLiteral("Choose that bootstrapper's folder: its customizations are copied in, and "
                                           "its FastFlags added to the FFlags tab."));
    connect(importOther, &QPushButton::clicked, this, [this, import] {
        import();
        refresh();
    });
    actions->addWidget(importOther);

    // Mirrors Roblox's content tree, so Bloxstrap and Froststrap folders drop in.
    auto* folders = new QToolButton(this);
    folders->setText(QStringLiteral("Open folder"));
    folders->setIcon(mono_icon("folder-open", QStyle::SP_DirOpenIcon));
    folders->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    folders->setPopupMode(QToolButton::InstantPopup);
    auto* folderMenu = new QMenu(folders);
    for (const auto product : {client::Product::Player, client::Product::Studio}) {
        folderMenu->addAction(product == client::Product::Studio ? QStringLiteral("Studio customizations")
                                                                 : QStringLiteral("Player customizations"),
                              this, [product] {
                                  QDir().mkpath(customization::folder(product));
                                  open_location(QUrl::fromLocalFile(customization::folder(product)));
                              });
    }
    folders->setMenu(folderMenu);
    actions->addWidget(folders);
    actions->addStretch();
    page->addLayout(actions);
}

void CustomizePage::showEvent(QShowEvent* event) {
    refresh();
    QWidget::showEvent(event);
}

void CustomizePage::refresh() {
    for (const auto& refresher : refreshers_) refresher();
}

}  // namespace stud::ui
