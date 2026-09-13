#include "settings_window.h"

#include "gpu_enum.h"
#include "stud/android_glue.h"
#include "stud/settings.h"
#include "stud/stud_paths.h"

#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
#include "stud/dev_backend_config.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QSlider>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QPixmap>

#include <fstream>

#include <unistd.h>

// Defined in main.cpp -- the session this window may have to stop and
// put back.
namespace stud::ui {
void terminate_stud_session();
bool stud_session_is_running();
void start_stud_session();
}  // namespace stud::ui

namespace stud::ui {

namespace {
// Entry order of the render-path list. Zink is last so that a build with
// the dev toggle off simply has one fewer entry and the other two keep
// their meaning.
// How large the app draws everything. Position 0 is "follow the
// display"; 1..kUiScaleSteps map linearly onto 0.5x..3.0x in 0.05x steps.
//
constexpr int kRenderPathVulkan = 0;
constexpr int kRenderPathAngleVulkan = 1;
constexpr int kRenderPathAngleDesktopGL = 2;
constexpr int kRenderPathSoftware = 3;
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
    general->addWidget(new QLabel("Roblox APK or bundle"));
    auto* apkRow = new QHBoxLayout();
    apkPathEdit_ = new QLineEdit(this);
    apkPathEdit_->setPlaceholderText("No APK loaded");
    apkPathEdit_->setReadOnly(true);
    auto* browseButton = new QPushButton("Browse...", this);
    apkRow->addWidget(apkPathEdit_);
    apkRow->addWidget(browseButton);
    general->addLayout(apkRow);
    connect(browseButton, &QPushButton::clicked, this, &SettingsWindow::onBrowseApkClicked);

    trayCheck_ = new QCheckBox("Show in system tray", this);
    general->addWidget(trayCheck_);
    closeOnLeaveCheck_ = new QCheckBox("Close Stud when leaving a game", this);
    general->addWidget(closeOnLeaveCheck_);
    serverRegionCheck_ = new QCheckBox("Notify the server region on join", this);
    general->addWidget(serverRegionCheck_);
    smoothZoomCheck_ = new QCheckBox("Smooth zoom", this);
    general->addWidget(smoothZoomCheck_);

    general->addStretch();

    // ---- Graphics ------------------------------------------------------
    auto* graphics = add_tab("Graphics");
    graphics->addWidget(new QLabel("Render path"));
    renderPathCombo_ = new QComboBox(this);
    // Ordered by how much hardware each one needs, most to least, so
    // moving down the list is what someone does when the one above did
    // not work.
    renderPathCombo_->addItem("Vulkan (recommended)");
    renderPathCombo_->addItem("ANGLE");
#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
    renderPathCombo_->addItem("OpenGL");
    renderPathCombo_->addItem("Software rendering");
#endif
    graphics->addWidget(renderPathCombo_);

    graphics->addWidget(new QLabel("GPU"));
    gpuCombo_ = new QComboBox(this);
    for (const auto& gpu : enumerate_gpus()) {
        gpuCombo_->addItem(QString::fromStdString(gpu.name), gpu.device_index);
    }
    if (gpuCombo_->count() == 0) {
        gpuCombo_->addItem("No GPUs found");
        gpuCombo_->setEnabled(false);
    }
    graphics->addWidget(gpuCombo_);

    // Sharpness only: how many pixels are drawn, never how big anything
    // looks. The size the app lays out against is fixed at 1.0 and is
    // not offered here, because the engine drops SSAO and its softer
    // shadows above that (see runtime/src/main.cpp, where it is set).
    hidpiCheck_ = new QCheckBox("HiDPI", this);
    hidpiCheck_->setToolTip("Render at the display's own scale. Sharper on a scaled screen; "
                             "off is blurrier and cheaper to draw.");
    graphics->addWidget(hidpiCheck_);

