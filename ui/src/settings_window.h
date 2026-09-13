#pragma once

#include <QWidget>

#include <string>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;

namespace stud::ui {

// Real settings window (M8): GPU picker, HiDPI toggle, graphics mode --
// the first real, non-placeholder piece of Stud's Qt6 UI. Backed
// directly by stud-config's JSON file ("UI is a front-end over the JSON
// config file, not a separate model" -- locked decision): loads on
// construction, writes on Save. Plain Qt6 Widgets, no libplasma (see
// the engineering notes' locked-decisions table for why that decision was
// revised at M8).
class SettingsWindow : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWindow(QWidget* parent = nullptr);

    // Says why this window opened, above the Save button, when Stud
    // opened it rather than the user asking for it.
    void setStatusMessage(const QString& text);

private slots:
    void onSaveClicked();
    void onBrowseApkClicked();
    void onRenderPathChanged(int index);
    void onMangohudToggled(bool checked);
    void onBackgroundFpsChanged(int value);

private:
    void loadFromDisk();

    // Real path this APK's libroblox.so was originally loaded from
    // (settings.apk_path as read from disk at construction time) --
    // compared against the field's live text on Save so the
    // extraction+patch-attempt work (task #17: real, once-per-import
    // work, not something to redo on every unrelated settings change)
    // only runs when the user actually picked a *different* APK, or
    // when no cached extraction exists yet at all.
    std::string loadedApkPath_;

    QComboBox* gpuCombo_;
    // Sharpness: whether the buffer is rendered at the display's own
    // scale. Its off state used to write 1.0 over the display's real
    // scale as well as the buffer's, which is why it appeared to do
    // nothing but stretch the window; those are separate values now.
    QCheckBox* hidpiCheck_;
    QCheckBox* followDpiCheck_;
    QCheckBox* smoothZoomCheck_;
    QSlider* backgroundFpsSlider_;
    QLabel* backgroundFpsLabel_;
    QCheckBox* mangohudCheck_;
    // What the user last asked for, kept across a render path that cannot
    // show the overlay -- switching to ANGLE and back should not silently
    // lose the setting.
    bool mangohudWanted_ = false;
    QCheckBox* closeOnLeaveCheck_;
    QCheckBox* trayCheck_;
    QCheckBox* serverRegionCheck_;
    QCheckBox* discordCheck_;
    QCheckBox* discordJoinCheck_;
    QLineEdit* apkPathEdit_;

    // One control for what is really one choice. It used to be two: a
    // Vulkan/OpenGL radio pair plus a separate ANGLE/Zink combo that only
    // meant anything in OpenGL mode and sat there looking selected the
    // rest of the time. Picking OpenGL while the other control still said
    // Zink read as "I chose OpenGL and it stayed on Zink", which is a
    // fair reading of what the window showed. The three entries are the
    // three things Stud can actually do; they still write the same two
    // config keys, so nothing downstream changes.
    //
    // The Zink entry is prototype-only (the engineering notes, render/
    // dev_backend_config.h) and goes before any public release.
    QComboBox* renderPathCombo_;

    QLabel* statusLabel_;
};

}  // namespace stud::ui
