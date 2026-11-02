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

void next_line() {
    Screen& s = screen();
    if (s.lines.isEmpty() || s.text_on) return;
    // Never the same line twice in a row.
    qsizetype i = QRandomGenerator::global()->bounded(s.lines.size());
    if (s.lines.size() > 1 && i == s.last) i = (i + 1) % s.lines.size();
    s.last = i;
    s.label->setText(s.lines.at(i));
}

void raise() {
    Screen& s = screen();
    if (s.window == nullptr) {
        s.window = std::make_unique<QWidget>();
        s.window->setWindowTitle(QStringLiteral("Stud"));
        auto* layout = new QVBoxLayout(s.window.get());
        auto* stud = new CubeView(s.window.get());
        stud->setFixedSize(128, 128);
        layout->addWidget(stud, 0, Qt::AlignHCenter);
        s.label = new QLabel(s.window.get());
        s.label->setAlignment(Qt::AlignCenter);
        s.label->setWordWrap(true);
        layout->addWidget(s.label);
        // A loading bar, not a progress bar: Stud cannot know how far along
        // the engine is, so it only says something is happening.
        auto* bar = new QProgressBar(s.window.get());
        bar->setRange(0, 0);
        bar->setTextVisible(false);
        layout->addWidget(bar);
        s.cancel = new QPushButton(QStringLiteral("Cancel"), s.window.get());
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
        s.window->setFixedWidth(360);
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
    next_line();
    s.timer->start();
}

bool loading_cancelled() { return screen().cancelled; }

void show_loading_text(const QString& text) {
    Screen& s = screen();
    s.text_on = true;
    raise();
    s.label->setText(text);
    QCoreApplication::processEvents();
}

void clear_loading_text() {
    Screen& s = screen();
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
    s.timer = nullptr;
}

}  // namespace stud::ui
