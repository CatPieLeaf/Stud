#include "customize_page.h"

#include "customization.h"
#include "mono_icon.h"
#include "open_location.h"

#include <QCheckBox>
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
    // Whether other bootstrappers offer looks for it.
    bool offers;
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
         {{Part::Cursor, "Cursor", "Images (*.png)", true, true, nullptr, QStyle::SP_ArrowUp},
          {Part::ShiftLock, "Shift lock", "Images (*.png)", true, false, nullptr, QStyle::SP_ArrowUp}}},
        {"Interface",
         {{Part::EmoteWheel, "Emote wheel", nullptr, true, false, nullptr, QStyle::SP_DesktopIcon},
          {Part::Font, "Game font", "Fonts (*.ttf *.otf)", false, false, nullptr, QStyle::SP_FileIcon},
          {Part::MonoFont, "Code font", "Fonts (*.ttf *.otf)", false, false, nullptr, QStyle::SP_FileIcon},
          {Part::Emoji, "Emoji", "Fonts (*.ttf)", false, true, nullptr, QStyle::SP_FileIcon}}},
        {"Sounds",
         {{Part::DeathSound, "Death sound", "Sounds (*.ogg)", false, false, "audio-x-generic", QStyle::SP_MediaVolume},
          {Part::CharacterSounds, "Character sounds", nullptr, false, true, "audio-x-generic", QStyle::SP_MediaVolume}}},
        {"Avatar",
         {{Part::AvatarBackground, "Avatar background", nullptr, false, true, "preferences-desktop-wallpaper",
           QStyle::SP_DesktopIcon}}},
    };
    return all;
}

// Offer pictures fetched for the menus, kept only while Stud runs.
QHash<QUrl, QIcon>& offer_previews() {
    static QHash<QUrl, QIcon> previews;
    return previews;
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

// A line of an error, in the card or group it belongs to.
QLabel* error_label(QWidget* parent) {
    auto* error = new QLabel(parent);
    error->setWordWrap(true);
    error->setStyleSheet(QStringLiteral("color:#d62f2f;"));
    error->hide();
    return error;
}

void show_error(QLabel* label, const QString& error) {
    label->setText(error);
    label->setVisible(!error.isEmpty());
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
        error_ = error_label(this);
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
        menu->setToolTipsVisible(true);
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
        if (spec_.offers) {
            looking_ = menu->addAction(QStringLiteral("Looking for more in other bootstrappers..."));
            looking_->setEnabled(false);
            connect(menu, &QMenu::aboutToShow, this, [this, menu] { load_offers(menu); });
        }
        return menu;
    }

    // What the other bootstrappers have, the first time the menu opens.
    void load_offers(QMenu* menu) {
        if (offers_asked_) return;
        offers_asked_ = true;
        QPointer<Card> self = this;
        QPointer<QMenu> guard = menu;
        customization::fetch_offers(spec_.part, [self, guard](const QList<customization::Offer>& offers,
                                                              const QString& error) {
            if (!self || !guard) return;
            if (offers.isEmpty()) {
                self->looking_->setText(error.isEmpty() ? QStringLiteral("Nothing more from other bootstrappers")
                                                        : QStringLiteral("Couldn't reach them: %1").arg(error));
                // Asked again at the next opening, in case the network is back.
                self->offers_asked_ = false;
                return;
            }
            guard->removeAction(self->looking_);
            QString section;
            for (const customization::Offer& offer : offers) {
                if (offer.source != section) {
                    section = offer.source;
                    guard->addSection(QStringLiteral("From %1").arg(section));
                }
                QAction* action = guard->addAction(offer.name, self, [self, offer] { self->install(offer); });
                if (offer.preview.isValid()) self->show_preview(action, offer.preview);
            }
        });
    }

    void show_preview(QAction* action, const QUrl& url) {
        if (offer_previews().contains(url)) {
            action->setIcon(offer_previews().value(url));
            return;
        }
        QPointer<QAction> guard = action;
        customization::fetch_image(url, [url, guard](const QImage& image) {
            if (image.isNull()) return;
            const QIcon icon(QPixmap::fromImage(visible_part(image)));
            offer_previews().insert(url, icon);
            if (guard) guard->setIcon(icon);
        });
    }

    void install(const customization::Offer& offer) {
        busy_ = true;
        state_->setText(QStringLiteral("Downloading from %1...").arg(offer.source));
        QPointer<Card> self = this;
        customization::install(spec_.part, offer, [self](const QString& error) {
            if (!self) return;
            self->busy_ = false;
            self->finished(error);
        });
    }

    void finished(const QString& error) {
        show_error(error_, error);
        refresh();
    }

    void draw_preview() {
        const QSize box = preview_->size();
        if (spec_.part == Part::Font || spec_.part == Part::MonoFont || spec_.part == Part::Emoji) {
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
            preview_->setText(spec_.part == Part::Emoji ? QStringLiteral("😀") : QStringLiteral("Aa"));
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
    QAction* looking_ = nullptr;
    bool offers_asked_ = false;
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

    auto* experiments = new QGroupBox(QStringLiteral("Experiments"), this);
    auto* tries = new QVBoxLayout(experiments);
    auto* oldLogin = new QCheckBox(QStringLiteral("Old login screen, from Froststrap"), experiments);
    oldLogin->setToolTip(QStringLiteral("The login background and logo Roblox had before."));
    tries->addWidget(oldLogin);
    QLabel* loginError = error_label(experiments);
    tries->addWidget(loginError);
    connect(oldLogin, &QCheckBox::toggled, this, [oldLogin, loginError](bool on) {
        oldLogin->setEnabled(false);
        QPointer<QCheckBox> guard = oldLogin;
        customization::set_old_login_screen(on, [guard, loginError](const QString& error) {
            if (!guard) return;
            guard->setEnabled(true);
            show_error(loginError, error);
            const QSignalBlocker quiet(guard);
            guard->setChecked(customization::old_login_screen());
        });
    });
    refreshers_.push_back([oldLogin] {
        if (!oldLogin->isEnabled()) return;
        const QSignalBlocker quiet(oldLogin);
        oldLogin->setChecked(customization::old_login_screen());
    });
    page->addWidget(experiments);

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
