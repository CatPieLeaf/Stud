#include "cube_view.h"

#include <QMouseEvent>
#include <QPainter>

#include "cube.h"

CubeView::CubeView(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TranslucentBackground);
    frames_.setInterval(16);
    connect(&frames_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    clock_.start();
}

// Frames only while it can be seen: a hidden Settings tab or a closed window costs nothing.
void CubeView::showEvent(QShowEvent* event) {
    frames_.start();
    QWidget::showEvent(event);
}

void CubeView::hideEvent(QHideEvent* event) {
    frames_.stop();
    QWidget::hideEvent(event);
}

void CubeView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && ++clicks_ == 10) {
        clicks_ = 0;
        if (on_tenth_click) on_tenth_click();
    }
    QWidget::mousePressEvent(event);
}

void CubeView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    draw_cube(painter, rect(), clock_.elapsed() / 1000.0);
}
