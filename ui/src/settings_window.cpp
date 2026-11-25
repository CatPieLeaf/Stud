#include "project_links.h"
#include "mono_icon.h"
#include "settings_window.h"
#include "translations.h"

#include "update_check.h"

#include "cube_view.h"
#include "gpu_enum.h"
#include "customization.h"
#include "customize_page.h"
#include "fflags.h"
#include "desktop_entry.h"
#include "history.h"
#include "launcher_portal.h"
#include "log_export.h"
#include "diagnose.h"
#include "mac_client.h"
#include "roblox_settings.h"
#include "stud1_cleanup.h"
#include "notifications.h"
#include "flatpak_permissions.h"
#include "command_warning.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

#include <QStyle>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include "open_location.h"
#include <QGuiApplication>
#include <QUrl>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QLabel>
#include <QMessageBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QInputDialog>
#include <limits>
#include <QStandardPaths>
#include <QProcess>
#include <QJsonParseError>
#include <QDirIterator>
#include <QLocale>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <functional>
#include <memory>
#include <thread>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTreeWidget>
#include <QHeaderView>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSaveFile>
#include <QCloseEvent>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QPixmap>
#include <QEvent>
#include <QPainter>

#include <cstdio>

#include <unistd.h>

// Defined in main.cpp.
namespace stud::ui {
void terminate_stud_session();
bool stud_session_is_running();
void relaunch_stud();
}  // namespace stud::ui

namespace stud::ui {

namespace {
void select_data(QComboBox* combo, const std::string& value) {
    const int index = combo->findData(QString::fromStdString(value));
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

std::string selected_data(const QComboBox* combo) {
    return combo->currentData().toString().toStdString();
}

// A yellow warning with the `flatpak override` command that grants `missing`, shown
// while `toggle` is on. Null outside the Flatpak or with nothing missing; overrides
// apply on the next start, so this is read once.
QFrame* permission_box(QWidget* parent, QCheckBox* toggle, const QString& what,
                       QStringList (*missing_of)(const QString&)) {
    QFile info(QStringLiteral("/.flatpak-info"));
    if (!info.open(QIODevice::ReadOnly)) return nullptr;
    const QStringList missing = missing_of(QString::fromUtf8(info.readAll()));
    if (missing.isEmpty()) return nullptr;
    QFrame* box = command_warning(
        parent,
        QCoreApplication::translate("stud::ui::SettingsWindow",
                                    "%1 need a permission the Flatpak doesn't have. Run this in a terminal, "
                                    "then restart Stud:")
            .arg(what),
        flatpak_override_command(missing));
    box->setVisible(toggle->isChecked());
    QObject::connect(toggle, &QCheckBox::toggled, box, &QWidget::setVisible);
    return box;
}
}  // namespace

SettingsWindow::SettingsWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle(tr("Stud Settings"));

    auto* layout = new QVBoxLayout(this);
    auto* body = new QHBoxLayout();
    layout->addLayout(body, 1);
    auto* categories = new QListWidget(this);
    categories->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
    auto* pages = new QStackedWidget(this);
    body->addWidget(categories);
    body->addWidget(pages, 1);
    connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

    // Pages scroll instead of stretching the window.
    const auto scroll_page = [](QWidget*& area) {
        auto* scroll = new QScrollArea();
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* page = new QWidget();
        auto* page_layout = new QVBoxLayout(page);
        scroll->setWidget(page);
        area = scroll;
        return page_layout;
    };
    auto add_tab = [&](const QString& title) {
        QWidget* area = nullptr;
        QVBoxLayout* page = scroll_page(area);
        categories->addItem(title);
        pages->addWidget(area);
        return page;
    };
    auto add_parts = [&](const QString& title, const QStringList& parts) {
        auto* tabs = new QTabWidget();
        std::vector<QVBoxLayout*> layouts;
        for (const QString& part : parts) {
            QWidget* area = nullptr;
            layouts.push_back(scroll_page(area));
            tabs->addTab(area, part);
        }
        categories->addItem(title);
        pages->addWidget(tabs);
        return layouts;
    };

    // ---- General -------------------------------------------------------
    auto* general = add_tab(tr("General"));
    general->addWidget(new QLabel(tr("Language"), this));
    languageCombo_ = new QComboBox(this);
    languageCombo_->addItem(tr("System language"), QStringLiteral("system"));
    languageCombo_->addItem(QStringLiteral("English"), QStringLiteral("en"));
    for (const QString& code : translation::languages()) {
        languageCombo_->addItem(translation::language_name(code), code);
    }
    general->addWidget(languageCombo_);
    trayCheck_ = new QCheckBox(tr("Show in system tray"), this);
    general->addWidget(trayCheck_);
    playerDirectlyCheck_ = new QCheckBox(tr("Open Roblox Player directly, without the welcome page"), this);
    general->addWidget(playerDirectlyCheck_);
    gamemodeCheck_ = new QCheckBox(tr("GameMode"), this);
    gamemodeCheck_->setToolTip(tr("Ask Feral GameMode for its performance settings while Roblox runs."));
    general->addWidget(gamemodeCheck_);
    controllersCheck_ = new QCheckBox(tr("Game controllers"), this);
    controllersCheck_->setVisible(stud::config::in_flatpak());
    general->addWidget(controllersCheck_);
    if (QFrame* box =
            permission_box(this, controllersCheck_, tr("Game controllers"), missing_controller_permissions)) {
        general->addWidget(box);
    }
    auto* cleanerRow = new QHBoxLayout();
    cleanerRow->addWidget(new QLabel(tr("Delete logs and caches older than"), this));
    cleanerSpin_ = new QSpinBox(this);
    cleanerSpin_->setRange(0, std::numeric_limits<int>::max());
    cleanerSpin_->setSpecialValueText(tr("Never"));
    cleanerSpin_->setSuffix(tr(" days"));
    cleanerRow->addWidget(cleanerSpin_);
    cleanerRow->addStretch();
    general->addLayout(cleanerRow);

    closeOnLeaveCheck_ = new QCheckBox(tr("Close Roblox when leaving a game"), this);
    general->addWidget(closeOnLeaveCheck_);
    serverRegionCheck_ = new QCheckBox(tr("Notify the server region on join"), this);
    serverRegionCheck_->setToolTip(tr("Names the country of the game server each time you join one."));
    general->addWidget(serverRegionCheck_);

    general->addWidget(new QLabel(tr("MacOS title bar")));
    titleBarCombo_ = new QComboBox(this);
    titleBarCombo_->addItem(tr("MacOS title bar only"), QStringLiteral("macos-no-menu"));
    titleBarCombo_->addItem(tr("MacOS title bar and menu bar"), QStringLiteral("macos"));
    titleBarCombo_->addItem(tr("Linux native title bar"), QStringLiteral("native"));
    general->addWidget(titleBarCombo_);

    general->addWidget(new QLabel(tr("Notifications")));
    notificationsCombo_ = new QComboBox(this);
    notificationsCombo_->addItem(tr("In Roblox when it is fullscreen"), QStringLiteral("fullscreen"));
    notificationsCombo_->addItem(tr("Always in Roblox"), QStringLiteral("always"));
    notificationsCombo_->addItem(tr("System notifications"), QStringLiteral("system"));
    notificationsCombo_->setToolTip(
        tr("Where Stud's notifications show while Roblox or Studio is open. The desktop hides its own "
           "over a fullscreen game, so Stud can show a Mac-style one in Roblox instead."));
    general->addWidget(notificationsCombo_);

    // Only offered on Wayland desktops that also run XWayland.
    auto* displayBackendLabel = new QLabel(tr("Display server"), this);
    general->addWidget(displayBackendLabel);
    displayBackendCombo_ = new QComboBox(this);
    displayBackendCombo_->addItem(tr("Wayland"), QStringLiteral("wayland"));
    displayBackendCombo_->addItem(tr("X11 (XWayland)"), QStringLiteral("x11"));
    general->addWidget(displayBackendCombo_);
    const bool backend_choice = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
                                !qEnvironmentVariableIsEmpty("DISPLAY");
    displayBackendLabel->setVisible(backend_choice);
    displayBackendCombo_->setVisible(backend_choice);

    general->addWidget(new QLabel(tr("Launch Roblox through"), this));
    wrapperEdit_ = new QLineEdit(this);
    wrapperEdit_->setPlaceholderText(tr("gamescope -f --"));
    wrapperEdit_->setToolTip(tr("A command Roblox starts through, such as gamescope or prime-run. "
                                "Quotes work as in a shell. Empty for none."));
    general->addWidget(wrapperEdit_);
    general->addWidget(new QLabel(tr("Environment variables"), this));
    environmentEdit_ = new QPlainTextEdit(this);
    environmentEdit_->setPlaceholderText(tr("NAME=value, one a line"));
    environmentEdit_->setToolTip(tr("Set for Roblox when it starts, after Stud's own, so one here wins "
                                    "over Stud's value for the same name. Lines starting with # are "
                                    "skipped."));
    environmentEdit_->setTabChangesFocus(true);
    environmentEdit_->setMaximumHeight(environmentEdit_->fontMetrics().lineSpacing() * 5);
    general->addWidget(environmentEdit_);

