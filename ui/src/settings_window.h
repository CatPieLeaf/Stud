#pragma once

#include <QWidget>
#include <QHash>
#include <functional>

#include <string>
#include <vector>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QVBoxLayout;
class QJsonObject;
class QSlider;
class QSpinBox;

namespace stud::ui {

// Stud's settings window, a front-end over stud-config's JSON file: loads on
// construction, writes on Save.
class SettingsWindow : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWindow(QWidget* parent = nullptr);

    // The one settings window, wherever the request came from.
    //
    // Two of these can disagree, both loaded the same file, both write
    // the whole of it on Save, so whichever is saved last silently undoes
    // the other. So there is exactly one: in this process, and (see below) across
    // processes too.
    static SettingsWindow* showSingleton(const QString& status = QString());

protected:
    // The desktop can switch between light and dark while this window is
    // open. The restart icon is drawn in the palette's own colour, so it
    // has to be drawn again when that colour changes.

public:

    // The desktop entry's Settings action starts a new stud-ui, which
    // knows nothing about the one already running the session. If that
    // one is listening, hand the request over and let it raise its own
    // window, opening Settings from the shortcut then behaves exactly
    // like opening it from the tray, because it IS that window.
    //
    // True when the request was handed off and this process should exit.
    static bool handOffToRunningInstance();

    // Listen for those requests. Called by a stud-ui that is staying
    // alive (the tray) and by a settings-only one that found nobody to
    // hand off to.
    static void listenForOpenRequests();

    // Says why this window opened, above the Save button, when Stud
    // opened it rather than the user asking for it.
    void setStatusMessage(const QString& text);

private:
    // The status line at rest: empty, or the update notice.
    void showIdleStatus();

public:

private slots:
    void onSaveClicked();
    void onRestartClicked();
    // Merges a JSON file's flags into the FFlags editor.
    void onImportFlagsClicked();
    void onImportBootstrapperClicked();
    void onBackgroundFpsChanged(int value);

protected:
    // Right-click resets the control under the pointer; see
    // installResets().
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void loadFromDisk();
    void installResets();
    void setBackgroundFps(int fps);

    QComboBox* gpuCombo_;
    QCheckBox* vsyncCheck_;
    QSpinBox* renderScaleSpin_;
    QComboBox* upscalerCombo_;
    QComboBox* rendererCombo_;
    QComboBox* titleBarCombo_;
    QComboBox* displayBackendCombo_;
    QCheckBox* gamemodeCheck_;
    QCheckBox* mangohudCheck_;
    QCheckBox* trayCheck_;
    QCheckBox* playerDirectlyCheck_;
    QSlider* backgroundFpsSlider_;
    QLabel* backgroundFpsLabel_;
    QCheckBox* discordCheck_;
    QCheckBox* discordJoinCheck_;
    QCheckBox* serverRegionCheck_;
    QSpinBox* cleanerSpin_;
    void buildHistoryTab(QVBoxLayout* tab);
    QPlainTextEdit* flagsEdit_;
    QLabel* flagsWarning_;
    // Each preset's control, set from the editor's JSON.
    std::vector<std::function<void(const QJsonObject&)>> flagRefresh_;
    void buildFlagsTab(QVBoxLayout* presets_tab, QVBoxLayout* tab);
    QPushButton* restartButton_;
    // Applies the palette's button-text colour to the restart glyph. Also
    // called on a palette change, hence keeping it around.
    void paintRestartIcon();

    QLabel* statusLabel_;

    // What each control's default is, as a closure that applies it. Keyed
    // by the widget, so the event filter can look up whatever was
    // right-clicked. Every value comes from a default-constructed
    // StudSettings rather than being written down a second time.
    QHash<QObject*, std::function<void()>> resets_;
};

}  // namespace stud::ui
