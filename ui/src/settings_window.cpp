#include "settings_window.h"

#include "update_check.h"

#include "cube_view.h"
#include "gpu_enum.h"
#include "notifications.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

#include <QStyle>
#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QUrl>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QFileDialog>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSlider>
#include <QSaveFile>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QPixmap>
#include <QEvent>
#include <QPainter>

#include <cstdio>

#include <unistd.h>

// Defined in main.cpp, the session this window may have to stop and
// put back.
namespace stud::ui {
void terminate_stud_session();
bool stud_session_is_running();
void relaunch_stud();
}  // namespace stud::ui

namespace stud::ui {

namespace {
// Selects the entry whose data is `value`, or the first one.
void select_data(QComboBox* combo, const std::string& value) {
    const int index = combo->findData(QString::fromStdString(value));
    combo->setCurrentIndex(index >= 0 ? index : 0);
}

std::string selected_data(const QComboBox* combo) {
    return combo->currentData().toString().toStdString();
}
}  // namespace

SettingsWindow::SettingsWindow(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Stud Settings");

    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    layout->addWidget(tabs);

    // Every tab is a plain vertical list ending in a stretch, so the
    // controls sit at the top rather than spreading to fill the tab.
    auto add_tab = [tabs](const QString& title) {
        auto* page = new QWidget();
        auto* page_layout = new QVBoxLayout(page);
        tabs->addTab(page, title);
        return page_layout;
    };

    // ---- General -------------------------------------------------------
    auto* general = add_tab("General");
    trayCheck_ = new QCheckBox("Show in system tray", this);
    general->addWidget(trayCheck_);
    gamemodeCheck_ = new QCheckBox("GameMode", this);
    gamemodeCheck_->setToolTip("Ask Feral GameMode for its performance settings while Roblox runs.");
    general->addWidget(gamemodeCheck_);
    serverRegionCheck_ = new QCheckBox("Notify the server region on join", this);
    serverRegionCheck_->setToolTip("Names the country of the game server each time you join one.");
    general->addWidget(serverRegionCheck_);

    general->addWidget(new QLabel("MacOS title bar"));
    titleBarCombo_ = new QComboBox(this);
    titleBarCombo_->addItem("MacOS title bar and menu bar", QStringLiteral("macos"));
    titleBarCombo_->addItem("MacOS title bar only", QStringLiteral("macos-no-menu"));
    titleBarCombo_->addItem("Linux native title bar", QStringLiteral("native"));
    general->addWidget(titleBarCombo_);

    // A choice only on a Wayland desktop that also runs XWayland: anywhere
    // else there is one way to open a window.
    auto* displayBackendLabel = new QLabel("Display server", this);
    general->addWidget(displayBackendLabel);
    displayBackendCombo_ = new QComboBox(this);
    displayBackendCombo_->addItem("Wayland", QStringLiteral("wayland"));
    displayBackendCombo_->addItem("X11 (XWayland)", QStringLiteral("x11"));
    general->addWidget(displayBackendCombo_);
    const bool backend_choice = !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") &&
                                !qEnvironmentVariableIsEmpty("DISPLAY");
    displayBackendLabel->setVisible(backend_choice);
    displayBackendCombo_->setVisible(backend_choice);

    general->addStretch();

    // ---- Graphics ------------------------------------------------------
    auto* graphics = add_tab("Graphics");
    graphics->addWidget(new QLabel("GPU"));
    gpuCombo_ = new QComboBox(this);
    for (const auto& gpu : enumerate_gpus()) {
        gpuCombo_->addItem(QString::fromStdString(gpu.name), gpu.device_index);
        gpuCombo_->setItemData(gpuCombo_->count() - 1, gpu.vendor_id, Qt::UserRole + 1);
        gpuCombo_->setItemData(gpuCombo_->count() - 1, QString::fromStdString(gpu.uuid),
                               Qt::UserRole + 2);
    }
    if (gpuCombo_->count() == 0) {
        gpuCombo_->addItem("No GPUs found");
        gpuCombo_->setEnabled(false);
    }
    graphics->addWidget(gpuCombo_);

    vsyncCheck_ = new QCheckBox("VSync", this);
    vsyncCheck_->setToolTip("Wait for the display's refresh. Off presents frames as soon as "
                            "they are ready.");
    graphics->addWidget(vsyncCheck_);

    // Frame rate while Roblox is in the background: hidden (minimised,
    // another workspace, covered) or merely unfocused. One step past the top
    // reads as Unlimited.
    backgroundFpsLabel_ = new QLabel(this);
    graphics->addWidget(backgroundFpsLabel_);
    backgroundFpsSlider_ = new QSlider(Qt::Horizontal, this);
    backgroundFpsSlider_->setRange(1, stud::config::kBackgroundFpsUnlimited + 1);
    backgroundFpsSlider_->setPageStep(10);
    backgroundFpsSlider_->setToolTip(
        "Frames per second while Roblox is not the window you are using: minimised,\n"
        "on another workspace, covered, or alt-tabbed away. All the way right is Unlimited.");
    graphics->addWidget(backgroundFpsSlider_);
    connect(backgroundFpsSlider_, &QSlider::valueChanged, this,
            &SettingsWindow::onBackgroundFpsChanged);

    mangohudCheck_ = new QCheckBox("MangoHud overlay", this);
    mangohudCheck_->setToolTip("Shows MangoHud's performance overlay over Roblox.");
    graphics->addWidget(mangohudCheck_);

    graphics->addStretch();

    // ---- Discord RPC ---------------------------------------------------
    auto* discord = add_tab("Discord RPC");
    discordCheck_ = new QCheckBox("Discord Rich Presence", this);
    discordCheck_->setToolTip("Shows the game you are playing on your Discord profile, with what "
                              "the game itself reports.");
    discord->addWidget(discordCheck_);
    discordJoinCheck_ = new QCheckBox("Show a Join server button", this);
    discord->addWidget(discordJoinCheck_);
    connect(discordCheck_, &QCheckBox::toggled, discordJoinCheck_, &QWidget::setEnabled);
    discord->addStretch();

    // ---- FFlags --------------------------------------------------------
    // flags.json in Stud's config directory, as JSON: written into the
    // client's ClientAppSettings.json at every launch.
    auto* fflags = add_tab("FFlags");
    flagsEdit_ = new QPlainTextEdit(this);
    flagsEdit_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    flagsEdit_->setPlaceholderText("{\n    \"FFlagExample\": true\n}");
    fflags->addWidget(flagsEdit_, 1);
    auto* importButton = new QPushButton("Import JSON...", this);
    importButton->setToolTip("Add the flags of a JSON file to these; a flag both have takes the "
                             "imported value.");
    fflags->addWidget(importButton, 0, Qt::AlignLeft);
    connect(importButton, &QPushButton::clicked, this, &SettingsWindow::onImportFlagsClicked);

    // ---- About ---------------------------------------------------------
    auto* about = add_tab("About");
    auto* cube = new CubeView(this);
    cube->setFixedSize(128, 128);
    // Ten clicks on the cube, on the GPU picked above being NVIDIA's.
    cube->on_tenth_click = [this] {
        constexpr uint32_t kNvidia = 0x10DE;
        if (gpuCombo_->currentData(Qt::UserRole + 1).toUInt() != kNvidia) return;
        send_notification(QStringLiteral("Stud"),
                          QStringLiteral("This GPU model may experience stability due to a decent "
                                         "Vulkan memory allocator in its driver. It's recommended "
                                         "you use Vulkan if you experience stability."));
    };
    about->addWidget(cube, 0, Qt::AlignHCenter);
    auto* logo = new QLabel(this);
    logo->setPixmap(QPixmap(":/stud-title.png")
                        .scaled(160, 160, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    about->addWidget(logo);
    auto* aboutText = new QLabel(
        "<div style='text-align:center'>"
        "<p><b>Version " STUD_VERSION "</b></p>"
        "<p>Runs Roblox's real, unmodified macOS client<br>"
        "on Linux.</p>"
        "<p><a href=\"https://github.com/CatPieLeaf/Stud\">"
        "CatPieLeaf/Stud</a></p>"
        "</div>",
        this);
    aboutText->setOpenExternalLinks(true);
    aboutText->setTextFormat(Qt::RichText);
    aboutText->setAlignment(Qt::AlignCenter);
    about->addWidget(aboutText);
    about->addStretch();

    // ---- Save, shared by every tab -------------------------------------
    auto* buttonRow = new QHBoxLayout();
    // Restart, for comparing two settings without a trip through the tray:
    // save, restart, look, repeat. Only meaningful while a session is
    // actually running, so it appears only then, there is nothing to
    // restart otherwise, and Save already starts one when it has to.
    restartButton_ = new QPushButton(this);
    paintRestartIcon();
    restartButton_->setToolTip("Save and restart Stud");
    // Icon only: the row's width belongs to Save, and a refresh glyph says
    // this on its own. With no glyph anywhere, paintRestartIcon() leaves a
    // word instead, and a word needs room.
    if (!restartButton_->icon().isNull()) {
        restartButton_->setFixedWidth(36);
    }
    // Only when there is a session to restart. Opened from the desktop
    // entry with nothing running, Save is the whole story.
    restartButton_->setVisible(stud_session_is_running());
    buttonRow->addWidget(restartButton_);
    connect(restartButton_, &QPushButton::clicked, this, &SettingsWindow::onRestartClicked);

    auto* saveButton = new QPushButton("Save", this);
    buttonRow->addWidget(saveButton);
    layout->addLayout(buttonRow);
    connect(saveButton, &QPushButton::clicked, this, &SettingsWindow::onSaveClicked);

    statusLabel_ = new QLabel(this);
    // The status line doubles as where an available update is announced,
    // so it has to render a link and open it.
    statusLabel_->setTextFormat(Qt::RichText);
    statusLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    statusLabel_->setOpenExternalLinks(true);
    layout->addWidget(statusLabel_);

    // Ask GitHub whether there is a newer Stud. The tray may have asked already, in which
    // case this costs nothing and the answer is remembered; see UpdateCheck.
    connect(UpdateCheck::instance(), &UpdateCheck::updateFound, this,
            [this] { showIdleStatus(); });
    UpdateCheck::start();

    loadFromDisk();
    installResets();
    // Ask now, rather than only on the signal. The tray starts the same
    // check at launch, so by the time this window is opened the answer is
    // usually already known and `updateFound` has long since fired,
    // connecting to it and waiting meant Settings never showed the notice
    // at all.
    showIdleStatus();
}

// Right-click a control to put it back to its default.
//
// The defaults come from a default-constructed StudSettings, which is
// where they are already defined, writing them out again here would be a
// second copy to drift out of step with the first.
void SettingsWindow::installResets() {
    const stud::config::StudSettings d;
    auto add = [this](QWidget* widget, std::function<void()> reset) {
        if (widget == nullptr) return;
        widget->installEventFilter(this);
        resets_.insert(widget, std::move(reset));
    };

    add(gpuCombo_, [this] {
        // The default is whichever device Stud would have picked on its
        // own, not simply the first row.
        const int index = gpuCombo_->findData(default_gpu_index());
        gpuCombo_->setCurrentIndex(index >= 0 ? index : 0);
    });
    add(vsyncCheck_, [this, d] { vsyncCheck_->setChecked(d.vsync); });
    add(titleBarCombo_, [this, d] { select_data(titleBarCombo_, d.title_bar); });
    add(displayBackendCombo_, [this, d] { select_data(displayBackendCombo_, d.display_backend); });
    add(gamemodeCheck_, [this, d] { gamemodeCheck_->setChecked(d.gamemode); });
    add(mangohudCheck_, [this, d] { mangohudCheck_->setChecked(d.mangohud); });
    add(backgroundFpsSlider_, [this, d] { setBackgroundFps(d.background_fps); });
    add(discordCheck_, [this, d] { discordCheck_->setChecked(d.discord_rich_presence); });
    add(discordJoinCheck_, [this, d] { discordJoinCheck_->setChecked(d.discord_join_button); });
    add(serverRegionCheck_,
        [this, d] { serverRegionCheck_->setChecked(d.server_region_notification); });
    add(trayCheck_, [this, d] { trayCheck_->setChecked(d.system_tray); });
}

bool SettingsWindow::eventFilter(QObject* watched, QEvent* event) {
    // The context-menu event rather than a raw right-button press: it is
    // what a right-click means on every platform, and it arrives whether
    // the pointer or the keyboard asked for it.
    if (event->type() == QEvent::ContextMenu) {
        const auto reset = resets_.constFind(watched);
        if (reset != resets_.constEnd()) {
            (*reset)();
            statusLabel_->setText("Reset to default. Save to keep it.");
            return true;  // no empty context menu behind it
        }
    }
    return QWidget::eventFilter(watched, event);
}

namespace {

QString flags_path() { return QString::fromStdString(stud::config::default_flag_overrides_path()); }

// The editor's text as a JSON object; an empty editor is no flags. Empty
// with `error` set when it is not one.
std::optional<QJsonObject> parse_flags(const QString& text, QString* error) {
    if (text.trimmed().isEmpty()) return QJsonObject();
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parse);
    if (parse.error != QJsonParseError::NoError) {
        *error = QStringLiteral("%1 (at character %2)").arg(parse.errorString()).arg(parse.offset);
        return std::nullopt;
    }
    if (!doc.isObject()) {
        *error = QStringLiteral("it is not a JSON object");
        return std::nullopt;
    }
    return doc.object();
}

QString format_flags(const QJsonObject& flags) {
    return flags.isEmpty() ? QString()
                           : QString::fromUtf8(QJsonDocument(flags).toJson(QJsonDocument::Indented));
}

}  // namespace

// Unlimited is stored as 0 and lives one step past the top of the slider.
void SettingsWindow::setBackgroundFps(int fps) {
    const int position =
        fps <= stud::config::kBackgroundFpsNoLimit ? stud::config::kBackgroundFpsUnlimited + 1 : fps;
    backgroundFpsSlider_->setValue(position);
    onBackgroundFpsChanged(position);
}

void SettingsWindow::onBackgroundFpsChanged(int value) {
    backgroundFpsLabel_->setText(value > stud::config::kBackgroundFpsUnlimited
                                     ? QStringLiteral("Background frame rate: Unlimited")
                                     : QStringLiteral("Background frame rate: %1 FPS").arg(value));
}

void SettingsWindow::onImportFlagsClicked() {
    const QString path = QFileDialog::getOpenFileName(this, "Import FFlags", QString(),
                                                      "JSON files (*.json);;All files (*)");
    if (path.isEmpty()) return;
    QFile file(path);
    QString error;
    if (!file.open(QIODevice::ReadOnly)) {
        statusLabel_->setText(QString("Could not read %1.").arg(path).toHtmlEscaped());
        return;
    }
    const auto imported = parse_flags(QString::fromUtf8(file.readAll()), &error);
    if (!imported) {
        statusLabel_->setText(QString("%1 is not usable: %2").arg(path, error).toHtmlEscaped());
        return;
    }
    const auto current = parse_flags(flagsEdit_->toPlainText(), &error);
    if (!current) {
        statusLabel_->setText(
            QString("Fix the flags above before importing: %1").arg(error).toHtmlEscaped());
        return;
    }
    QJsonObject merged = *current;
    for (auto it = imported->begin(); it != imported->end(); ++it) merged.insert(it.key(), it.value());
    flagsEdit_->setPlainText(format_flags(merged));
    statusLabel_->setText(QString("Imported %1 flag(s). Save to keep them.").arg(imported->size()));
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
        statusLabel_->setText(QString("Failed to load settings: %1").arg(e.what()));
        return;
    }

    int index = gpuCombo_->findData(chosen_gpu_index(settings.gpu));
    if (index >= 0) {
        gpuCombo_->setCurrentIndex(index);
    }
    vsyncCheck_->setChecked(settings.vsync);
    select_data(titleBarCombo_, settings.title_bar);
    select_data(displayBackendCombo_, settings.display_backend);
    mangohudCheck_->setChecked(settings.mangohud);
    setBackgroundFps(settings.background_fps);
    discordCheck_->setChecked(settings.discord_rich_presence);
    discordJoinCheck_->setChecked(settings.discord_join_button);
    discordJoinCheck_->setEnabled(settings.discord_rich_presence);
    serverRegionCheck_->setChecked(settings.server_region_notification);
    gamemodeCheck_->setChecked(settings.gamemode);
    trayCheck_->setChecked(settings.system_tray);
}

namespace {

// One well-known name per user session, beside the render socket Stud
// already keeps there.
QString settings_socket_name() {
    return QStringLiteral("stud-settings-%1").arg(::getuid());
}

QPointer<SettingsWindow> g_open_window;
QLocalServer* g_open_server = nullptr;

}  // namespace

SettingsWindow* SettingsWindow::showSingleton(const QString& status) {
    if (g_open_window.isNull()) {
        g_open_window = new SettingsWindow();
        g_open_window->setAttribute(Qt::WA_DeleteOnClose);
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
    // A server socket outlives a process that was killed rather than
    // closed, and QLocalServer will not bind over one. Removing it is
    // safe here: anything still listening on it would have answered the
    // hand-off attempt that ran first.
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

// What the status line says when it has nothing else to say: either
// nothing, or that this Stud is out of date.
//
// It lives here rather than in a banner of its own because this is the
// one line in the window that is already about "what just happened", and
// an update is news of exactly that kind. Anything the user actually does
// saving, restarting, replaces it, and it comes back when that
// message is cleared.
void SettingsWindow::showIdleStatus() {
    // Blue and underlined, stated rather than inherited: a link's default
    // colour comes from the palette and is not dependable across themes,
    // and this one has to read as a link in both.
    const QString link = QStringLiteral(
        "<a href=\"%1\" style=\"color:#2f7fd6; text-decoration:underline;\">%2</a>");
    QStringList lines;
    if (UpdateCheck::updateAvailable()) {
        lines << link.arg(UpdateCheck::releasesUrl(), QStringLiteral("Your Stud version is outdated!"));
    }
    statusLabel_->setText(lines.join(QStringLiteral("<br>")));
}

// Save, stop the running session, start it again. The whole point is the
// A/B loop: change a setting, see it, change it back.
//
// Not a plain restart: settings are read when a session STARTS, so
// restarting without saving would relaunch the old ones and look like the
// change did nothing.
void SettingsWindow::paintRestartIcon() {
    // Draws the restart glyph in the palette's own button-text colour.
    //
    // Neither source of that glyph gets the colour right by itself. A Breeze
    // monochrome icon is a fixed dark glyph, measured at rgb(35,37,40): KDE's
    // own applications recolour it at runtime through KIconTheme, and a plain
    // Qt application does not, so a dark colour scheme draws it dark on dark.
    // Where no icon theme is installed, Qt falls back to SP_BrowserReload, a
    // fixed blue-grey bitmap at rgb(59,107,146) whatever the scheme.
    //
    // Each is a single-colour glyph. Repainting every opaque pixel in the
    // palette colour keeps the shape and settles the colour, and the button
    // then follows an accent or a light/dark switch.
    QIcon source = QIcon::fromTheme(QStringLiteral("view-refresh"),
                                    QIcon::fromTheme(QStringLiteral("system-reboot")));
    if (source.isNull()) {
        source = style()->standardIcon(QStyle::SP_BrowserReload);
    }
    if (source.isNull()) {
        // Nothing to draw anywhere: a word beats an empty button.
        restartButton_->setIcon(QIcon());
        restartButton_->setText(QStringLiteral("Restart"));
        return;
    }

    const QColor colour = restartButton_->palette().buttonText().color();
    QIcon painted;
    // Several sizes, so a HiDPI screen picks a real one instead of scaling
    // a small pixmap up.
    for (const int size : {16, 22, 24, 32, 48}) {
        QPixmap pixmap = source.pixmap(QSize(size, size));
        if (pixmap.isNull()) continue;
        QPainter painter(&pixmap);
        // Keeps the glyph's own alpha, replaces its colour.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), colour);
        painter.end();
        painted.addPixmap(pixmap);
    }
    restartButton_->setIcon(painted.isNull() ? source : painted);
}

void SettingsWindow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    // A colour-scheme switch while this window is open changes the colour
    // the glyph belongs in.
    if (event->type() == QEvent::PaletteChange && restartButton_ != nullptr) {
        paintRestartIcon();
    }
}

void SettingsWindow::onRestartClicked() {
    // Save first, through the ordinary path, settings are read when a
    // session STARTS, so restarting without saving would relaunch the old
    // ones and look like the change did nothing.
    onSaveClicked();
    if (!stud_session_is_running()) {
        // Nothing running: the next launch reads what was saved.
        statusLabel_->setText("Settings saved.");
        return;
    }
    statusLabel_->setText("Restarting Stud...");
    // Let the label paint: stopping the session blocks this thread for a
    // moment.
    QApplication::processEvents();
    terminate_stud_session();
    // As a new process, the way closing and reopening Stud does it; see
    // relaunch_stud().
    relaunch_stud();
}

void SettingsWindow::onSaveClicked() {
    // The flags first: a flags.json the client cannot read is refused
    // before anything is written.
    QString flags_error;
    const auto flags = parse_flags(flagsEdit_->toPlainText(), &flags_error);
    if (!flags) {
        statusLabel_->setText(QString("Nothing was saved. The FFlags are not valid JSON: %1")
                                  .arg(flags_error)
                                  .toHtmlEscaped());
        return;
    }
    // From what is on disk: the file may hold keys this window does not
    // show. A file that cannot be read is not overwritten at all.
    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(
            QString("Nothing was saved. Fix or delete the settings file first: %1")
                .arg(QString::fromUtf8(e.what()).toHtmlEscaped()));
        return;
    }
    settings.gpu.device_index = static_cast<uint32_t>(gpuCombo_->currentData().toUInt());
    settings.gpu.device_name = gpuCombo_->currentText().toStdString();
    settings.gpu.device_uuid = gpuCombo_->currentData(Qt::UserRole + 2).toString().toStdString();
    settings.vsync = vsyncCheck_->isChecked();
    settings.title_bar = selected_data(titleBarCombo_);
    settings.display_backend = selected_data(displayBackendCombo_);
    settings.mangohud = mangohudCheck_->isChecked();
    settings.background_fps = backgroundFpsSlider_->value() > stud::config::kBackgroundFpsUnlimited
                                  ? stud::config::kBackgroundFpsNoLimit
                                  : backgroundFpsSlider_->value();
    settings.discord_rich_presence = discordCheck_->isChecked();
    settings.discord_join_button = discordJoinCheck_->isChecked();
    settings.server_region_notification = serverRegionCheck_->isChecked();
    settings.gamemode = gamemodeCheck_->isChecked();
    settings.system_tray = trayCheck_->isChecked();
    QDir().mkpath(QFileInfo(flags_path()).absolutePath());
    QSaveFile flags_file(flags_path());
    const QByteArray flags_text = format_flags(*flags).toUtf8();
    if (!flags_file.open(QIODevice::WriteOnly) || flags_file.write(flags_text) != flags_text.size() ||
        !flags_file.commit()) {
        statusLabel_->setText(QString("Could not write %1.").arg(flags_path()).toHtmlEscaped());
        return;
    }
    flagsEdit_->setPlainText(QString::fromUtf8(flags_text));
    try {
        stud::config::save_settings(stud::config::default_config_path(), settings);
        statusLabel_->setText(stud_session_is_running()
                                  ? "Settings saved. They apply the next time Roblox starts."
                                  : "Settings saved.");
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(QString("Failed to save settings: %1").arg(e.what()));
    }
}

}  // namespace stud::ui