    // Size, not sharpness: lay the app out at the display's own scale so
    // it matches the rest of the desktop. The warning is not decoration
    // -- the engine reads this same scale to pick its render technique
    // and drops SSAO above 1.0, which is measured and unavoidable from
    // here (runtime/src/main.cpp says why).
    followDpiCheck_ = new QCheckBox("Follow DPI", this);
    followDpiCheck_->setToolTip(
        "Draw the app at your display's scale, so it is the same size as the rest of the "
        "desktop.\nWarning: the engine turns off ambient occlusion and its softer shadows "
        "above 1.0x.");
    graphics->addWidget(followDpiCheck_);
    // Only means anything while the buffer is being scaled: with HiDPI
    // off the compositor does the scaling and the engine stays at 1.0,
    // so the control would claim to do something it cannot.
    connect(hidpiCheck_, &QCheckBox::toggled, followDpiCheck_, &QWidget::setEnabled);

    // Render scale: the input end of the pipeline HiDPI sits at the
    // output end of. HiDPI decides how many real pixels the buffer has;
    // this decides how many of them the engine actually draws, and the
    // rest of the way is a scale back up to the window.
    //
    // Greyed out without HiDPI for the same reason as Follow DPI, and a
    // sharper one: with HiDPI off the compositor is already stretching
    // the buffer, so rendering smaller here would simply be resampled
    // twice.
    auto* renderScaleRow = new QHBoxLayout();
    renderScaleRow->addWidget(new QLabel("Render scale", this));
    renderScaleCombo_ = new QComboBox(this);
    // Percentages, not adjectives: "Quality"/"Performance" are marketing
    // for a number, and the number is the thing that is true.
    renderScaleCombo_->addItem("100% (native)", 100);
    renderScaleCombo_->addItem("85%", 85);
    renderScaleCombo_->addItem("75%", 75);
    renderScaleCombo_->addItem("67%", 67);
    renderScaleCombo_->addItem("50%", 50);
    renderScaleCombo_->setToolTip(
        "Draw the game at a fraction of the window's pixels and scale the result back up.\n"
        "Fewer pixels to draw means more frames; everything the engine draws, including its\n"
        "own menus and chat, is scaled with it.");
    renderScaleRow->addWidget(renderScaleCombo_);
    renderScaleRow->addStretch();
    graphics->addLayout(renderScaleRow);
    connect(hidpiCheck_, &QCheckBox::toggled, renderScaleCombo_, &QWidget::setEnabled);

    // Enabled or greyed out by the render path -- see onRenderPathChanged,
    // which also owns the tooltip and says why when it is unavailable.
    mangohudCheck_ = new QCheckBox("MangoHud overlay", this);
    graphics->addWidget(mangohudCheck_);
    connect(mangohudCheck_, &QCheckBox::toggled, this, &SettingsWindow::onMangohudToggled);
    // Connected only now that the checkbox it drives exists: the slot
    // dereferences it, and a combo signal arriving before that would be
    // a null dereference.
    connect(renderPathCombo_, &QComboBox::currentIndexChanged, this,
            &SettingsWindow::onRenderPathChanged);
    // Frame rate while Stud is in the background.
    //
    // Background means both halves of it: hidden (minimised, another
    // workspace, covered) and merely unfocused. A slider rather than a
    // checkbox because the useful answer is a number: a few frames a
    // second keeps the game alive and costs almost nothing, while someone
    // recording, waiting on a download, or watching it on a second
    // monitor wants it left alone. One step past the top reads as
    // Unlimited, so "do not throttle" is a position on the same control
    // rather than a second one.
    backgroundFpsLabel_ = new QLabel(this);
    graphics->addWidget(backgroundFpsLabel_);
    backgroundFpsSlider_ = new QSlider(Qt::Horizontal, this);
    backgroundFpsSlider_->setRange(1, stud::config::kBackgroundFpsUnlimited + 1);
    backgroundFpsSlider_->setSingleStep(1);
    backgroundFpsSlider_->setPageStep(10);
    backgroundFpsSlider_->setToolTip(
        "Frames per second while Stud is not the window you are using -- minimised,\n"
        "on another workspace, covered, or simply alt-tabbed away. Rendering a game\n"
        "at full rate for a window nobody is looking at wastes GPU and CPU.\n"
        "All the way right is Unlimited.");
    graphics->addWidget(backgroundFpsSlider_);
    connect(backgroundFpsSlider_, &QSlider::valueChanged, this,
            &SettingsWindow::onBackgroundFpsChanged);