    jsdelivrCheck_ = new QCheckBox(tr("Use jsDelivr mirror instead of GitHub"), this);
    jsdelivrCheck_->setToolTip(tr("Update checks and customization downloads go through jsDelivr's mirror of "
                                  "GitHub first, for networks where GitHub is slow or blocked."));
    general->addWidget(jsdelivrCheck_);

    general->addStretch();

    // ---- Graphics ------------------------------------------------------
    auto* graphics = add_tab(tr("Graphics"));
    graphics->addWidget(new QLabel(tr("GPU")));
    gpuCombo_ = new QComboBox(this);
    for (const auto& gpu : enumerate_gpus()) {
        gpuCombo_->addItem(QString::fromStdString(gpu.name), gpu.device_index);
        gpuCombo_->setItemData(gpuCombo_->count() - 1, gpu.vendor_id, Qt::UserRole + 1);
        gpuCombo_->setItemData(gpuCombo_->count() - 1, QString::fromStdString(gpu.uuid),
                               Qt::UserRole + 2);
    }
    if (gpuCombo_->count() == 0) {
        gpuCombo_->addItem(tr("No GPUs found"));
        gpuCombo_->setEnabled(false);
    }
    graphics->addWidget(gpuCombo_);

    graphics->addWidget(new QLabel(tr("Renderer")));
    rendererCombo_ = new QComboBox(this);
    rendererCombo_->addItem(tr("Vulkan"), QStringLiteral("vulkan"));
    rendererCombo_->addItem(tr("OpenGL"), QStringLiteral("opengl"));
    rendererCombo_->setToolTip(tr("Vulkan: Roblox's Metal renderer, run on Vulkan.\n"
                                  "OpenGL: Roblox's OpenGL renderer, for hardware Vulkan does not run on."));
    graphics->addWidget(rendererCombo_);

    graphics->addWidget(new QLabel(tr("Presentation")));
    presentationCombo_ = new QComboBox(this);
    presentationCombo_->addItem(tr("VSync"), QStringLiteral("vsync"));
    presentationCombo_->addItem(tr("Adaptive VSync"), QStringLiteral("adaptive"));
    presentationCombo_->addItem(tr("No VSync, no tearing"), QStringLiteral("mailbox"));
    presentationCombo_->addItem(tr("No VSync"), QStringLiteral("immediate"));
    presentationCombo_->setToolTip(
        tr("VSync: every frame waits for the display's refresh. Smooth, and the frame rate follows the display.\n"
           "Adaptive VSync: waits, unless a frame is already late; then it tears instead of halving the frame rate.\n"
           "No VSync, no tearing: uncapped, and the newest frame is shown at each refresh. On OpenGL, "
           "the same as No VSync.\n"
           "No VSync: uncapped, and frames are shown at once, so they may tear. The lowest latency."));
    graphics->addWidget(presentationCombo_);

    auto* scaleRow = new QHBoxLayout();
    scaleRow->addWidget(new QLabel(tr("Render resolution"), this));
    renderScaleSpin_ = new QSpinBox(this);
    renderScaleSpin_->setRange(1, 100);
    renderScaleSpin_->setSuffix(tr("%"));
    renderScaleSpin_->setToolTip(tr("The game is drawn at this share of the window's size and scaled "
                                    "back up as it is shown. Lower draws fewer pixels: it helps when "
                                    "the GPU is the limit, and softens the picture and the menus with it.\n"
                                    "Vulkan only."));
    scaleRow->addWidget(renderScaleSpin_);
    upscalerCombo_ = new QComboBox(this);
    upscalerCombo_->addItem(tr("RAVU-Zoom AR"), QStringLiteral("ravu-ar"));
    upscalerCombo_->addItem(tr("SGSR1 ED"), QStringLiteral("sgsr-ed"));
    upscalerCombo_->setToolTip(
        tr("RAVU-Zoom AR: the better picture, and about three times the cost. Trained\n"
           "filter weights, clamped so a hard edge cannot grow a bright or dark border.\n"
           "\n"
           "SGSR1 ED: the lightweight one. Reconstructs edges from the pixels alone.\n"
           "The one to pick on a laptop or an integrated GPU."));
    scaleRow->addWidget(upscalerCombo_);
    graphics->addLayout(scaleRow);
    const auto scaling = [this] { upscalerCombo_->setEnabled(renderScaleSpin_->value() < 100); };
    connect(renderScaleSpin_, qOverload<int>(&QSpinBox::valueChanged), this, scaling);

    // Frame rate while Roblox is hidden or unfocused; one step past the top is Unlimited.
    backgroundFpsLabel_ = new QLabel(this);
    graphics->addWidget(backgroundFpsLabel_);
    backgroundFpsSlider_ = new QSlider(Qt::Horizontal, this);
    backgroundFpsSlider_->setRange(1, stud::config::kBackgroundFpsUnlimited + 1);
    backgroundFpsSlider_->setPageStep(10);
    backgroundFpsSlider_->setToolTip(
        tr("Frames per second while Roblox is not the window you are using: minimised,\n"
           "on another workspace, covered, or alt-tabbed away. All the way right is Unlimited."));
    graphics->addWidget(backgroundFpsSlider_);
    connect(backgroundFpsSlider_, &QSlider::valueChanged, this,
            &SettingsWindow::onBackgroundFpsChanged);

    mangohudCheck_ = new QCheckBox(tr("MangoHud overlay"), this);
    mangohudCheck_->setToolTip(tr("Shows MangoHud's performance overlay over Roblox."));
    graphics->addWidget(mangohudCheck_);

    graphics->addStretch();

    // ---- Discord RPC ---------------------------------------------------
    auto* discord = add_tab(tr("Discord RPC"));
    discordCheck_ = new QCheckBox(tr("Discord Rich Presence"), this);
    discordCheck_->setToolTip(tr("Shows the game you are playing on your Discord profile, with what "
                                 "the game itself reports."));
    discord->addWidget(discordCheck_);
    discordJoinCheck_ = new QCheckBox(tr("Show a Join server button"), this);
    discord->addWidget(discordJoinCheck_);
    connect(discordCheck_, &QCheckBox::toggled, discordJoinCheck_, &QWidget::setEnabled);
    if (QFrame* box = permission_box(this, discordCheck_, tr("Discord Rich Presence"), missing_discord_permissions)) {
        discord->addWidget(box);
    }
    discord->addStretch();

    // ---- Customizations ----------------------------------------------------------
    auto* customizeTab = add_tab(tr("Customize"));
    customizeTab->addWidget(new CustomizePage([this] { onImportBootstrapperClicked(); }, this));
    customizeTab->addStretch();

    // ---- FFlags --------------------------------------------------------
    buildRobloxTab(add_tab(tr("Roblox")));
    buildHistoryTab(add_tab(tr("History")));
    {
        const auto parts = add_parts(tr("FFlags"), {tr("Presets"), tr("FFlag editor")});
        buildFlagsTab(parts[0], parts[1]);
    }

    buildStudioTab(add_tab(tr("Studio")));
    buildDebugTab(add_tab(tr("Debug")));

