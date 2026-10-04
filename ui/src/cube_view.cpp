#include "cube_view.h"

#include <QOpenGLContext>
#include <QSurfaceFormat>

#include "cube_gl.h"

CubeView::CubeView(QWidget* parent) : QOpenGLWidget(parent) {
    // Transparent around the cube: an alpha channel to clear to, and drawn
    // over the window rather than under it, which is the only way Qt shows
    // what is behind a GL widget.
    QSurfaceFormat format = this->format();
    format.setAlphaBufferSize(8);
    setFormat(format);
    setAttribute(Qt::WA_AlwaysStackOnTop);
    connect(this, &QOpenGLWidget::frameSwapped, this, [this] { update(); });
}

void CubeView::initializeGL() {
    QOpenGLContext* context = QOpenGLContext::currentContext();
    ready_ = context != nullptr && cube_gl_init([](const char* name, void* ctx) {
                 return reinterpret_cast<void*>(
                     static_cast<QOpenGLContext*>(ctx)->getProcAddress(name));
             }, context);
    clock_.start();
}

void CubeView::paintGL() {
    if (!ready_) return;
    const qreal ratio = devicePixelRatioF();
    cube_gl_draw(static_cast<float>(clock_.elapsed()) / 1000.0f,
                 static_cast<int>(width() * ratio), static_cast<int>(height() * ratio));
}
