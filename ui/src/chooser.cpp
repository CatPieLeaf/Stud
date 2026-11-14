#include "chooser.h"

#include "cube_gl.h"
#include "cube_view.h"
#include "gpu_enum.h"
#include "settings_window.h"
#include "update_check.h"
#include "stud/settings.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QRandomGenerator>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QSurfaceFormat>
#include <QOpenGLFramebufferObject>
#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QPushButton>
#include <QHash>
#include <QScrollBar>
#include <QSet>
#include <QTextBrowser>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

namespace stud::ui {

namespace {

// Stud's own studs, not Roblox's icons: red for Roblox, blue for Studio.
QIcon icon_of(client::Product product) {
    return QIcon(product == client::Product::Studio ? QStringLiteral(":/studblu.png")
                                                    : QStringLiteral(":/studred.png"));
}

// The Stud title swaying a few degrees either way, never turning over, with
// the spinning stud fixed to it in front, and now and then a white shine
// across the title. The stud is drawn offscreen in a GL context of its own
// and painted into the same picture, so it swings with the title smoothly,
// at no whole-pixel steps.
class Emblem : public QWidget {
public:
    Emblem(int cube, int title, int overlap, QWidget* parent) : QWidget(parent), cube_size_(cube) {
        // One scale for everything this paints: the images are made at it
        // and drawn at it, so they keep their size whatever the screen says
        // later.
        ratio_ = devicePixelRatioF();
        const qreal ratio = ratio_;
        logo_ = QPixmap(QStringLiteral(":/stud-title.png"))
                    .scaledToWidth(int(title * ratio), Qt::SmoothTransformation)
                    .toImage()
                    .convertToFormat(QImage::Format_ARGB32_Premultiplied);
        logo_.setDevicePixelRatio(ratio);
        const QSizeF size = logo_.deviceIndependentSize();

        // Where the stud's centre sits from the title's, before any sway.
        anchor_ = QPointF(-(size.width() / 2.0 + cube / 2.0 - overlap), 0.0);
        // Room for the title's corners and the stud's travel as they tilt.
        const double tilt = kSwayDegrees * std::numbers::pi / 180.0;
        const double title_w = size.width() * std::cos(tilt) + size.height() * std::sin(tilt);
        const double title_h = size.width() * std::sin(tilt) + size.height() * std::cos(tilt);
        const double travel = std::abs(anchor_.x()) * std::sin(tilt);
        const int width = int(cube - overlap + title_w) + 2;
        const int height = int(std::max(cube + 2 * travel, title_h)) + 2;
        setFixedSize(width, height);
        pivot_ = QPointF(width - title_w / 2.0 - 1, height / 2.0);

        // The stud's own context, offscreen, and the image it renders into.
        QSurfaceFormat format;
        format.setAlphaBufferSize(8);
        format.setDepthBufferSize(24);
        surface_.setFormat(format);
        surface_.create();
        context_.setFormat(format);
        if (context_.create() && context_.makeCurrent(&surface_)) {
            fbo_ = std::make_unique<QOpenGLFramebufferObject>(
                QSize(int(cube * ratio), int(cube * ratio)), QOpenGLFramebufferObject::CombinedDepthStencil);
            cube_state_ = cube_gl_create([](const char* name, void* ctx) {
                return reinterpret_cast<void*>(static_cast<QOpenGLContext*>(ctx)->getProcAddress(name));
            }, &context_);
            context_.doneCurrent();
        }

        clock_.start();
        next_shine_ = next_shine_after(0);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { step(); });
        timer->start(16);
        step();
    }

public:
    ~Emblem() override {
        if (cube_state_ != nullptr && context_.makeCurrent(&surface_)) {
            cube_gl_destroy(cube_state_);
            fbo_.reset();
            context_.doneCurrent();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QImage frame = logo_;
        const double t = seconds() - shine_start_;
        if (shining_ && t >= 0.0 && t <= kShineSeconds) {
            // A white band with soft edges, kept to the title's own pixels and
            // screened over them, sweeping from its top right to its bottom left.
            const QSizeF size = logo_.deviceIndependentSize();
            QImage band(frame.size(), QImage::Format_ARGB32_Premultiplied);
            band.setDevicePixelRatio(frame.devicePixelRatio());
            band.fill(Qt::transparent);
            {
                QPainter shine(&band);
                shine.setRenderHint(QPainter::Antialiasing);
                const double travel = size.width() + size.height();
                const double along = t / kShineSeconds * (travel + 2 * size.height()) - size.height();
                // The band runs top left to bottom right; it moves down and left.
                const QPointF centre(size.width() - along * size.width() / travel,
                                     along * size.height() / travel);
                const QPointF dir(size.height(), size.height());
                // Wide and faint, then narrow and bright: soft edges.
                for (const auto& [width, alpha] : {std::pair{kShineWidth, 70}, std::pair{kShineWidth * 0.6, 140},
                                                   std::pair{kShineWidth * 0.3, 230}}) {
                    shine.setPen(QPen(QColor(255, 255, 255, alpha), width, Qt::SolidLine, Qt::FlatCap));
                    shine.drawLine(centre - dir, centre + dir);
                }
                shine.setCompositionMode(QPainter::CompositionMode_DestinationIn);
                shine.drawImage(QPointF(0, 0), logo_);
            }
            QPainter blend(&frame);
            blend.setCompositionMode(QPainter::CompositionMode_Screen);
            blend.drawImage(QPointF(0, 0), band);
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(pivot_);
        painter.rotate(angle_);
        const QSizeF size = logo_.deviceIndependentSize();
        painter.drawImage(QPointF(-size.width() / 2.0, -size.height() / 2.0), frame);
        // The stud in front, carried by the same turn, at its anchor.
        if (!cube_image_.isNull()) {
            painter.drawImage(QPointF(anchor_.x() - cube_size_ / 2.0, anchor_.y() - cube_size_ / 2.0),
                              cube_image_);
        }
    }

private:
    static constexpr double kSwayDegrees = 3.0;
    static constexpr double kPeriodSeconds = 4.0;
    static constexpr double kShineSeconds = 0.8;
    static constexpr double kShineWidth = 10.0;
    // A shine every few seconds, never on a beat.
    static constexpr int kShineEveryMinMs = 4000;
    static constexpr int kShineEveryMaxMs = 10000;

    double seconds() const { return double(clock_.elapsed()) / 1000.0; }
    double next_shine_after(double now) const {
        return now + QRandomGenerator::global()->bounded(kShineEveryMinMs, kShineEveryMaxMs) / 1000.0;
    }

    void step() {
        const double now = seconds();
        angle_ = kSwayDegrees * std::sin(2.0 * std::numbers::pi * now / kPeriodSeconds);
        if (!shining_ && now >= next_shine_) {
            shining_ = true;
            shine_start_ = now;
        } else if (shining_ && now - shine_start_ > kShineSeconds) {
            shining_ = false;
            next_shine_ = next_shine_after(now);
        }
        // This frame of the stud, offscreen.
        if (cube_state_ != nullptr && context_.makeCurrent(&surface_)) {
            fbo_->bind();
            cube_gl_draw(cube_state_, float(now), fbo_->width(), fbo_->height());
            cube_image_ = fbo_->toImage();
            cube_image_.setDevicePixelRatio(ratio_);
            fbo_->release();
            context_.doneCurrent();
        }
        update();
    }

    QImage logo_;
    QOffscreenSurface surface_;
    QOpenGLContext context_;
    std::unique_ptr<QOpenGLFramebufferObject> fbo_;
    CubeGlState* cube_state_ = nullptr;
    QImage cube_image_;
    qreal ratio_ = 1.0;
    int cube_size_;
    QPointF anchor_;
    QPointF pivot_;
    QElapsedTimer clock_;
    double angle_ = 0.0;
    bool shining_ = false;
    double shine_start_ = 0.0;
    double next_shine_ = 0.0;
};

// Release notes as GitHub renders them. A text browser shows no image it
// cannot read from disk, so this one downloads each picture the notes name
// and lays the notes out again once it is in.
class NotesView : public QTextBrowser {
public:
    using QTextBrowser::QTextBrowser;

    void show_html(const QString& html) {
        html_ = html;
        const int scroll = verticalScrollBar()->value();
        setHtml(html_);
        verticalScrollBar()->setValue(scroll);
    }

protected:
    QVariant loadResource(int type, const QUrl& url) override {
        if (type != QTextDocument::ImageResource || !url.scheme().startsWith(QStringLiteral("http")))
            return QTextBrowser::loadResource(type, url);
        if (images_.contains(url)) return images_.value(url);
        if (!requested_.contains(url)) {
            requested_.insert(url);
            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Stud/" STUD_VERSION));
            QNetworkReply* reply = net_.get(request);
            QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
                reply->deleteLater();
                QImage image;
                if (reply->error() != QNetworkReply::NoError || !image.loadFromData(reply->readAll())) return;
                // Never wider than the notes themselves.
                const int room = viewport()->width() - 2 * int(document()->documentMargin());
                if (room > 0 && image.width() > room) image = image.scaledToWidth(room, Qt::SmoothTransformation);
                images_.insert(url, image);
                show_html(html_);
            });
        }
        return {};
    }

private:
    QNetworkAccessManager net_;
    QString html_;
    QHash<QUrl, QImage> images_;
    QSet<QUrl> requested_;
};

// The notes of the release this Stud is, from GitHub, in a window of their own.
void show_release_notes(QWidget* parent) {
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Stud %1").arg(QStringLiteral(STUD_VERSION)));
    auto* layout = new QVBoxLayout(dialog);
    auto* notes = new NotesView(dialog);
    notes->setOpenExternalLinks(true);
    notes->setText(QStringLiteral("Loading the release notes..."));
    layout->addWidget(notes);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(close, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(close);
    dialog->resize(dialog->sizeHint().width() * 2, dialog->sizeHint().height() * 2);
    dialog->show();

    auto* net = new QNetworkAccessManager(dialog);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/CatPieLeaf/Stud/releases/tags/%1")
                                     .arg(QStringLiteral(STUD_VERSION))));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Stud/" STUD_VERSION));
    // The notes as GitHub itself renders them, HTML and all.
    request.setRawHeader("Accept", "application/vnd.github.html+json");
    request.setTransferTimeout(10000);
    QNetworkReply* reply = net->get(request);
    QObject::connect(reply, &QNetworkReply::finished, notes, [reply, notes] {
        reply->deleteLater();
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString body = release.value(QStringLiteral("body_html")).toString();
        if (reply->error() != QNetworkReply::NoError || body.isEmpty()) {
            notes->setText(QStringLiteral("There are no release notes for Stud %1 on GitHub.")
                               .arg(QStringLiteral(STUD_VERSION)));
            return;
        }
        const QString name = release.value(QStringLiteral("name")).toString();
        notes->show_html((name.isEmpty() ? QString() : QStringLiteral("<h1>%1</h1>").arg(name.toHtmlEscaped())) +
                         body);
    });
}

}  // namespace