    // ---- About ---------------------------------------------------------
    auto* about = add_tab(tr("About"));
    auto* cube = new CubeView(this);
    cube->setFixedSize(128, 128);
    // Ten clicks on the cube, with an NVIDIA GPU selected.
    cube->on_tenth_click = [this] {
        if (gpuCombo_->currentData(Qt::UserRole + 1).toUInt() != kVendorNvidia) return;
        send_notification(QStringLiteral("Stud"), QString::fromLatin1(kNvidiaNote));
    };
    about->addWidget(cube, 0, Qt::AlignHCenter);
    auto* logo = new QLabel(this);
    logo->setPixmap(QPixmap(":/stud-title.png")
                        .scaled(138, 138, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    about->addWidget(logo);
    auto* aboutText = new QLabel(
        tr("<div style='text-align:center'>"
           "<p><b>Version %1</b></p>"
           "<p>Runs Roblox's real, unmodified macOS client<br>"
           "on Linux.</p>"
           "</div>")
            .arg(QString::fromLatin1(STUD_VERSION)),
        this);
    aboutText->setTextFormat(Qt::RichText);
    aboutText->setAlignment(Qt::AlignCenter);
    about->addWidget(aboutText);
    auto* links = new QHBoxLayout();
    links->addStretch();
    links->addWidget(github_button(this));
    links->addWidget(discord_button(this));
    links->addStretch();
    about->addLayout(links);
    about->addStretch();

    categories->setMaximumWidth(categories->sizeHintForColumn(0) + 2 * categories->frameWidth() +
                                categories->style()->pixelMetric(QStyle::PM_ScrollBarExtent));
    categories->setCurrentRow(0);

    // ---- Save, shared by every tab -------------------------------------
    auto* buttonRow = new QHBoxLayout();
    // Shown only while a session runs, to compare settings quickly.
    restartButton_ = new QPushButton(this);
    paintRestartIcon();
    restartButton_->setToolTip(tr("Save and restart Stud"));
    // Icon only; paintRestartIcon() falls back to a word when there is no glyph.
    if (!restartButton_->icon().isNull()) {
        restartButton_->setFixedWidth(36);
    }
    restartButton_->setVisible(stud_session_is_running());
    buttonRow->addWidget(restartButton_);
    connect(restartButton_, &QPushButton::clicked, this, &SettingsWindow::onRestartClicked);

    auto* saveButton = new QPushButton(tr("Save"), this);
    saveButton->setIcon(mono_icon("document-save", QStyle::SP_DialogSaveButton));
    buttonRow->addWidget(saveButton);
    layout->addLayout(buttonRow);
    connect(saveButton, &QPushButton::clicked, this, &SettingsWindow::onSaveClicked);

    statusLabel_ = new QLabel(this);
    // Also announces updates, so it renders and opens links.
    statusLabel_->setTextFormat(Qt::RichText);
    statusLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    statusLabel_->setOpenExternalLinks(false);
    connect(statusLabel_, &QLabel::linkActivated, this, [](const QString& link) {
        if (link == UpdateCheck::releasesUrl()) {
            UpdateCheck::open();
        } else {
            open_location(QUrl(link));
        }
    });
    layout->addWidget(statusLabel_);

    connect(UpdateCheck::instance(), &UpdateCheck::updateFound, this,
            [this] { showIdleStatus(); });
    UpdateCheck::start();

    loadFromDisk();
    installResets();
    // The tray's check usually finished before this window opened, so the signal
    // has already fired; ask for the remembered answer now.
    showIdleStatus();
}

// Right-click a control to reset it to the default of a fresh StudSettings.
void SettingsWindow::installResets() {
    const stud::config::StudSettings d;
    auto add = [this](QWidget* widget, std::function<void()> reset) {
        if (widget == nullptr) return;
        widget->installEventFilter(this);
        resets_.insert(widget, std::move(reset));
    };

    add(gpuCombo_, [this] {
        // The device Stud would pick on its own, not the first row.
        const int index = gpuCombo_->findData(default_gpu_index());
        gpuCombo_->setCurrentIndex(index >= 0 ? index : 0);
    });
    add(presentationCombo_, [this, d] { select_data(presentationCombo_, d.presentation); });
    add(renderScaleSpin_, [this, d] { renderScaleSpin_->setValue(d.render_scale); });
    add(upscalerCombo_, [this, d] { select_data(upscalerCombo_, d.upscaler); });
    add(rendererCombo_, [this, d] { select_data(rendererCombo_, d.renderer); });
    add(titleBarCombo_, [this, d] { select_data(titleBarCombo_, d.title_bar); });
    add(notificationsCombo_, [this, d] { select_data(notificationsCombo_, d.notifications); });
    add(displayBackendCombo_, [this, d] { select_data(displayBackendCombo_, d.display_backend); });
    add(languageCombo_, [this, d] { select_data(languageCombo_, d.language); });
    add(gamemodeCheck_, [this, d] { gamemodeCheck_->setChecked(d.gamemode); });
    add(mangohudCheck_, [this, d] { mangohudCheck_->setChecked(d.mangohud); });
    add(wrapperEdit_, [this, d] { wrapperEdit_->setText(QString::fromStdString(d.launch_wrapper)); });
    add(environmentEdit_,
        [this, d] { environmentEdit_->setPlainText(QString::fromStdString(d.environment)); });
    add(backgroundFpsSlider_, [this, d] { setBackgroundFps(d.background_fps); });
    add(discordCheck_, [this, d] { discordCheck_->setChecked(d.discord_rich_presence); });
    add(discordJoinCheck_, [this, d] { discordJoinCheck_->setChecked(d.discord_join_button); });
    add(controllersCheck_, [this, d] { controllersCheck_->setChecked(d.game_controllers); });
    add(closeOnLeaveCheck_, [this, d] { closeOnLeaveCheck_->setChecked(d.close_on_leave); });
    add(jsdelivrCheck_, [this, d] { jsdelivrCheck_->setChecked(d.use_jsdelivr); });
    add(serverRegionCheck_,
        [this, d] { serverRegionCheck_->setChecked(d.server_region_notification); });
    add(historyGamesCheck_, [this, d] { historyGamesCheck_->setChecked(d.record_game_history); });
    add(historyServersCheck_, [this, d] { historyServersCheck_->setChecked(d.record_server_history); });
    add(trayCheck_, [this, d] { trayCheck_->setChecked(d.system_tray); });
    add(playerDirectlyCheck_, [this, d] { playerDirectlyCheck_->setChecked(d.open_player_directly); });
    add(cleanerSpin_, [this, d] { cleanerSpin_->setValue(d.cleaner_days); });
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event) {
    // ContextMenu, so keyboard-triggered menus work too.
    if (event->type() == QEvent::ContextMenu) {
        const auto reset = resets_.constFind(watched);
        if (reset != resets_.constEnd()) {
            (*reset)();
            statusLabel_->setText(tr("Reset to default. Save to keep it."));
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

namespace {

QString flags_path() { return QString::fromStdString(stud::config::default_flag_overrides_path()); }

// An empty editor means no flags; nullopt with `error` set when it isn't a JSON object.
std::optional<QJsonObject> parse_flags(const QString& text, QString* error) {
    if (text.trimmed().isEmpty()) return QJsonObject();
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError) {
        *error = QCoreApplication::translate("stud::ui::SettingsWindow", "%1 (at character %2)")
                     .arg(parse.errorString())
                     .arg(parse.offset);
        return std::nullopt;
    }
    if (!doc.isObject()) {
        *error = QCoreApplication::translate("stud::ui::SettingsWindow", "it is not a JSON object");
        return std::nullopt;
    }
    return doc.object();
}

// `imported` over `current`: a flag both have takes the imported value.
QJsonObject merged_flags(QJsonObject current, const QJsonObject& imported) {
    for (auto it = imported.begin(); it != imported.end(); ++it) current.insert(it.key(), it.value());
    return current;
}

QString format_flags(const QJsonObject& flags) {
    return flags.isEmpty() ? QString()
                           : QString::fromUtf8(QJsonDocument(flags).toJson(QJsonDocument::Indented));
}

}  // namespace

// Unlimited is stored as 0, one step past the slider's top.
void SettingsWindow::setBackgroundFps(int fps) {
    const int position =
        fps <= stud::config::kBackgroundFpsNoLimit ? stud::config::kBackgroundFpsUnlimited + 1 : fps;
    backgroundFpsSlider_->setValue(position);
    onBackgroundFpsChanged(position);
}

void SettingsWindow::onBackgroundFpsChanged(int value) {
    backgroundFpsLabel_->setText(value > stud::config::kBackgroundFpsUnlimited
                                     ? tr("Background frame rate: Unlimited")
                                     : tr("Background frame rate: %1 FPS").arg(value));
}

// The JSON editor is the single source: presets rewrite it, and it sets the presets.
void SettingsWindow::buildFlagsTab(QVBoxLayout* presets_tab, QVBoxLayout* tab) {
    flagsEdit_ = new QPlainTextEdit(this);
    flagsEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    flagsEdit_->setPlaceholderText(tr("{\n    \"FIntDebugForceMSAASamples\": \"4\"\n}"));

    auto* presets = new QGridLayout();
    int row = 0;
    const auto edit_flags = [this](const std::function<void(QJsonObject&)>& change) {
        QString error;
        auto flags = parse_flags(flagsEdit_->toPlainText(), &error);
        if (!flags) {
            statusLabel_->setText(tr("Fix the flags first: %1").arg(error).toHtmlEscaped());
            return;
        }
        change(*flags);
        flagsEdit_->setPlainText(format_flags(*flags));
    };
    const auto combo = [&](const char* label, fflags::Preset preset,
                           const QList<QPair<QString, int>>& choices) {
        auto* box = new QComboBox(this);
        for (const auto& [text, value] : choices) box->addItem(text, value);
        presets->addWidget(new QLabel(QString::fromLatin1(label), this), row, 0);
        presets->addWidget(box, row++, 1);
        connect(box, &QComboBox::activated, this, [box, preset, edit_flags] {
            edit_flags([&](QJsonObject& f) { fflags::set(f, preset, box->currentData().toInt()); });
        });
        flagRefresh_.push_back([box, preset](const QJsonObject& f) {
            const int index = box->findData(fflags::get(f, preset));
            box->setCurrentIndex(index >= 0 ? index : 0);
        });
    };
    const auto check_box = [&](const char* label, fflags::Preset preset) {
        auto* box = new QCheckBox(QString::fromLatin1(label), this);
        presets->addWidget(box, row++, 0, 1, 2);
        connect(box, &QCheckBox::clicked, this, [box, preset, edit_flags] {
            edit_flags([&](QJsonObject& f) { fflags::set(f, preset, box->isChecked() ? 1 : 0); });
        });
        flagRefresh_.push_back([box, preset](const QJsonObject& f) {
            box->setChecked(fflags::get(f, preset) == 1);
        });
    };
    combo("Anti-aliasing (MSAA)", fflags::Preset::Msaa,
          {{"Default", -1}, {"1x", 1}, {"2x", 2}, {"4x", 4}});
    combo("Texture quality", fflags::Preset::TextureQuality,
          {{"Default", -1}, {"Lowest", 0}, {"Low", 1}, {"Medium", 2}, {"High", 3}});
    QList<QPair<QString, int>> levels{{"Default", -1}};
    for (int level = 1; level <= 21; ++level) levels.append({QString::number(level), level});
    combo("Graphics quality override", fflags::Preset::QualityLevel, levels);
    QList<QPair<QString, int>> meshes{{"Default", -1}};
    for (int level = 0; level <= 9; ++level) meshes.append({QString::number(level), level});
    combo("Mesh detail", fflags::Preset::MeshDetail, meshes);
    check_box("Disable grass", fflags::Preset::DisableGrass);
    check_box("Gray sky", fflags::Preset::GraySky);
    check_box("Pause voxelizer", fflags::Preset::PauseVoxelizer);
    check_box("Keep rendering quality with display scaling", fflags::Preset::KeepQualityWhenScaled);
    presets_tab->addLayout(presets);
    presets_tab->addStretch();

    auto* profiles = new QHBoxLayout();
    auto* profileBox = new QComboBox(this);
    profileBox->addItems(fflags::profiles());
    profileBox->setMinimumContentsLength(12);
    auto* load = new QPushButton(tr("Load"), this);
    auto* saveAs = new QPushButton(tr("Save as..."), this);
    auto* remove = new QPushButton(tr("Delete"), this);
    load->setIcon(mono_icon("document-open", QStyle::SP_DialogOpenButton));
    saveAs->setIcon(mono_icon("document-save-as", QStyle::SP_DialogSaveButton));
    remove->setIcon(mono_icon("edit-delete", QStyle::SP_TrashIcon));
    profiles->addWidget(new QLabel(tr("Profile"), this));
    profiles->addWidget(profileBox, 1);
    profiles->addWidget(load);
    profiles->addWidget(saveAs);
    profiles->addWidget(remove);
    tab->addLayout(profiles);
    connect(load, &QPushButton::clicked, this, [this, profileBox] {
        if (profileBox->currentText().isEmpty()) return;
        flagsEdit_->setPlainText(format_flags(fflags::load_profile(profileBox->currentText())));
        statusLabel_->setText(tr("Profile loaded. Save to use it."));
    });
    connect(saveAs, &QPushButton::clicked, this, [this, profileBox] {
        QString error;
        const auto flags = parse_flags(flagsEdit_->toPlainText(), &error);
        if (!flags) {
            statusLabel_->setText(tr("Fix the flags first: %1").arg(error).toHtmlEscaped());
            return;
        }
        const QString name = QInputDialog::getText(this, tr("Save FFlag profile"), tr("Name"));
        if (name.trimmed().isEmpty() || !fflags::save_profile(name.trimmed(), *flags)) return;
        profileBox->clear();
        profileBox->addItems(fflags::profiles());
        profileBox->setCurrentText(name.trimmed());
    });
    connect(remove, &QPushButton::clicked, this, [profileBox] {
        if (profileBox->currentText().isEmpty()) return;
        fflags::delete_profile(profileBox->currentText());
        profileBox->removeItem(profileBox->currentIndex());
    });

    tab->addWidget(flagsEdit_, 1);

    flagsWarning_ = new QLabel(this);
    flagsWarning_->setWordWrap(true);
    tab->addWidget(flagsWarning_);
    auto* buttons = new QHBoxLayout();
    auto* importButton = new QPushButton(tr("Import JSON..."), this);
    importButton->setIcon(mono_icon("document-import", QStyle::SP_DialogOpenButton));
    importButton->setToolTip(tr("Add the flags of a JSON file to these; a flag both have takes the "
                                "imported value."));
    connect(importButton, &QPushButton::clicked, this, &SettingsWindow::onImportFlagsClicked);
    auto* clean = new QPushButton(tr("Clean list"), this);
    clean->setIcon(mono_icon("edit-clear", QStyle::SP_DialogDiscardButton));
    clean->setToolTip(tr("Remove the flags Roblox ignores: those not on its allowlist."));
    connect(clean, &QPushButton::clicked, this, [edit_flags] {
        edit_flags([](QJsonObject& f) {
            for (const QString& name : fflags::not_allowlisted(f)) f.remove(name);
        });
    });
    buttons->addWidget(importButton);
    buttons->addWidget(clean);
    buttons->addStretch();
    tab->addLayout(buttons);

    connect(flagsEdit_, &QPlainTextEdit::textChanged, this, [this] {
        QString error;
        const auto flags = parse_flags(flagsEdit_->toPlainText(), &error);
        if (!flags) {
            flagsWarning_->setText(tr("Not valid JSON: %1").arg(error).toHtmlEscaped());
            return;
        }
        for (const auto& refresh : flagRefresh_) refresh(*flags);
        const QStringList ignored = fflags::not_allowlisted(*flags);
        flagsWarning_->setText(ignored.isEmpty()
                                   ? QString()
                                   : tr("Roblox ignores flags not on its allowlist: %1")
                                         .arg(ignored.join(QStringLiteral(", ")))
                                         .toHtmlEscaped());
    });
}

// Roblox's in-game settings, edited in its file in the prefix. Graphics quality
// is deliberately left to Roblox.
void SettingsWindow::buildRobloxTab(QVBoxLayout* tab) {
    enum class Kind { Toggle, Fraction, Number };
    struct Row {
        const char* name;
        const char* label;
        Kind kind;
    };
    static const Row rows[] = {
        {"MasterVolume", "Volume", Kind::Fraction},
        {"VoiceChatVolume", "Voice chat volume", Kind::Fraction},
        {"PartyVoiceVolume", "Party voice volume", Kind::Fraction},
        {"MouseSensitivity", "Mouse sensitivity", Kind::Number},
        {"GamepadCameraSensitivity", "Gamepad camera sensitivity", Kind::Number},
        {"HapticStrength", "Controller vibration", Kind::Fraction},
        {"CameraYInverted", "Invert the camera", Kind::Toggle},
        {"Fullscreen", "Fullscreen", Kind::Toggle},
        {"PerformanceStatsVisible", "Performance stats", Kind::Toggle},
        {"ReducedMotion", "Reduce motion", Kind::Toggle},
        {"PreferredTransparency", "Background transparency", Kind::Fraction},
    };

    auto* note = new QLabel(tr("The settings from Roblox's in-game menu. Close Roblox before changing them, "
                               "or it will overwrite them when it exits."), this);
    note->setWordWrap(true);
    tab->addWidget(note);
    auto* grid = new QGridLayout();
    tab->addLayout(grid);
    struct Control {
        const char* name;
        QWidget* widget;
        std::function<void(const QString&)> set;
        std::function<QString()> get;
    };
    auto controls = std::make_shared<std::vector<Control>>();
    int line = 0;
    for (const Row& row : rows) {
        Control c{row.name, nullptr, {}, {}};
        if (row.kind == Kind::Toggle) {
            auto* box = new QCheckBox(QString::fromLatin1(row.label), this);
            c.widget = box;
            c.set = [box](const QString& v) { box->setChecked(v == QLatin1String("true")); };
            c.get = [box] { return box->isChecked() ? QStringLiteral("true") : QStringLiteral("false"); };
            grid->addWidget(box, line, 0, 1, 2);
        } else if (row.kind == Kind::Fraction) {
            // Stored as 0 to 1, shown as a percentage.
            auto* slider = new QSlider(Qt::Horizontal, this);
            slider->setRange(0, 100);
            auto* value = new QLabel(this);
            connect(slider, &QSlider::valueChanged, value,
                    [value](int v) { value->setText(tr("%1%").arg(v)); });
            c.widget = slider;
            c.set = [slider](const QString& v) { slider->setValue(qRound(v.toDouble() * 100)); };
            c.get = [slider] { return QString::number(slider->value() / 100.0); };
            grid->addWidget(new QLabel(QString::fromLatin1(row.label), this), line, 0);
            grid->addWidget(slider, line, 1);
            grid->addWidget(value, line, 2);
        } else {
            auto* spin = new QDoubleSpinBox(this);
            spin->setRange(0, std::numeric_limits<float>::max());
            spin->setDecimals(2);
            spin->setSingleStep(0.1);
            c.widget = spin;
            c.set = [spin](const QString& v) { spin->setValue(v.toDouble()); };
            c.get = [spin] { return QString::number(spin->value()); };
            grid->addWidget(new QLabel(QString::fromLatin1(row.label), this), line, 0);
            grid->addWidget(spin, line, 1);
        }
        controls->push_back(std::move(c));
        ++line;
    }

    auto* status = new QLabel(this);
    status->setWordWrap(true);
    tab->addWidget(status);
    auto* buttons = new QHBoxLayout();
    auto* save = new QPushButton(tr("Save to Roblox"), this);
    save->setIcon(mono_icon("document-save", QStyle::SP_DialogSaveButton));
    auto* reload = new QPushButton(tr("Reload"), this);
    reload->setIcon(mono_icon("view-refresh", QStyle::SP_BrowserReload));
    buttons->addWidget(save);
    buttons->addWidget(reload);
    buttons->addStretch();
    tab->addLayout(buttons);
    tab->addStretch();

    // Keys missing from the file stay disabled: only what Roblox wrote is changed.
    const auto load = [controls, status, save] {
        const QString path = roblox_settings::player_file();
        QFile in(path);
        const bool found = !path.isEmpty() && in.open(QIODevice::ReadOnly);
        const QString xml = found ? QString::fromUtf8(in.readAll()) : QString();
        for (Control& c : *controls) {
            const auto value = roblox_settings::read(xml, QString::fromLatin1(c.name));
            c.widget->setEnabled(value.has_value());
            if (value) c.set(value->second);
        }
        save->setEnabled(found);
        status->setText(found ? QString() : tr("Roblox has not saved its settings yet: start it once."));
    };
    load();
    connect(reload, &QPushButton::clicked, this, load);
    connect(save, &QPushButton::clicked, this, [controls, status] {
        const QString path = roblox_settings::player_file();
        QFile in(path);
        if (path.isEmpty() || !in.open(QIODevice::ReadOnly)) {
            status->setText(tr("Roblox's settings file is gone."));
            return;
        }
        QString xml = QString::fromUtf8(in.readAll());
        in.close();
        for (const Control& c : *controls) {
            if (c.widget->isEnabled()) xml = roblox_settings::write(xml, QString::fromLatin1(c.name), c.get());
        }
        QSaveFile out(path);
        const bool saved = out.open(QIODevice::WriteOnly) && out.write(xml.toUtf8()) >= 0 && out.commit();
        status->setText(saved ? tr("Saved. Roblox uses them the next time it starts.")
                              : tr("%1 could not be written.").arg(path));
    });
}

void SettingsWindow::buildStudioTab(QVBoxLayout* tab) {
    tab->addWidget(new QLabel(tr("<b>FastFlags</b>"), this));
    auto* note = new QLabel(
        tr("Studio's own, separate from the Player's: a JSON object, written into Studio's "
           "ClientAppSettings.json each time it starts."), this);
    note->setWordWrap(true);
    tab->addWidget(note);
    auto* editor = new QPlainTextEdit(this);
    editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    editor->setPlaceholderText(tr("{\n    \"FFlagExample\": true\n}"));
    const QString path = QString::fromStdString(stud::config::default_studio_flags_path());
    {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) editor->setPlainText(QString::fromUtf8(file.readAll()));
    }
    tab->addWidget(editor, 1);
    auto* row = new QHBoxLayout();
    auto* save = new QPushButton(tr("Save Studio's flags"), this);
    save->setIcon(mono_icon("document-save", QStyle::SP_DialogSaveButton));
    auto* status = new QLabel(this);
    status->setWordWrap(true);
    row->addWidget(save);
    row->addWidget(status, 1);
    tab->addLayout(row);
    connect(save, &QPushButton::clicked, this, [editor, status, path] {
        const QByteArray text = editor->toPlainText().toUtf8();
        QString error;
        if (!parse_flags(QString::fromUtf8(text), &error)) {
            status->setText(tr("Not saved: %1.").arg(error));
            return;
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile out(path);
        if (!out.open(QIODevice::WriteOnly) || out.write(text) < 0 || !out.commit()) {
            status->setText(tr("Not saved: %1 could not be written.").arg(path));
            return;
        }
        status->setText(tr("Saved. Studio uses them the next time it starts."));
    });

    tab->addSpacing(fontMetrics().height());
    tab->addWidget(new QLabel(tr("<b>Folders</b>"), this));
    auto* folders = new QHBoxLayout();
    auto* plugins = new QPushButton(tr("Open the Plugins folder"), this);
    plugins->setIcon(mono_icon("folder-open", QStyle::SP_DirOpenIcon));
    plugins->setToolTip(tr("Where Studio loads local plugins from, such as Rojo's: Documents/Roblox/Plugins "
                           "inside Stud's macOS environment."));
    auto* documents = new QPushButton(tr("Open Documents"), this);
    documents->setIcon(mono_icon("folder-documents", QStyle::SP_DirOpenIcon));
    documents->setToolTip(tr("Studio's Documents folder inside Stud's macOS environment."));
    folders->addWidget(plugins);
    folders->addWidget(documents);
    folders->addStretch();
    tab->addLayout(folders);
    const auto open = [this](const QString& relative) {
        const QString home = client::prefix_home();
        if (home.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Stud"),
                                     tr("Start Roblox or Studio once first: the folder is made then."));
            return;
        }
        const QString dir = home + QLatin1Char('/') + relative;
        QDir().mkpath(dir);
        open_location(QUrl::fromLocalFile(dir));
    };
    connect(plugins, &QPushButton::clicked, this, [open] { open(QStringLiteral("Documents/Roblox/Plugins")); });
    connect(documents, &QPushButton::clicked, this, [open] { open(QStringLiteral("Documents")); });
}

void SettingsWindow::buildDebugTab(QVBoxLayout* tab) {
    auto* logsLabel = new QLabel(tr("<b>Logs</b>"), this);
    tab->addWidget(logsLabel);
    auto* logsNote = new QLabel(tr("For a bug report: every session's log and Roblox's own, or only the "
                                   "newest session's."), this);
    logsNote->setWordWrap(true);
    tab->addWidget(logsNote);
    auto* logs = new QHBoxLayout();
    auto* exportAll = new QPushButton(tr("Export logs"), this);
    exportAll->setIcon(mono_icon("document-export", QStyle::SP_DialogSaveButton));
    exportAll->setToolTip(tr("Every session log, and Roblox's own logs from the same days, as one .tar.gz."));
    auto* exportLatest = new QPushButton(tr("Export the latest log"), this);
    exportLatest->setIcon(mono_icon("text-x-generic", QStyle::SP_FileIcon));
    exportLatest->setToolTip(tr("Only the newest session's log, as a .log file."));
    auto* openLogs = new QPushButton(tr("Open the logs folder"), this);
    openLogs->setIcon(mono_icon("folder-open", QStyle::SP_DirOpenIcon));
    auto* copyInfo = new QPushButton(tr("Copy system info"), this);
    copyInfo->setIcon(mono_icon("edit-copy", QStyle::SP_FileDialogDetailedView));
    copyInfo->setToolTip(tr("Stud's and Rosemary's versions, the system, GPU and settings: the top of every "
                            "session log, ready to paste into a report. Nothing secret is in it."));
    logs->addWidget(exportAll);
    logs->addWidget(exportLatest);
    logs->addStretch();
    tab->addLayout(logs);
    auto* logs2 = new QHBoxLayout();
    logs2->addWidget(openLogs);
    logs2->addWidget(copyInfo);
    logs2->addStretch();
    tab->addLayout(logs2);

    tab->addSpacing(fontMetrics().height());
    tab->addWidget(new QLabel(tr("<b>Storage</b>"), this));
    auto* storageNote = new QLabel(tr("Settings, FastFlags, customizations and logs will not be erased."), this);
    storageNote->setWordWrap(true);
    tab->addWidget(storageNote);
    auto* storage = new QHBoxLayout();
    auto* clearCache = new QPushButton(this);
    clearCache->setIcon(mono_icon("edit-clear", QStyle::SP_DialogResetButton));
    clearCache->setToolTip(tr("The web pages' cache. Nothing in it is needed; it fills up again as pages load."));
    auto* clearData = new QPushButton(this);
    clearData->setIcon(mono_icon("edit-delete", QStyle::SP_TrashIcon));
    clearData->setToolTip(tr("The Roblox client and Studio, the macOS environment, your Roblox login and your "
                             "game history. The next launch downloads and sets up everything again."));
    storage->addWidget(clearCache);
    storage->addWidget(clearData);
    storage->addStretch();
    tab->addLayout(storage);
    auto* repair = new QHBoxLayout();
    auto* repairRoblox = new QPushButton(tr("Repair Roblox"), this);
    repairRoblox->setIcon(mono_icon("tools-wizard", QStyle::SP_BrowserReload));
    repairRoblox->setToolTip(tr("Removes the installed Roblox and Studio, so the next launch downloads them "
                                "fresh. Your Roblox login and the macOS environment stay."));
    auto* resetPrefix = new QPushButton(tr("Reset the macOS environment"), this);
    resetPrefix->setIcon(mono_icon("view-refresh", QStyle::SP_BrowserReload));
    resetPrefix->setToolTip(tr("Removes the macOS environment Roblox runs in, which the next launch sets up "
                               "again. Roblox itself stays; your Roblox login and Roblox's own settings go."));
    auto* openData = new QPushButton(tr("Open the data folder"), this);
    openData->setIcon(mono_icon("folder-open", QStyle::SP_DirOpenIcon));
    repair->addWidget(repairRoblox);
    repair->addWidget(resetPrefix);
    repair->addWidget(openData);
    repair->addStretch();
    tab->addLayout(repair);

    tab->addSpacing(fontMetrics().height());
    tab->addWidget(new QLabel(tr("<b>Settings</b>"), this));
    auto* resetAll = new QPushButton(tr("Reset all settings"), this);
    resetAll->setIcon(mono_icon("edit-undo", QStyle::SP_DialogResetButton));
    resetAll->setToolTip(tr("Every setting here back to its default, as right-clicking each one does. "
                            "Nothing changes until you save."));
    tab->addWidget(resetAll, 0, Qt::AlignLeft);

    auto* status = new QLabel(this);
    status->setWordWrap(true);
    tab->addWidget(status);
    tab->addStretch();

    const QString cache = QString::fromStdString(stud::paths::cache_dir());
    const QString data = QString::fromStdString(stud::paths::data_dir());
    const auto label_sizes = [clearCache, clearData, cache, data] {
        clearCache->setText(tr("Clear Stud's cache"));
        clearData->setText(tr("Clear Stud's data"));
        QPointer<QPushButton> cache_button = clearCache;
        QPointer<QPushButton> data_button = clearData;
        std::thread([cache_button, data_button, cache, data] {
            const qint64 cache_bytes = disk_size(cache);
            const qint64 data_bytes = disk_size(data);
            QMetaObject::invokeMethod(qApp, [cache_button, data_button, cache_bytes, data_bytes] {
                const QLocale here;
                if (cache_button) {
                    cache_button->setText(tr("Clear Stud's cache (%1)").arg(here.formattedDataSize(cache_bytes)));
                }
                if (data_button) {
                    data_button->setText(tr("Clear Stud's data (%1)").arg(here.formattedDataSize(data_bytes)));
                }
            });
        }).detach();
    };
    label_sizes();

    connect(exportAll, &QPushButton::clicked, this, [this, status] {
        const QString saved = export_all_logs(this);
        if (!saved.isEmpty()) status->setText(saved);
    });
    connect(exportLatest, &QPushButton::clicked, this, [this, status] {
        const QString saved = export_latest_log(this);
        if (!saved.isEmpty()) status->setText(saved);
    });
    connect(openLogs, &QPushButton::clicked, this, [] {
        const QString dir = QString::fromStdString(stud::paths::log_dir());
        QDir().mkpath(dir);
        open_location(QUrl::fromLocalFile(dir));
    });
    connect(openData, &QPushButton::clicked, this, [] {
        const QString dir = QString::fromStdString(stud::paths::data_dir());
        QDir().mkpath(dir);
        open_location(QUrl::fromLocalFile(dir));
    });
    connect(copyInfo, &QPushButton::clicked, this, [status] {
        QGuiApplication::clipboard()->setText(QString::fromStdString(diagnostics_text()));
        status->setText(tr("System info copied."));
    });
    connect(resetAll, &QPushButton::clicked, this, [this, status] {
        for (const auto& reset : resets_) reset();
        status->setText(tr("Every setting is back to its default. Save to keep them."));
    });
    // Refused while Roblox runs: its client, prefix and web views are in use.
    const auto refuse_while_running = [this] {
        if (!stud_session_is_running()) return false;
        QMessageBox::information(this, QStringLiteral("Stud"),
                                 tr("Close Roblox first: Stud is using these files while it runs."));
        return true;
    };
    connect(clearCache, &QPushButton::clicked, this, [status, cache, label_sizes, refuse_while_running] {
        if (refuse_while_running()) return;
        const bool cleared = QDir(cache).removeRecursively();
        label_sizes();
        status->setText(cleared ? tr("Stud's cache is cleared.")
                                : tr("Some of Stud's cache could not be removed."));
    });
    connect(clearData, &QPushButton::clicked, this, [this, status, data, label_sizes, refuse_while_running] {
        if (refuse_while_running()) return;
        if (QMessageBox::question(
                this, tr("Clear Stud's data"),
                tr("Remove the Roblox client and Studio, the macOS environment, your Roblox login "
                   "and your game history?\n\nThe next launch downloads and sets up everything "
                   "again, and you sign in to Roblox again. Settings, FastFlags, customizations "
                   "and logs are kept.")) != QMessageBox::Yes) {
            return;
        }
        const bool cleared = QDir(data).removeRecursively();
        label_sizes();
        status->setText(cleared ? tr("Stud's data is cleared.")
                                : tr("Some of Stud's data could not be removed."));
    });
    connect(repairRoblox, &QPushButton::clicked, this, [this, status, label_sizes, refuse_while_running] {
        if (refuse_while_running()) return;
        if (QMessageBox::question(this, tr("Repair Roblox"),
                                  tr("Remove the installed Roblox and Studio? The next launch "
                                     "downloads them again. Your Roblox login stays.")) !=
            QMessageBox::Yes) {
            return;
        }
        bool cleared = true;
        for (const auto product : {client::Product::Player, client::Product::Studio}) {
            cleared = QDir(client::clients_dir(product)).removeRecursively() && cleared;
        }
        label_sizes();
        status->setText(cleared ? tr("Roblox will be downloaded again at the next launch.")
                                : tr("Some of Roblox could not be removed."));
    });
    connect(resetPrefix, &QPushButton::clicked, this, [this, status, label_sizes, refuse_while_running] {
        if (refuse_while_running()) return;
        if (QMessageBox::question(
                this, tr("Reset the macOS environment"),
                tr("Remove the macOS environment Roblox runs in? The next launch sets it up "
                   "again. You sign in to Roblox again, and Roblox's own settings (volume, "
                   "sensitivity and the like) go back to its defaults.")) != QMessageBox::Yes) {
            return;
        }
        const bool cleared = QDir(client::prefix_dir()).removeRecursively();
        label_sizes();
        status->setText(cleared ? tr("The macOS environment will be set up again at the next launch.")
                                : tr("Some of the macOS environment could not be removed."));
    });
}

void SettingsWindow::buildHistoryTab(QVBoxLayout* tab) {
    historyGamesCheck_ = new QCheckBox(tr("Track games"), this);
    historyGamesCheck_->setToolTip(tr("Keeps each game you play here, with the time played."));
    historyServersCheck_ = new QCheckBox(tr("Track servers"), this);
    historyServersCheck_->setToolTip(
        tr("Keeps each server you join in a game, with where it is, so you can rejoin it.\n"
           "Finding where a server is asks ipwho.is about the server's address, never yours."));
    connect(historyGamesCheck_, &QCheckBox::toggled, historyServersCheck_, &QWidget::setEnabled);
    auto* tracking = new QHBoxLayout();
    tracking->addWidget(historyGamesCheck_);
    tracking->addWidget(historyServersCheck_);
    tracking->addStretch();
    tab->addLayout(tracking);

    auto* total = new QLabel(this);
    tab->addWidget(total);
    auto* list = new QTreeWidget(this);
    list->setColumnCount(3);
    list->setHeaderLabels({"Game", "Played", "Last played"});
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tab->addWidget(list, 1);
    const QString when = QStringLiteral("yyyy-MM-dd hh:mm");
    const auto refresh = [list, total, when] {
        list->clear();
        qint64 played = 0;
        for (const history::Entry& e : history::load()) {
            played += e.seconds_played;
            auto* item = new QTreeWidgetItem(list);
            item->setText(0, e.name.isEmpty() ? tr("Place %1").arg(e.place) : e.name);
            item->setText(1, history::duration(e.seconds_played));
            item->setText(2, QDateTime::fromSecsSinceEpoch(e.last_played).toString(when));
            item->setData(0, Qt::UserRole, e.place);
            item->setData(0, Qt::UserRole + 1, e.icon);
            for (const history::Server& s : e.servers) {
                auto* server = new QTreeWidgetItem(item);
                server->setText(0, s.location.isEmpty() ? tr("Server %1").arg(s.job.left(8))
                                                        : s.location);
                server->setToolTip(0, s.job);
                server->setText(1, history::duration(s.seconds));
                server->setText(2, QDateTime::fromSecsSinceEpoch(s.joined).toString(when));
                server->setData(0, Qt::UserRole, e.place);
                server->setData(0, Qt::UserRole + 2, s.job);
            }
        }
        total->setText(tr("Time played in all: %1").arg(history::duration(played)));
    };
    refresh();

    auto* buttons = new QHBoxLayout();
    auto* join = new QPushButton(tr("Join"), this);
    auto* shortcut = new QPushButton(tr("Create shortcut"), this);
    join->setIcon(mono_icon("media-playback-start", QStyle::SP_MediaPlay));
    shortcut->setIcon(mono_icon("list-add", QStyle::SP_FileDialogNewFolder));
    shortcut->setToolTip(tr("Adds the game to the application menu; it opens straight into the game."));
    auto* reload = new QPushButton(tr("Refresh"), this);
    reload->setIcon(mono_icon("view-refresh", QStyle::SP_BrowserReload));
    auto* forget = new QPushButton(tr("Clear history"), this);
    forget->setIcon(mono_icon("edit-clear-history", QStyle::SP_DialogDiscardButton));
    forget->setToolTip(tr("Forgets every game and server here, and the time played."));
    buttons->addWidget(join);
    buttons->addWidget(shortcut);
    buttons->addStretch();
    buttons->addWidget(forget);
    buttons->addWidget(reload);
    tab->addLayout(buttons);
    connect(reload, &QPushButton::clicked, this, refresh);
    connect(forget, &QPushButton::clicked, this, [this, refresh] {
        if (QMessageBox::question(this, tr("Clear history"),
                                  tr("Forget every game and server, and the time played?")) !=
            QMessageBox::Yes) {
            return;
        }
        history::clear();
        refresh();
    });
    const auto update_join = [list, join] {
        const QTreeWidgetItem* item = list->currentItem();
        const bool server = item != nullptr && item->parent() != nullptr;
        join->setText(server ? tr("Rejoin server") : tr("Join"));
    };
    connect(list, &QTreeWidget::currentItemChanged, this, update_join);
    connect(join, &QPushButton::clicked, this, [list] {
        if (list->currentItem() == nullptr) return;
        const QString place = list->currentItem()->data(0, Qt::UserRole).toString();
        const QString job = list->currentItem()->data(0, Qt::UserRole + 2).toString();
        QString link = QStringLiteral("roblox://experiences/start?placeId=%1").arg(place);
        if (!job.isEmpty()) link += QStringLiteral("&gameInstanceId=%1").arg(job);
        QProcess::startDetached(launcher_command(), {link});
    });
    connect(shortcut, &QPushButton::clicked, this, [this, list] {
        if (list->currentItem() == nullptr) return;
        if (list->currentItem()->parent() != nullptr) list->setCurrentItem(list->currentItem()->parent());
        const QString place = list->currentItem()->data(0, Qt::UserRole).toString();
        const QString name = list->currentItem()->text(0);
        const QString icon_url = list->currentItem()->data(0, Qt::UserRole + 1).toString();
        // Flatpak can't write the desktop's menu, so the portal adds the launcher
        // after the user confirms.
        if (stud::config::in_flatpak()) {
            QPointer<QLabel> status = statusLabel_;
            const auto ask = [status, place, name](const QByteArray& image_file) {
                QByteArray png = launcher_portal::icon_png(image_file);
                if (png.isEmpty()) {
                    QFile logo(QStringLiteral(":/stud-logo-color.png"));
                    if (logo.open(QIODevice::ReadOnly)) png = launcher_portal::icon_png(logo.readAll());
                }
                launcher_portal::install(
                    QStringLiteral("game%1").arg(place), name,
                    QStringLiteral("stud \"roblox://experiences/start?placeId=%1\"").arg(place), png,
                    [status](bool added, const QString& what) {
                        if (status.isNull() || (!added && what.isEmpty())) return;
                        status->setText((added ? tr("Added %1 to the application menu.").arg(what)
                                               : tr("Could not add the shortcut: %1").arg(what))
                                            .toHtmlEscaped());
                    });
            };
            if (icon_url.isEmpty()) {
                ask({});
                return;
            }
            // The portal shows the game's icon while asking.
            auto* net = new QNetworkAccessManager(this);
            QNetworkRequest request{QUrl(icon_url)};
            request.setTransferTimeout(10000);
            QNetworkReply* reply = net->get(request);
            connect(reply, &QNetworkReply::finished, this, [reply, net, ask] {
                ask(reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray());
                reply->deleteLater();
                net->deleteLater();
            });
            return;
        }
        const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        const QString icon = QString::fromStdString(stud::paths::data_dir()) +
                             QStringLiteral("/icons/%1.png").arg(place);
        const QString entry = data + QStringLiteral("/applications/stud-game-%1.desktop").arg(place);
        QDir().mkpath(QFileInfo(icon).absolutePath());
        QDir().mkpath(QFileInfo(entry).absolutePath());
        QString clean_name = name;
        clean_name.replace(QLatin1Char('\n'), QLatin1Char(' '));
        QFile file(entry);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            statusLabel_->setText(tr("Could not write %1.").arg(entry).toHtmlEscaped());
            return;
        }
        file.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nComment=Play %1 with Stud\n"
                                  "Exec=%2 \"roblox://experiences/start?placeId=%3\"\nIcon=%4\n"
                                  "Categories=Game;\nTerminal=false\n")
                       .arg(clean_name, desktop_exec_quote(launcher_command()), place, icon)
                       .toUtf8());
        file.close();
        statusLabel_->setText(tr("Added %1 to the application menu.").arg(clean_name).toHtmlEscaped());
        if (!icon_url.isEmpty() && !QFileInfo::exists(icon)) {
            auto* net = new QNetworkAccessManager(this);
            QNetworkReply* reply = net->get(QNetworkRequest(QUrl(icon_url)));
            connect(reply, &QNetworkReply::finished, this, [reply, net, icon] {
                if (reply->error() == QNetworkReply::NoError) {
                    QFile out(icon);
                    if (out.open(QIODevice::WriteOnly)) out.write(reply->readAll());
                }
                reply->deleteLater();
                net->deleteLater();
            });
        }
    });
}

// Imports another bootstrapper's Modifications and FastFlags.
void SettingsWindow::onImportBootstrapperClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Choose the Bloxstrap, Fishstrap or Froststrap folder"));
    if (dir.isEmpty()) return;
    const QString source = dir + QStringLiteral("/Modifications");
    if (!QDir(source).exists()) {
        statusLabel_->setText(tr("That folder has no Modifications folder."));
        return;
    }
    int copied = 0;
    const QString target = customization::folder(client::Product::Player);
    QDirIterator it(source, QDir::Files | QDir::NoSymLinks | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        const QString relative = file.mid(source.size() + 1);
        if (relative.startsWith(QLatin1String("ClientSettings/"))) continue;
        const QString to = target + QLatin1Char('/') + relative;
        QDir().mkpath(QFileInfo(to).absolutePath());
        QFile::remove(to);
        if (QFile::copy(file, to)) ++copied;
    }
    QFile flags_file(source + QStringLiteral("/ClientSettings/ClientAppSettings.json"));
    int flags = 0;
    if (flags_file.open(QIODevice::ReadOnly)) {
        QString error;
        const auto imported = parse_flags(QString::fromUtf8(flags_file.readAll()), &error);
        auto current = parse_flags(flagsEdit_->toPlainText(), &error);
        if (imported && current) {
            flagsEdit_->setPlainText(format_flags(merged_flags(*current, *imported)));
            flags = int(imported->size());
        }
    }
    statusLabel_->setText(tr("Imported %1 customization file(s) and %2 flag(s). Save to keep the flags.")
                              .arg(copied)
                              .arg(flags));
}

