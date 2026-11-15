#include "mono_icon.h"

#include <QApplication>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>

namespace stud::ui {

namespace {

class MonoIconEngine : public QIconEngine {
public:
    explicit MonoIconEngine(QIcon source) : source_(std::move(source)) {}

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pixmap = source_.pixmap(size, scale, QIcon::Normal, state);
        if (pixmap.isNull()) return pixmap;
        const QPalette palette = QApplication::palette();
        const QColor colour = mode == QIcon::Disabled
                                  ? palette.color(QPalette::Disabled, QPalette::ButtonText)
                                  : palette.color(QPalette::Active, QPalette::ButtonText);
        QPainter painter(&pixmap);
        // Keeps the glyph's own alpha, replaces its colour.
        painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        painter.fillRect(pixmap.rect(), colour);
        return pixmap;
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return source_.actualSize(size, mode, state);
    }

    QList<QSize> availableSizes(QIcon::Mode mode, QIcon::State state) override {
        return source_.availableSizes(mode, state);
    }

    bool isNull() override { return source_.isNull(); }

    QIconEngine* clone() const override { return new MonoIconEngine(source_); }

private:
    QIcon source_;
};

}  // namespace

QIcon mono_icon(const char* name, QStyle::StandardPixmap fallback) {
    const QString plain = QLatin1String(name);
    QIcon source = QIcon::fromTheme(plain + QStringLiteral("-symbolic"));
    if (source.isNull()) source = QIcon::fromTheme(plain);
    if (source.isNull()) source = QApplication::style()->standardIcon(fallback);
    if (source.isNull()) return {};
    return QIcon(new MonoIconEngine(source));
}

}  // namespace stud::ui
