#include "loading_screen.h"

#include "cube_view.h"

#include <QCoreApplication>
#include <QFile>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <functional>
#include <memory>

namespace stud::ui {
namespace {

struct Screen {
    std::unique_ptr<QWidget> window;
    QLabel* label = nullptr;
    QPushButton* cancel = nullptr;
    QProgressBar* bar = nullptr;
    QTimer* timer = nullptr;
    QStringList lines;
    qsizetype last = -1;
    bool lines_on = false;
    bool text_on = false;
    bool cancelled = false;
    std::function<void()> on_cancel;
};

Screen& screen() {
    static Screen s;
    return s;
}

// The window's width, as it always was; its height is what the contents need
// at that width, so lines wrap rather than widen it, and it cannot be resized.
constexpr int kWidth = 360;

void fit() {
    Screen& s = screen();
    if (s.window == nullptr) return;
    s.window->setFixedSize(kWidth, s.window->layout()->totalHeightForWidth(kWidth));
}

void next_line() {
    Screen& s = screen();
    if (s.lines.isEmpty() || s.text_on) return;
    // Never the same line twice in a row.
    qsizetype i = QRandomGenerator::global()->bounded(s.lines.size());
    if (s.lines.size() > 1 && i == s.last) i = (i + 1) % s.lines.size();
    s.last = i;
    s.label->setText(s.lines.at(i));
    fit();
}

void raise() {
    Screen& s = screen();
    if (s.window == nullptr) {
        // A dialog of a fixed size: tiling window managers float both.
        s.window = std::make_unique<QWidget>(nullptr, Qt::Dialog);
        s.window->setWindowTitle(QStringLiteral("Stud"));
        auto* layout = new QVBoxLayout(s.window.get());
        auto* stud = new CubeView(s.window.get());
        stud->setFixedSize(128, 128);
        layout->addWidget(stud, 0, Qt::AlignHCenter);
        s.label = new QLabel(s.window.get());
        s.label->setAlignment(Qt::AlignCenter);
        s.label->setWordWrap(true);
        layout->addWidget(s.label);
        // A moving bar while nobody knows how far along it is; a progress bar
        // while something says (a download, the unpacking).
        s.bar = new QProgressBar(s.window.get());
        s.bar->setRange(0, 0);
        s.bar->setTextVisible(false);
        layout->addWidget(s.bar);
        s.cancel = new QPushButton(QCoreApplication::translate("stud::ui::LoadingScreen", "Cancel"), s.window.get());
        s.cancel->hide();
        QObject::connect(s.cancel, &QPushButton::clicked, s.window.get(), []() {
            Screen& screen_ = screen();
            screen_.cancelled = true;
            // Taken before the window goes: closing it destroys this button.
            std::function<void()> on_cancel = std::move(screen_.on_cancel);
            QTimer::singleShot(0, qApp, [on_cancel]() {
                close_loading_screen();
                if (on_cancel) on_cancel();
            });
        });
        layout->addWidget(s.cancel, 0, Qt::AlignHCenter);
        fit();
        s.timer = new QTimer(s.window.get());
        s.timer->setInterval(2000);
        QObject::connect(s.timer, &QTimer::timeout, s.window.get(), next_line);
    }
    s.window->show();
    s.window->raise();
    // Painted now: what follows may not return to an event loop for a
    // moment (unpacking, building, starting the other processes).
    QCoreApplication::processEvents();
}

}  // namespace

void show_loading_lines(std::function<void()> on_cancel) {
    Screen& s = screen();
    s.on_cancel = std::move(on_cancel);
    if (s.lines.isEmpty()) {
        QFile file(QStringLiteral(":/loading_lines.txt"));
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            for (const QString& line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
                if (!line.trimmed().isEmpty()) s.lines.append(line.trimmed());
            }
        }
    }
    s.lines_on = true;
    raise();
    s.cancel->setVisible(s.on_cancel != nullptr);
    fit();
    next_line();
    s.timer->start();
}

bool loading_cancelled() { return screen().cancelled; }

void show_loading_text(const QString& text) {
    Screen& s = screen();
    s.text_on = true;
    raise();
    s.label->setText(text);
    fit();
    QCoreApplication::processEvents();
}

void set_loading_progress(int percent) {
    Screen& s = screen();
    if (s.bar == nullptr) return;
    if (percent < 0) {
        s.bar->setRange(0, 0);
    } else {
        s.bar->setRange(0, 100);
        s.bar->setValue(percent);
    }
}

void clear_loading_text() {
    Screen& s = screen();
    set_loading_progress(-1);
    if (!s.text_on) return;
    s.text_on = false;
    if (s.lines_on) {
        next_line();
    } else if (s.window != nullptr) {
        s.window->hide();
    }
}

void close_loading_screen() {
    Screen& s = screen();
    s.lines_on = false;
    s.text_on = false;
    s.window.reset();
    s.label = nullptr;
    s.cancel = nullptr;
    s.bar = nullptr;
    s.timer = nullptr;
}

}  // namespace stud::ui