void SettingsWindow::onImportFlagsClicked() {
    const QString path = QFileDialog::getOpenFileName(this, tr("Import FFlags"), QString(),
                                                      tr("JSON files (*.json);;All files (*)"));
    if (path.isEmpty()) return;
    QFile file(path);
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        statusLabel_->setText(tr("Could not read %1.").arg(path).toHtmlEscaped());
        return;
    }
    const auto imported = parse_flags(QString::fromUtf8(file.readAll()), &error);
    if (!imported) {
        statusLabel_->setText(tr("%1 is not usable: %2").arg(path, error).toHtmlEscaped());
        return;
    }
    const auto current = parse_flags(flagsEdit_->toPlainText(), &error);
    if (!current) {
        statusLabel_->setText(
            tr("Fix the flags above before importing: %1").arg(error).toHtmlEscaped());
        return;
    }
    flagsEdit_->setPlainText(format_flags(merged_flags(*current, *imported)));
    statusLabel_->setText(tr("Imported %1 flag(s). Save to keep them.").arg(imported->size()));
}

void SettingsWindow::loadFromDisk() {
    {
        QFile file(flags_path());
        flagsEdit_->setPlainText(file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                                                 : QString());
    }
    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(tr("Failed to load settings: %1").arg(e.what()));
        return;
    }

    int index = gpuCombo_->findData(chosen_gpu_index(settings.gpu));
    if (index >= 0) {
        gpuCombo_->setCurrentIndex(index);
    }
    select_data(presentationCombo_, settings.presentation);
    renderScaleSpin_->setValue(settings.render_scale);
    select_data(upscalerCombo_, settings.upscaler);
    upscalerCombo_->setEnabled(settings.render_scale < 100);
    select_data(rendererCombo_, settings.renderer);
    select_data(titleBarCombo_, settings.title_bar);
    select_data(notificationsCombo_, settings.notifications);
    select_data(displayBackendCombo_, settings.display_backend);
    select_data(languageCombo_, settings.language);
    mangohudCheck_->setChecked(settings.mangohud);
    wrapperEdit_->setText(QString::fromStdString(settings.launch_wrapper));
    environmentEdit_->setPlainText(QString::fromStdString(settings.environment));
    setBackgroundFps(settings.background_fps);
    discordCheck_->setChecked(settings.discord_rich_presence);
    discordJoinCheck_->setChecked(settings.discord_join_button);
    discordJoinCheck_->setEnabled(settings.discord_rich_presence);
    controllersCheck_->setChecked(settings.game_controllers);
    closeOnLeaveCheck_->setChecked(settings.close_on_leave);
    jsdelivrCheck_->setChecked(settings.use_jsdelivr);
    serverRegionCheck_->setChecked(settings.server_region_notification);
    historyGamesCheck_->setChecked(settings.record_game_history);
    historyServersCheck_->setChecked(settings.record_server_history);
    historyServersCheck_->setEnabled(settings.record_game_history);
    gamemodeCheck_->setChecked(settings.gamemode);
    trayCheck_->setChecked(settings.system_tray);
    playerDirectlyCheck_->setChecked(settings.open_player_directly);
    cleanerSpin_->setValue(settings.cleaner_days);
}

