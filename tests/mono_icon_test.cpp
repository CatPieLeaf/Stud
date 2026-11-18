// Every visible pixel of a button icon is the palette's button-text colour,
// in a dark scheme and a light one, and the disabled colour on a disabled
// button: no colour of the theme's own survives.

#include <QApplication>
#include <QImage>
#include <QPalette>
#include <QPixmap>
#include <cstdio>
#include <cstdlib>

#include "mono_icon.h"

namespace {

int failures = 0;

void expect_only(const QIcon& icon, QIcon::Mode mode, QColor colour, const char* what) {
    // Premultiplied, so soft edges are checked exactly too.
    const QImage image = icon.pixmap(QSize(32, 32), mode).toImage().convertToFormat(
        QImage::Format_ARGB32_Premultiplied);
    int visible = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = image.pixel(x, y);
            const int alpha = qAlpha(pixel);
            if (alpha == 0) continue;
            ++visible;
            auto off = [alpha](int got, int want) { return std::abs(got - want * alpha / 255) > 1; };
            if (off(qRed(pixel), colour.red()) || off(qGreen(pixel), colour.green()) ||
                off(qBlue(pixel), colour.blue())) {
                std::fprintf(stderr, "%s: pixel %d,%d is #%08x premultiplied, not %s at alpha %d\n",
                             what, x, y, pixel, qPrintable(colour.name()), alpha);
                ++failures;
                return;
            }
        }
    }
    if (visible == 0) {
        std::fprintf(stderr, "%s: the icon draws nothing\n", what);
        ++failures;
    }
}

void check_scheme(const QColor& text, const QColor& disabled, const char* scheme) {
    QPalette palette = QApplication::palette();
    palette.setColor(QPalette::Active, QPalette::ButtonText, text);
    palette.setColor(QPalette::Inactive, QPalette::ButtonText, text);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    QApplication::setPalette(palette);
    // Qt's own glyphs, which are in colour: what a missing theme falls back to.
    const QIcon icon = stud::ui::mono_icon("stud-no-such-icon", QStyle::SP_DialogSaveButton);
    const QByteArray normal = QByteArray(scheme) + " normal";
    const QByteArray off = QByteArray(scheme) + " disabled";
    expect_only(icon, QIcon::Normal, text, normal.constData());
    expect_only(icon, QIcon::Disabled, disabled, off.constData());
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    check_scheme(QColor(0xfc, 0xfc, 0xfc), QColor(0x70, 0x70, 0x70), "dark");
    check_scheme(QColor(0x23, 0x26, 0x29), QColor(0xa0, 0xa0, 0xa0), "light");
    if (failures == 0) std::printf("mono icons: ok\n");
    return failures == 0 ? 0 : 1;
}