    graphics->addStretch();

    // ---- Discord RPC ---------------------------------------------------
    auto* discord = add_tab("Discord RPC");
    discordCheck_ = new QCheckBox("Discord Rich Presence", this);
    discord->addWidget(discordCheck_);
    discordJoinCheck_ = new QCheckBox("Show a Join server button", this);
    discord->addWidget(discordJoinCheck_);
    connect(discordCheck_, &QCheckBox::toggled, discordJoinCheck_, &QWidget::setEnabled);
    discord->addStretch();

    // ---- About ---------------------------------------------------------
    auto* about = add_tab("About");
    auto* logo = new QLabel(this);
    logo->setPixmap(QPixmap(":/stud-logo.png")
                        .scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    logo->setAlignment(Qt::AlignCenter);
    about->addWidget(logo);
    auto* aboutText = new QLabel(
        "<div style='text-align:center'>"
        "<p><b>Version " STUD_VERSION "</b></p>"
        "<p>A Linux desktop wrapper that runs the real, unmodified<br>"
        "Roblox Android app.</p>"
        "<p><a href=\"https://github.com/CatPieLeaf/Stud\">"
        "github.com/CatPieLeaf/Stud</a></p>"
        "</div>",
        this);
    aboutText->setOpenExternalLinks(true);
    aboutText->setTextFormat(Qt::RichText);
    aboutText->setAlignment(Qt::AlignCenter);
    about->addWidget(aboutText);
    about->addStretch();

    // ---- Save, shared by every tab -------------------------------------
    auto* saveButton = new QPushButton("Save", this);
    layout->addWidget(saveButton);
    connect(saveButton, &QPushButton::clicked, this, &SettingsWindow::onSaveClicked);

    statusLabel_ = new QLabel(this);
    layout->addWidget(statusLabel_);

    loadFromDisk();
}

void SettingsWindow::loadFromDisk() {
    stud::config::StudSettings settings;
    try {
        settings = stud::config::load_settings(stud::config::default_config_path());
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(QString("Failed to load settings: %1").arg(e.what()));
        return;
    }

    // An empty name means nobody has ever picked one, which is not the
    // same as having picked device 0 -- see default_gpu_index().
    const uint32_t wanted = settings.gpu.device_name.empty()
                                ? default_gpu_index()
                                : settings.gpu.device_index;
    int index = gpuCombo_->findData(wanted);
    if (index >= 0) {
        gpuCombo_->setCurrentIndex(index);
    }
    hidpiCheck_->setChecked(settings.hidpi);
    followDpiCheck_->setChecked(settings.follow_dpi);
    followDpiCheck_->setEnabled(settings.hidpi);
    {
        const int index = renderScaleCombo_->findData(settings.render_scale_percent);
        renderScaleCombo_->setCurrentIndex(index >= 0 ? index : 0);
        renderScaleCombo_->setEnabled(settings.hidpi);
    }
    smoothZoomCheck_->setChecked(settings.smooth_zoom);
    // Unlimited is stored as 0 and lives at the far end of the slider.
    const int fps_position = settings.background_fps <= stud::config::kBackgroundFpsNoLimit
                                 ? stud::config::kBackgroundFpsUnlimited + 1
                                 : settings.background_fps;
    backgroundFpsSlider_->setValue(fps_position);
    onBackgroundFpsChanged(fps_position);
    mangohudCheck_->setChecked(settings.mangohud);
    closeOnLeaveCheck_->setChecked(settings.close_on_leave);
    trayCheck_->setChecked(settings.system_tray);
    serverRegionCheck_->setChecked(settings.server_region_notification);
    discordCheck_->setChecked(settings.discord_rich_presence);
    discordJoinCheck_->setChecked(settings.discord_join_button);
    discordJoinCheck_->setEnabled(settings.discord_rich_presence);
    // Carried through untouched: the window offers no field for it, but a
    // hand-edited id must survive a save.
    // The version of the APK Stud actually has, read out of its own
    // manifest -- not a path, and not the name of whatever file it was
    // copied from. That name is shown only while the user is picking;
    // after import the useful fact is which Roblox build this is.
    pickedApkPath_.clear();
    apkPathEdit_->setText(storedApkLabel());

    // Vulkan wins outright: the backend key is about which GL library to
    // load, and Vulkan mode loads none, so a saved "zink" alongside
    // Vulkan is not a third state -- it is a leftover.
    int path_index = kRenderPathVulkan;
    if (settings.graphics_mode != stud::config::GraphicsMode::kVulkan) {
        path_index = kRenderPathAngleVulkan;
#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
        try {
            if (auto backend =
                    stud::render::load_dev_render_backend_config(stud::config::default_config_path())) {
                if (backend->mode == stud::render::DevRenderBackendMode::kAngleDesktopGL) {
                    path_index = kRenderPathAngleDesktopGL;
                } else if (backend->mode == stud::render::DevRenderBackendMode::kAngleSwiftShader) {
                    path_index = kRenderPathSoftware;
                }
            }
        } catch (const std::runtime_error&) {
            // A malformed backend key is not worth refusing to show the
            // window over; the plain OpenGL entry is the honest default.
        }
#endif
    }
    renderPathCombo_->setCurrentIndex(path_index);
    // After the path, so the checkbox ends up matching it: setChecked()
    // above recorded what the user wants, and this decides whether the
    // chosen path can honour it.
    onRenderPathChanged(path_index);
}

// MangoHud can only overlay a render path it can actually hook, so the
// checkbox follows the render path rather than sitting there offering
// something that will not happen.
//
// Vulkan is render-host's own real driver calls, which its Vulkan layer
// hooks. OpenGL reaches the system EGL through ANGLE's desktop-GL
// backend, which its OpenGL hook catches once its shim is preloaded --
// live-confirmed, libMangoHud_opengl.so really is mapped into
// render-host there. The other two present through ANGLE's own private
// Vulkan and touch neither: its GL hook never loads, and its Vulkan
// layer crashes ANGLE the moment a swapchain is recreated, which is what
// joining a game does (MangoHud#1259, #1774). Stud disables that layer on
// those paths, so there is genuinely no overlay to offer.
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

void SettingsWindow::setStatusMessage(const QString& text) { statusLabel_->setText(text); }

void SettingsWindow::onRenderPathChanged(int index) {
    const bool can_overlay = index == kRenderPathVulkan || index == kRenderPathAngleDesktopGL;
    mangohudCheck_->setEnabled(can_overlay);
    if (can_overlay) {
        mangohudCheck_->setToolTip(
            "Shows MangoHud's performance overlay over Stud's own window.");
    } else {
        mangohudCheck_->setToolTip(
            "Not available on this render path: it presents through ANGLE's own\n"
            "Vulkan, which MangoHud cannot overlay without crashing on a game join.\n"
            "Use the Vulkan or OpenGL render path for the overlay.");
    }
    // The signal is blocked so restoring the box does not overwrite what
    // the user actually asked for -- switching away and back keeps it.
    const QSignalBlocker block(mangohudCheck_);
    mangohudCheck_->setChecked(can_overlay && mangohudWanted_);
}

void SettingsWindow::onMangohudToggled(bool checked) { mangohudWanted_ = checked; }

// What the picker shows when it is not mid-pick: which Roblox build
// Stud has, read out of the stored APK's own manifest. Empty when there
// is no APK or its manifest cannot be read, so the field falls back to
// its placeholder rather than claiming a version.
QString SettingsWindow::storedApkLabel() const {
    const std::string version =
        stud::android_glue::apk_version_name(stud::paths::stored_apk_path());
    if (version.empty()) return {};
    return QStringLiteral("Loaded version: %1").arg(QString::fromStdString(version));
}

void SettingsWindow::onBackgroundFpsChanged(int value) {
    backgroundFpsLabel_->setText(
        value > stud::config::kBackgroundFpsUnlimited
            ? QStringLiteral("Background frame rate: Unlimited")
            : QStringLiteral("Background frame rate: %1 FPS").arg(value));
}

void SettingsWindow::onBrowseApkClicked() {
    // Bundles too: Google Play ships Roblox as a split-APK bundle, so an
    // .apkm (APKMirror) or .apks (SAI) is easier to obtain than a merged
    // .apk and is what most people will actually have. Stud detects which
    // it was given by content, not extension.
    QString path = QFileDialog::getOpenFileName(
        this, "Select Roblox APK or bundle", QString(),
        "Roblox APK or bundle (*.apk *.apkm *.apks *.xapk);;APK files (*.apk);;"
        "Split-APK bundles (*.apkm *.apks *.xapk)");
    if (!path.isEmpty()) {
        pickedApkPath_ = path;
        apkPathEdit_->setText(QFileInfo(path).fileName());
    }
}

void SettingsWindow::onSaveClicked() {
    stud::config::StudSettings settings;
    settings.gpu.device_index = static_cast<uint32_t>(gpuCombo_->currentData().toUInt());
    settings.gpu.device_name = gpuCombo_->currentText().toStdString();
    settings.hidpi = hidpiCheck_->isChecked();
    settings.follow_dpi = followDpiCheck_->isChecked();
    settings.render_scale_percent = renderScaleCombo_->currentData().toInt();
    settings.smooth_zoom = smoothZoomCheck_->isChecked();
    settings.background_fps = backgroundFpsSlider_->value() > stud::config::kBackgroundFpsUnlimited
                                  ? stud::config::kBackgroundFpsNoLimit
                                  : backgroundFpsSlider_->value();
    settings.mangohud = mangohudCheck_->isChecked();
    settings.close_on_leave = closeOnLeaveCheck_->isChecked();
    settings.system_tray = trayCheck_->isChecked();
    settings.server_region_notification = serverRegionCheck_->isChecked();
    settings.discord_rich_presence = discordCheck_->isChecked();
    settings.discord_join_button = discordJoinCheck_->isChecked();
    const int render_path = renderPathCombo_->currentIndex();
    settings.graphics_mode = render_path == kRenderPathVulkan ? stud::config::GraphicsMode::kVulkan
                                                               : stud::config::GraphicsMode::kOpenGL;
    // Take a copy, and use the copy.
    //
    // The window is a picker: a user who picks an APK out of ~/Downloads
    // and then clears Downloads should not discover weeks later that
    // Stud will no longer launch. Stud keeps exactly one APK of its own,
    // under one fixed name in its data directory, and importing another
    // overwrites it -- so nothing points at the user's own file and
    // there is never a second copy quietly aging on disk.
    const QString stored = QString::fromStdString(stud::paths::stored_apk_path());
    bool imported = false;
    bool restart_session = false;
    if (!pickedApkPath_.isEmpty() &&
        QFileInfo(pickedApkPath_).absoluteFilePath() != QFileInfo(stored).absoluteFilePath()) {
        // Stop the session BEFORE replacing anything underneath it.
        //
        // A running Stud has the extracted libroblox.so mapped and the
        // engine's caches open; importing over them while it runs takes
        // it down -- live-reported as "if someone loads Stud's APK while
        // Stud is open, Stud crashes". The runtime also clears everything
        // derived from the previous build on the next launch, which is
        // not a thing to do to a live process either.
        //
        // Put back afterwards only if there was one: replacing the APK
        // from a closed Stud should not start a game.
        if (stud_session_is_running()) {
            statusLabel_->setText("Stopping Stud to replace the APK...");
            QApplication::processEvents();
            terminate_stud_session();
            restart_session = true;
        }
        if (!QDir().mkpath(QString::fromStdString(stud::paths::apk_dir()))) {
            statusLabel_->setText("Could not create Stud's APK directory.");
            return;
        }
        statusLabel_->setText("Importing the APK...");
        QApplication::processEvents();
        // Written beside the real file and renamed over it, so an
        // interrupted import cannot leave half an APK in place of a
        // working one.
        const QString partial = stored + ".part";
        QFile::remove(partial);
        if (!QFile::copy(pickedApkPath_, partial) ||
            (QFile::exists(stored) && !QFile::remove(stored)) ||
            !QFile::rename(partial, stored)) {
            QFile::remove(partial);
            statusLabel_->setText(QString("Could not copy the APK into %1 -- is there room?")
                                       .arg(QString::fromStdString(stud::paths::apk_dir())));
            return;
        }
        // Anything else in there is not Stud's: an earlier build kept
        // the picked file under its own name, and leaving a second
        // 230MB copy behind is exactly what keeping one APK is meant to
        // avoid.
        QDir store(QString::fromStdString(stud::paths::apk_dir()));
        for (const QString& name : store.entryList(QDir::Files | QDir::NoDotAndDotDot)) {
            if (name != QFileInfo(stored).fileName()) store.remove(name);
        }
        imported = true;
    }
    const std::string apk = stored.toStdString();

    // Real, once-per-import work: extract libroblox.so from the chosen
    // APK, once, right here at APK-selection time -- not on every game
    // launch. Only runs when the APK path actually changed (or no
    // cached extraction exists yet at all); an unrelated settings
    // change (GPU, HiDPI, ...) with the same APK re-saves without
    // touching this. No patching -- real bionic Process B loads the
    // extracted file completely unmodified (confirmed this session:
    // real bionic dlopen() resolves libroblox.so's entire real
    // dependency graph natively, zero byte patches needed).
    std::string cache_path = stud::android_glue::default_libroblox_cache_path();
    bool cache_exists = std::ifstream(cache_path).good();
    // Compare the APK's own identity, not just its path: re-saving the
    // same path after replacing the file with a newer build has to
    // re-extract, and it silently did not.
    const std::string apk_fingerprint = stud::android_glue::apk_source_fingerprint(apk);
    const std::string fingerprint_path = cache_path + ".source";
    std::string cached_fingerprint;
    if (std::ifstream stamp{fingerprint_path}) std::getline(stamp, cached_fingerprint);
    if (QFile::exists(stored) &&
        (imported || !cache_exists || cached_fingerprint != apk_fingerprint)) {
        try {
            stud::android_glue::extract_apk_native_library(apk, "libroblox.so", cache_path);
            std::ofstream(fingerprint_path, std::ios::trunc) << apk_fingerprint << "\n";
        } catch (const stud::android_glue::ExtractError& e) {
            statusLabel_->setText(QString("Failed to extract libroblox.so from the selected APK: %1").arg(e.what()));
            return;
        }
    }

    try {
        stud::config::save_settings(stud::config::default_config_path(), settings);
#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
        // Written on every save, including for Vulkan: leaving a stale
        // backend behind is what made a later switch to OpenGL come up on
        // the previous one with nothing in the window saying so.
        //
        // The mode is all this setting is: where ANGLE lives is looked up
        // at startup and never written down. See dev_backend_config.h.
        stud::render::DevRenderBackendConfig backend;
        if (render_path == kRenderPathAngleDesktopGL) {
            backend.mode = stud::render::DevRenderBackendMode::kAngleDesktopGL;
        } else if (render_path == kRenderPathSoftware) {
            backend.mode = stud::render::DevRenderBackendMode::kAngleSwiftShader;
        }
        stud::render::save_dev_render_backend_config(stud::config::default_config_path(), backend);
#endif
        // Back to the version, now that the picked name has served its
        // purpose (saying what was about to be imported).
        pickedApkPath_.clear();
        apkPathEdit_->setText(storedApkLabel());
        if (restart_session) {
            statusLabel_->setText("Starting Stud again...");
            QApplication::processEvents();
            start_stud_session();
            statusLabel_->setText("Settings saved. Stud restarted with the new APK.");
        } else {
            statusLabel_->setText("Settings saved.");
        }
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(QString("Failed to save settings: %1").arg(e.what()));
    } catch (const std::runtime_error& e) {
        statusLabel_->setText(QString("Failed to save render backend setting: %1").arg(e.what()));
    }
}

}  // namespace stud::ui
