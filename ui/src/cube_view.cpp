#include "cube_view.h"

#include <QMouseEvent>
#include <QOpenGLContext>
#include <QSurfaceFormat>
#include <QTimer>

#include "cube_gl.h"

CubeView::CubeView(QWidget* parent) : QOpenGLWidget(parent) {
    // Transparent around the cube: an alpha channel to clear to, and drawn
    // over the window rather than under it, which is the only way Qt shows
    // what is behind a GL widget.
    QSurfaceFormat format = this->format();
    format.setAlphaBufferSize(8);
    setFormat(format);
    setAttribute(Qt::WA_AlwaysStackOnTop);
    // Paced by a timer, not by each frame being shown: windows do not wait on
    // the display (see main()), so "draw again once shown" would spin.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    timer->start(16);
}

CubeView::~CubeView() {
    makeCurrent();
    cube_gl_destroy(state_);
    doneCurrent();
}

void CubeView::initializeGL() {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    cube_gl_destroy(state_);
    state_ = context == nullptr ? nullptr : cube_gl_create([](const char* name, void* ctx) {
                 return reinterpret_cast<void*>(
                     static_cast<QOpenGLContext*>(ctx)->getProcAddress(name));
             }, context);
    clock_.start();
}

void CubeView::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && ++clicks_ == 10) {
        clicks_ = 0;
        if (on_tenth_click) on_tenth_click();
    }
    QOpenGLWidget::mousePressEvent(event);
}

void CubeView::paintGL() {
    if (state_ == nullptr) return;
    const qreal ratio = devicePixelRatioF();
    cube_gl_draw(state_, static_cast<float>(clock_.elapsed()) / 1000.0f,
                 static_cast<int>(width() * ratio), static_cast<int>(height() * ratio));
}
