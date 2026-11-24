#pragma once

#include <QWidget>
#include <QHash>
#include <functional>

#include <string>
#include <vector>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QVBoxLayout;
class QJsonObject;
class QSlider;
class QSpinBox;

namespace stud::ui {

// Loads stud-config's JSON file on construction and writes it on Save.
class SettingsWindow : public QWidget {
    Q_OBJECT

public:
    explicit SettingsWindow(QWidget* parent = nullptr);

    // Only one exists, across processes too: two windows would each save the whole
    // file and the last one would silently undo the other.
    static SettingsWindow* showSingleton(const QString& status = QString());

    // The desktop entry's Settings action starts a new stud-ui; this hands the
    // request to the running one. True when handed off and this process should exit.
    static bool handOffToRunningInstance();

    static void listenForOpenRequests();

    // Explains why Stud opened this window, above Save.
    void setStatusMessage(const QString& text);

private:
    void showIdleStatus();

private slots:
    bool onSaveClicked();
    void onRestartClicked();
    void onImportFlagsClicked();
    void onImportBootstrapperClicked();
    void onBackgroundFpsChanged(int value);

protected:
    // Right-click resets the control under the pointer.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void saveWindowGeometry() const;
    void loadFromDisk();
    void installResets();
    void setBackgroundFps(int fps);

    QComboBox* gpuCombo_;
    QComboBox* presentationCombo_;
    QSpinBox* renderScaleSpin_;
    QComboBox* upscalerCombo_;
    QComboBox* rendererCombo_;
    QComboBox* titleBarCombo_;
    QComboBox* notificationsCombo_;
    QComboBox* displayBackendCombo_;
    QCheckBox* gamemodeCheck_;
    QCheckBox* mangohudCheck_;
    QLineEdit* wrapperEdit_;
    QPlainTextEdit* environmentEdit_;
    QCheckBox* trayCheck_;
    QCheckBox* playerDirectlyCheck_;
    QSlider* backgroundFpsSlider_;
    QLabel* backgroundFpsLabel_;
    QCheckBox* discordCheck_;
    QCheckBox* discordJoinCheck_;
    QCheckBox* controllersCheck_;
    QCheckBox* closeOnLeaveCheck_;
    QCheckBox* jsdelivrCheck_;
    QCheckBox* serverRegionCheck_;
    QCheckBox* historyGamesCheck_;
    QCheckBox* historyServersCheck_;
    QSpinBox* cleanerSpin_;
    void buildHistoryTab(QVBoxLayout* tab);
    void buildRobloxTab(QVBoxLayout* tab);
    void buildDebugTab(QVBoxLayout* tab);
    void buildStudioTab(QVBoxLayout* tab);
    QPlainTextEdit* flagsEdit_;
    QLabel* flagsWarning_;
    std::vector<std::function<void(const QJsonObject&)>> flagRefresh_;
    void buildFlagsTab(QVBoxLayout* presets_tab, QVBoxLayout* tab);
    QPushButton* restartButton_;
    // Recoloured on palette changes.
    void paintRestartIcon();

    QLabel* statusLabel_;

    // Reset actions keyed by widget, with defaults from a fresh StudSettings.
    QHash<QObject*, std::function<void()>> resets_;
};

}  // namespace stud::ui