namespace {

QString settings_socket_name() {
    return QStringLiteral("stud-settings-%1").arg(::getuid());
}

QPointer<SettingsWindow> g_open_window;
QLocalServer* g_open_server = nullptr;

// Size, place and maximized state, in Qt's own format, beside the Roblox windows' state.
QString window_geometry_path() {
    return QString::fromStdString(stud::paths::state_dir()) + QStringLiteral("/settings-window");
}

}  // namespace

void SettingsWindow::saveWindowGeometry() const {
    QDir().mkpath(QString::fromStdString(stud::paths::state_dir()));
    QSaveFile file(window_geometry_path());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(saveGeometry());
        file.commit();
    }
}

void SettingsWindow::closeEvent(QCloseEvent* event) {
    saveWindowGeometry();
    QWidget::closeEvent(event);
}

SettingsWindow* SettingsWindow::showSingleton(const QString& status) {
    if (g_open_window.isNull()) {
        g_open_window = new SettingsWindow();
        g_open_window->setAttribute(Qt::WA_DeleteOnClose);
        if (QFile file(window_geometry_path()); file.open(QIODevice::ReadOnly)) {
            g_open_window->restoreGeometry(file.readAll());
        }
        // Quitting Stud doesn't close its windows, so the one still open saves too.
        QObject::connect(qApp, &QCoreApplication::aboutToQuit, g_open_window, [window = g_open_window] {
            if (window != nullptr && window->isVisible()) window->saveWindowGeometry();
        });
    }
    if (!status.isEmpty()) g_open_window->setStatusMessage(status);
    g_open_window->show();
    g_open_window->raise();
    g_open_window->activateWindow();
    return g_open_window;
}

