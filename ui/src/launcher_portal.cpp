#include "launcher_portal.h"

#include <QBuffer>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QImage>
#include <QObject>
#include <QRandomGenerator>
#include <QVariantMap>

namespace {

// A serialized GIcon, as the portal reads one: ("bytes", <the file's bytes>).
struct PortalIcon {
    QString kind;
    QDBusVariant data;
};

}  // namespace

Q_DECLARE_METATYPE(PortalIcon)

namespace {

QDBusArgument& operator<<(QDBusArgument& out, const PortalIcon& icon) {
    out.beginStructure();
    out << icon.kind << icon.data;
    out.endStructure();
    return out;
}

const QDBusArgument& operator>>(const QDBusArgument& in, PortalIcon& icon) {
    in.beginStructure();
    in >> icon.kind >> icon.data;
    in.endStructure();
    return in;
}

const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kInterface = QStringLiteral("org.freedesktop.portal.DynamicLauncher");

// One portal request: its Response signal, heard once.
class Request : public QObject {
    Q_OBJECT
public:
    std::function<void(uint, const QVariantMap&)> on_response;

public slots:
    void response(uint code, const QVariantMap& results) {
        if (on_response) on_response(code, results);
        deleteLater();
    }
};

}  // namespace

namespace stud::ui::launcher_portal {

QByteArray icon_png(const QByteArray& image_file) {
    QImage image = QImage::fromData(image_file);
    if (image.isNull()) return {};
    // The portal's own limit.
    if (image.width() > 512 || image.height() > 512) {
        image = image.scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return png;
}

void install(const QString& id_suffix, const QString& name, const QString& exec, const QByteArray& png,
             std::function<void(bool, const QString&)> done) {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalIcon>();
        return true;
    }();
    (void)registered;
    QDBusConnection bus = QDBusConnection::sessionBus();

    // The request's object path is known before the call, from the token
    // and this connection's name, so its Response is listened for first.
    const QString token = QStringLiteral("stud%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString request_path = kPath + QStringLiteral("/request/") + sender + QLatin1Char('/') + token;
    auto* request = new Request();
    bus.connect(kService, request_path, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                request, SLOT(response(uint, QVariantMap)));
    request->on_response = [id_suffix, exec, done](uint code, const QVariantMap& results) {
        // 1: the user declined; 2: anything else.
        if (code == 1) return done(false, QString());
        if (code != 0) return done(false, QStringLiteral("the desktop could not add it"));
        const QString kept = results.value(QStringLiteral("name")).toString();
        const QString entry = QStringLiteral("[Desktop Entry]\nType=Application\nName=%1\nComment=Play %1 with Stud\n"
                                             "Exec=%2\nCategories=Game;\nTerminal=false\n")
                                  .arg(kept, exec);
        QDBusMessage install = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("Install"));
        install << results.value(QStringLiteral("token")).toString()
                << QStringLiteral(STUD_APP_ID ".") + id_suffix + QStringLiteral(".desktop") << entry
                << QVariantMap();
        const QDBusMessage reply = QDBusConnection::sessionBus().call(install);
        if (reply.type() == QDBusMessage::ErrorMessage) return done(false, reply.errorMessage());
        done(true, kept);
    };

    QDBusMessage prepare = QDBusMessage::createMethodCall(kService, kPath, kInterface, QStringLiteral("PrepareInstall"));
    const PortalIcon icon{QStringLiteral("bytes"), QDBusVariant(png)};
    prepare << QString() << name << QVariant::fromValue(QDBusVariant(QVariant::fromValue(icon)))
            << QVariantMap{{QStringLiteral("handle_token"), token},
                           {QStringLiteral("modal"), true},
                           {QStringLiteral("launcher_type"), uint(1)},  // an application
                           {QStringLiteral("editable_name"), true},
                           {QStringLiteral("editable_icon"), false}};
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(prepare), request);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, request, [request, done](QDBusPendingCallWatcher* call) {
        call->deleteLater();
        if (!call->isError()) return;
        // No portal, or one too old for launchers: no Response follows.
        done(false, call->error().message());
        request->deleteLater();
    });
}

}  // namespace stud::ui::launcher_portal

#include "launcher_portal.moc"