std::optional<client::Product> choose_product() {
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Stud"));
    auto* layout = new QVBoxLayout(&dialog);
    // As large as its contents and no larger, like the loading window.
    layout->setSizeConstraint(QLayout::SetFixedSize);

    // The spinning stud in front, the title tucked in behind its right side,
    // swaying together.
    constexpr int kCube = 96;
    constexpr int kTitle = 160;
    constexpr int kOverlap = 18;
    // Room kept between the title and the version.
    constexpr int kGap = 40;
    auto* header = new Emblem(kCube, kTitle, kOverlap, &dialog);

    // Who, which version, and what is new in it: right beside the title.
    auto* about = new QVBoxLayout();
    about->addStretch();
    auto* by = new QLabel(QStringLiteral("By CatPieLeaf"), &dialog);
    auto* version = new QLabel(QStringLiteral("Version %1").arg(QStringLiteral(STUD_VERSION)), &dialog);
    by->setAlignment(Qt::AlignRight);
    version->setAlignment(Qt::AlignRight);
    // A newer Stud out: the version turns red, underlined, and opens the
    // latest release.
    const auto mark_outdated = [version] {
        version->setTextFormat(Qt::RichText);
        version->setOpenExternalLinks(true);
        version->setText(QStringLiteral("<a href=\"%1\" style=\"color:#d62f2f; text-decoration:underline;\">"
                                        "Version %2</a>")
                             .arg(UpdateCheck::releasesUrl(), QStringLiteral(STUD_VERSION)));
        version->setToolTip(QStringLiteral("Stud %1 is available").arg(UpdateCheck::latestVersion()));
    };
    QObject::connect(UpdateCheck::instance(), &UpdateCheck::updateFound, version,
                     [mark_outdated](const QString&) { mark_outdated(); });
    if (UpdateCheck::updateAvailable()) mark_outdated();
    UpdateCheck::start();
    about->addWidget(by, 0, Qt::AlignRight);
    about->addWidget(version, 0, Qt::AlignRight);
    auto* notes = new QPushButton(QStringLiteral("Release notes"), &dialog);
    notes->setIcon(QIcon::fromTheme(QStringLiteral("help-about"),
                                    dialog.style()->standardIcon(QStyle::SP_MessageBoxInformation)));
    QObject::connect(notes, &QPushButton::clicked, &dialog, [&dialog] { show_release_notes(&dialog); });
    about->addWidget(notes, 0, Qt::AlignRight);
    about->addStretch();

    auto* top = new QHBoxLayout();
    top->addWidget(header, 0, Qt::AlignLeft | Qt::AlignVCenter);
    top->addSpacing(kGap);
    top->addLayout(about);
    layout->addLayout(top);

    // A fun fact about Stud, a different one each time the page opens.
    QStringList quotes;
    QFile file(QStringLiteral(":/quotes.txt"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const QString& line : QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'))) {
            if (!line.trimmed().isEmpty()) quotes.append(line.trimmed());
        }
    }
    // One more on the GPU Stud runs on being NVIDIA's, as the About cube's.
    {
        stud::config::StudSettings settings;
        try {
            settings = stud::config::load_settings(stud::config::default_config_path());
        } catch (const stud::config::SettingsError&) {
        }
        const auto chosen = chosen_gpu_index(settings.gpu);
        constexpr uint32_t kNvidia = 0x10DE;
        for (const auto& gpu : enumerate_gpus()) {
            if (gpu.device_index == chosen && gpu.vendor_id == kNvidia) {
                quotes.append(QStringLiteral("This GPU model may experience stability due to a decent Vulkan "
                                             "memory allocator in its driver. It's recommended you use Vulkan "
                                             "if you experience stability."));
            }
        }
    }
    if (!quotes.isEmpty()) {
        // Italic in the text itself, so no style can set it upright.
        auto* quote = new QLabel(
            QStringLiteral("<i>%1</i>").arg(quotes.at(QRandomGenerator::global()->bounded(quotes.size())).toHtmlEscaped()),
            &dialog);
        quote->setTextFormat(Qt::RichText);
        quote->setWordWrap(true);
        quote->setAlignment(Qt::AlignCenter);
        // As wide as the header row, so a long line wraps instead of widening it.
        quote->setFixedWidth(top->sizeHint().width());
        layout->addWidget(quote, 0, Qt::AlignHCenter);
    }
    layout->addSpacing(dialog.fontMetrics().height());

    // Roblox Studio and Roblox, side by side.
    std::optional<client::Product> chosen;
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(dialog.fontMetrics().height() * 2);
    buttons->addStretch();
    const int icon = dialog.fontMetrics().height() * 2;
    for (const client::Product product : {client::Product::Studio, client::Product::Player}) {
        auto* button = new QToolButton(&dialog);
        button->setText(client::display_name(product));
        button->setIcon(icon_of(product));
        button->setIconSize(QSize(icon, icon));
        button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        QObject::connect(button, &QToolButton::clicked, &dialog, [&chosen, &dialog, product] {
            chosen = product;
            dialog.accept();
        });
        buttons->addWidget(button);
    }
    buttons->addStretch();
    layout->addLayout(buttons);

    auto* settings = new QPushButton(QStringLiteral("Settings"), &dialog);
    settings->setIcon(QIcon::fromTheme(QStringLiteral("configure"),
                                       dialog.style()->standardIcon(QStyle::SP_FileDialogDetailedView)));
    QObject::connect(settings, &QPushButton::clicked, &dialog, [] { SettingsWindow::showSingleton(); });
    layout->addWidget(settings, 0, Qt::AlignHCenter);

    // Not modal: Settings, opened from here, takes input beside it.
    dialog.setWindowModality(Qt::NonModal);
    QEventLoop loop;
    QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    dialog.show();
    loop.exec();
    return chosen;
}

}  // namespace stud::ui