bool SettingsWindow::handOffToRunningInstance() {
    QLocalSocket socket;
    socket.connectToServer(settings_socket_name());
    if (!socket.waitForConnected(500)) return false;
    socket.write("show\n");
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

void SettingsWindow::listenForOpenRequests() {
    if (g_open_server != nullptr) return;
    g_open_server = new QLocalServer(qApp);
    // A killed process leaves its socket behind and QLocalServer won't bind over it.
    // Anything still listening would have answered the hand-off attempt above.
    QLocalServer::removeServer(settings_socket_name());
    if (!g_open_server->listen(settings_socket_name())) {
        std::fprintf(stderr, "stud: could not listen for settings requests: %s\n",
                     g_open_server->errorString().toUtf8().constData());
        return;
    }
    QObject::connect(g_open_server, &QLocalServer::newConnection, g_open_server, []() {
        while (QLocalSocket* client = g_open_server->nextPendingConnection()) {
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            showSingleton();
        }
    });
}

void SettingsWindow::setStatusMessage(const QString& text) {
    if (text.isEmpty()) {
        showIdleStatus();
        return;
    }
    statusLabel_->setText(text.toHtmlEscaped());
}

// The status line when idle: empty, or an update notice.
void SettingsWindow::showIdleStatus() {
    // Explicit colour: the palette's link colour isn't readable in every theme.
    const QString link = QStringLiteral(
        "<a href=\"%1\" style=\"color:#2f7fd6; text-decoration:underline;\">%2</a>");
    QStringList lines;
    if (UpdateCheck::updateAvailable()) {
        lines << link.arg(UpdateCheck::releasesUrl(), tr("Your Stud version is outdated!"));
    }
    statusLabel_->setText(lines.join(QStringLiteral("<br>")));
}

void SettingsWindow::paintRestartIcon() {
    restartButton_->setIcon(mono_icon("view-refresh", QStyle::SP_BrowserReload));
    if (restartButton_->icon().isNull()) restartButton_->setText(tr("Restart"));
}

// Save, stop the session and start it again. Settings are read at session start.
void SettingsWindow::onRestartClicked() {
    // Save first; a failed save doesn't restart with the old settings.
    if (!onSaveClicked()) return;
    if (!stud_session_is_running()) {
        statusLabel_->setText(tr("Settings saved."));
        return;
    }
    statusLabel_->setText(tr("Restarting Stud..."));
    // Let the label paint before stopping the session blocks this thread.
    QApplication::processEvents();
    terminate_stud_session();
    relaunch_stud();
}

bool SettingsWindow::onSaveClicked() {
    // A flags.json the client can't read is refused before anything is written.
    QString flags_error;
    const auto flags = parse_flags(flagsEdit_->toPlainText(), &flags_error);
    if (!flags) {
        statusLabel_->setText(tr("Nothing was saved. The FFlags are not valid JSON: %1")
                                  .arg(flags_error)
                                  .toHtmlEscaped());
        return false;
    }
    // Start from the file on disk: it may hold keys this window doesn't show.
    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(
            tr("Nothing was saved. Fix or delete the settings file first: %1")
                .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return false;
    }
    const std::string language_before = settings.language;
    // The "No GPUs found" placeholder carries no data and is never saved as a GPU.
    if (gpuCombo_->currentData().isValid()) {
        settings.gpu.device_index = static_cast<uint32_t>(gpuCombo_->currentData().toUInt());
        settings.gpu.device_name = gpuCombo_->currentText().toStdString();
        settings.gpu.device_uuid = gpuCombo_->currentData(Qt::UserRole + 2).toString().toStdString();
    }
    settings.presentation = selected_data(presentationCombo_);
    settings.render_scale = renderScaleSpin_->value();
    settings.upscaler = selected_data(upscalerCombo_);
    settings.renderer = selected_data(rendererCombo_);
    settings.title_bar = selected_data(titleBarCombo_);
    settings.notifications = selected_data(notificationsCombo_);
    settings.display_backend = selected_data(displayBackendCombo_);
    settings.language = selected_data(languageCombo_);
    settings.mangohud = mangohudCheck_->isChecked();
    settings.launch_wrapper = wrapperEdit_->text().trimmed().toStdString();
    settings.environment = environmentEdit_->toPlainText().toStdString();
    settings.background_fps = backgroundFpsSlider_->value() > stud::config::kBackgroundFpsUnlimited
                                  ? stud::config::kBackgroundFpsNoLimit
                                  : backgroundFpsSlider_->value();
    settings.discord_rich_presence = discordCheck_->isChecked();
    settings.discord_join_button = discordJoinCheck_->isChecked();
    settings.game_controllers = controllersCheck_->isChecked();
    settings.close_on_leave = closeOnLeaveCheck_->isChecked();
    settings.use_jsdelivr = jsdelivrCheck_->isChecked();
    settings.server_region_notification = serverRegionCheck_->isChecked();
    settings.record_game_history = historyGamesCheck_->isChecked();
    settings.record_server_history = historyServersCheck_->isChecked();
    settings.gamemode = gamemodeCheck_->isChecked();
    settings.system_tray = trayCheck_->isChecked();
    settings.open_player_directly = playerDirectlyCheck_->isChecked();
    settings.cleaner_days = cleanerSpin_->value();
    QDir().mkpath(QFileInfo(flags_path()).absolutePath());
    QSaveFile flags_file(flags_path());
    const QByteArray flags_text = format_flags(*flags).toUtf8();
    if (!flags_file.open(QIODevice::WriteOnly) || flags_file.write(flags_text) != flags_text.size() ||
        !flags_file.commit()) {
        statusLabel_->setText(tr("Could not write %1.").arg(flags_path()).toHtmlEscaped());
        return false;
    }
    flagsEdit_->setPlainText(QString::fromUtf8(flags_text));
    try {
        stud::config::save_settings(stud::config::default_config_path(), settings);
        if (settings.language != language_before) {
            statusLabel_->setText(tr("Language saved. Restart Stud to apply it."));
        } else {
            statusLabel_->setText(stud_session_is_running()
                                      ? tr("Settings saved. They apply the next time Roblox starts.")
                                      : tr("Settings saved."));
        }
        return true;
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(tr("Failed to save settings: %1").arg(e.what()));
        return false;
    }
}

}  // namespace stud::ui
