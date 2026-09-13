#include "settings_window.h"

#include "gpu_enum.h"
#include "stud/android_glue.h"
#include "stud/settings.h"

#ifdef STUD_ENABLE_DEV_RENDER_TOGGLE
#include "stud/dev_backend_config.h"
#endif

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
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
    apkPathEdit_->setPlaceholderText("No APK selected -- required to launch");
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
    smoothZoomCheck_->setChecked(settings.smooth_zoom);
    backgroundFpsSlider_->setValue(settings.background_fps);
    onBackgroundFpsChanged(settings.background_fps);
    mangohudCheck_->setChecked(settings.mangohud);
    closeOnLeaveCheck_->setChecked(settings.close_on_leave);
    trayCheck_->setChecked(settings.system_tray);
    serverRegionCheck_->setChecked(settings.server_region_notification);
    discordCheck_->setChecked(settings.discord_rich_presence);
    discordJoinCheck_->setChecked(settings.discord_join_button);
    discordJoinCheck_->setEnabled(settings.discord_rich_presence);
    // Carried through untouched: the window offers no field for it, but a
    // hand-edited id must survive a save.
    apkPathEdit_->setText(QString::fromStdString(settings.apk_path));
    loadedApkPath_ = settings.apk_path;

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
        apkPathEdit_->setText(path);
    }
}

void SettingsWindow::onSaveClicked() {
    stud::config::StudSettings settings;
    settings.gpu.device_index = static_cast<uint32_t>(gpuCombo_->currentData().toUInt());
    settings.gpu.device_name = gpuCombo_->currentText().toStdString();
    settings.hidpi = hidpiCheck_->isChecked();
    settings.follow_dpi = followDpiCheck_->isChecked();
    settings.smooth_zoom = smoothZoomCheck_->isChecked();
    settings.background_fps = backgroundFpsSlider_->value();
    settings.mangohud = mangohudCheck_->isChecked();
    settings.close_on_leave = closeOnLeaveCheck_->isChecked();
    settings.system_tray = trayCheck_->isChecked();
    settings.server_region_notification = serverRegionCheck_->isChecked();
    settings.discord_rich_presence = discordCheck_->isChecked();
    settings.discord_join_button = discordJoinCheck_->isChecked();
    const int render_path = renderPathCombo_->currentIndex();
    settings.graphics_mode = render_path == kRenderPathVulkan ? stud::config::GraphicsMode::kVulkan
                                                               : stud::config::GraphicsMode::kOpenGL;
    settings.apk_path = apkPathEdit_->text().toStdString();

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
    const std::string apk_fingerprint =
        stud::android_glue::apk_source_fingerprint(settings.apk_path);
    const std::string fingerprint_path = cache_path + ".source";
    std::string cached_fingerprint;
    if (std::ifstream stamp{fingerprint_path}) std::getline(stamp, cached_fingerprint);
    if (!settings.apk_path.empty() &&
        (settings.apk_path != loadedApkPath_ || !cache_exists ||
         cached_fingerprint != apk_fingerprint)) {
        try {
            stud::android_glue::extract_apk_native_library(settings.apk_path, "libroblox.so", cache_path);
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
        loadedApkPath_ = settings.apk_path;
        statusLabel_->setText("Settings saved.");
    } catch (const stud::config::SettingsError& e) {
        statusLabel_->setText(QString("Failed to save settings: %1").arg(e.what()));
    } catch (const std::runtime_error& e) {
        statusLabel_->setText(QString("Failed to save render backend setting: %1").arg(e.what()));
    }
}

}  // namespace stud::ui
