#include "cube_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include "cube.h"

CubeView::CubeView(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    timer->start(16);
    clock_.start();
}

void CubeView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && ++clicks_ == 10) {
        clicks_ = 0;
        if (on_tenth_click) on_tenth_click();
    }
    QWidget::mousePressEvent(event);
}

void CubeView::paintEvent(QPaintEvent*) {
    // Timer-driven, so it stops whenever the widget isn't visible.
    QPainter painter(this);
    draw_cube(painter, rect(), clock_.elapsed() / 1000.0);
}
