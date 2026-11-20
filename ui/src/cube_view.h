#pragma once

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

#include <functional>

// The spinning stud cube, on the About tab and the loading screen.
class CubeView : public QWidget {
public:
    explicit CubeView(QWidget* parent = nullptr);

    // Called on every tenth click.
    std::function<void()> on_tenth_click;

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QTimer frames_;
    QElapsedTimer clock_;
    int clicks_ = 0;
};
