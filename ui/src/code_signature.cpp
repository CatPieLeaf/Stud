#include "code_signature.h"

#include "apple_root_ca.h"  // kAppleRootCa: Apple Root CA, DER, generated at build

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QVariant>
#include <QXmlStreamReader>

#include <openssl/cms.h>
#include <openssl/objects.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <map>
#include <memory>
#include <optional>

namespace stud::ui::codesign {
namespace {

constexpr uint32_t kCpuX86_64 = 0x01000007;
constexpr uint32_t kLcCodeSignature = 0x1d;
constexpr uint32_t kSuperblob = 0xfade0cc0;
constexpr uint32_t kCodeDirectory = 0xfade0c02;
constexpr uint32_t kSlotInfo = 1;
constexpr uint32_t kSlotRequirements = 2;
constexpr uint32_t kSlotResources = 3;
constexpr uint32_t kSlotEntitlements = 5;
constexpr uint32_t kSlotDerEntitlements = 7;
constexpr uint32_t kSlotSignature = 0x10000;
// Apple's markers: the Developer ID intermediate authority, and a Developer ID
// Application certificate.
constexpr const char* kDeveloperIdCa = "1.2.840.113635.100.6.2.6";
constexpr const char* kDeveloperIdApp = "1.2.840.113635.100.6.1.13";
// The FastFlags file Stud writes for the client (client::write_app_settings),
// relative to the bundle's Contents.
constexpr const char* kSettingsFile = "MacOS/ClientSettings/ClientAppSettings.json";
// Nested bundles inside nested bundles, at most: Roblox ships two levels.
constexpr int kMaxDepth = 4;

uint32_t be32(const QByteArray& b, qsizetype at) {
    const auto* p = reinterpret_cast<const unsigned char*>(b.constData() + at);
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint32_t le32(const QByteArray& b, qsizetype at) {
    const auto* p = reinterpret_cast<const unsigned char*>(b.constData() + at);
    return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
}
QByteArray sha256(const QByteArray& b) { return QCryptographicHash::hash(b, QCryptographicHash::Sha256); }

std::optional<QByteArray> sha256_of_file(const QString& path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return std::nullopt;
    return hash.result();
}

// One executable's signature, read from its x86_64 code.
struct Code {
    QByteArray directory;                // the SHA-256 code directory
    std::map<uint32_t, QByteArray> blobs;  // the superblob's other blobs, by slot
    QString identifier;
    QString team;
    // The code-directory hash of every architecture in the file, this one's
    // included: a seal pins a universal binary by whichever it recorded.
    QList<QByteArray> cdhashes;

    QByteArray cdhash() const { return sha256(directory).left(20); }
    // A special slot's hash; empty when the slot holds nothing.
    QByteArray special(uint32_t slot) const {
        const uint32_t hash_offset = be32(directory, 16);
        const uint32_t count = be32(directory, 24);
        if (slot > count) return {};
        const QByteArray hash = directory.mid(hash_offset - slot * 32, 32);
        return hash == QByteArray(32, '\0') ? QByteArray() : hash;
    }
};

// The architectures of a Mach-O file, thin or universal, each with its code.
std::optional<QList<std::pair<uint32_t, QByteArray>>> slices(const QByteArray& bytes, QString* error) {
    if (bytes.size() < 32) {
        *error = QStringLiteral("not an executable");
        return std::nullopt;
    }
    if (le32(bytes, 0) == 0xfeedfacf) return QList<std::pair<uint32_t, QByteArray>>{{le32(bytes, 4), bytes}};
    const uint32_t magic = be32(bytes, 0);
    if (magic != 0xcafebabe && magic != 0xcafebabf) {
        *error = QStringLiteral("not an executable");
        return std::nullopt;
    }
    const bool wide = magic == 0xcafebabf;
    const uint32_t count = be32(bytes, 4);
    const qsizetype entry = wide ? 32 : 20;
    QList<std::pair<uint32_t, QByteArray>> found;
    for (uint32_t i = 0; i < count; ++i) {
        const qsizetype at = 8 + i * entry;
        if (at + entry > bytes.size()) break;
        const quint64 offset = wide ? (quint64(be32(bytes, at + 8)) << 32 | be32(bytes, at + 12)) : be32(bytes, at + 8);
        const quint64 size = wide ? (quint64(be32(bytes, at + 16)) << 32 | be32(bytes, at + 20)) : be32(bytes, at + 12);
        if (offset + size > quint64(bytes.size())) {
            *error = QStringLiteral("truncated");
            return std::nullopt;
        }
        found.append({be32(bytes, at), bytes.mid(qsizetype(offset), qsizetype(size))});
    }
    return found;
}

// The SHA-256 code directory hash of a slice not run here, read without
// checking its pages; empty when it has none.
QByteArray other_cdhash(const QByteArray& slice) {
    const uint32_t ncmds = slice.size() >= 32 ? le32(slice, 16) : 0;
    qsizetype at = 32;
    for (uint32_t i = 0; i < ncmds && at + 16 <= slice.size(); ++i) {
        const uint32_t cmd = le32(slice, at), size = le32(slice, at + 4);
        if (size < 8) return {};
        if (cmd == kLcCodeSignature) {
            const uint32_t offset = le32(slice, at + 8);
            if (quint64(offset) + 12 > quint64(slice.size()) || be32(slice, offset) != kSuperblob) return {};
            const uint32_t count = be32(slice, offset + 8);
            for (uint32_t j = 0; j < count; ++j) {
                const qsizetype index = offset + 12 + j * 8;
                if (index + 8 > slice.size()) return {};
                if (be32(slice, index) != 0) continue;
                const qsizetype blob = offset + be32(slice, index + 4);
                if (blob + 40 > slice.size() || be32(slice, blob) != kCodeDirectory || uint8_t(slice[blob + 37]) != 2) return {};
                return sha256(slice.mid(blob, be32(slice, blob + 4))).left(20);
            }
            return {};
        }
        at += size;
    }
    return {};
}

// Read `slice`'s signature and check every page of its code against its code
// directory.
std::optional<Code> read_code(const QByteArray& slice, QString* error) {
    const auto fail = [&](const QString& why) {
        *error = why;
        return std::nullopt;
    };
    const uint32_t ncmds = le32(slice, 16);
    qsizetype at = 32;
    std::optional<std::pair<uint32_t, uint32_t>> signature;
    for (uint32_t i = 0; i < ncmds && at + 8 <= slice.size(); ++i) {
        const uint32_t cmd = le32(slice, at), size = le32(slice, at + 4);
        if (size < 8) return fail(QStringLiteral("bad load commands"));
        if (cmd == kLcCodeSignature && at + 16 <= slice.size()) signature = {le32(slice, at + 8), le32(slice, at + 12)};
        at += size;
    }
    if (!signature) return fail(QStringLiteral("not signed"));
    const auto [offset, size] = *signature;
    if (quint64(offset) + size > quint64(slice.size()) || size < 12) return fail(QStringLiteral("bad signature"));
    const QByteArray super = slice.mid(offset, size);
    if (be32(super, 0) != kSuperblob) return fail(QStringLiteral("bad signature"));
    Code code;
    const uint32_t count = be32(super, 8);
    for (uint32_t i = 0; i < count && 12 + i * 8 + 8 <= uint32_t(super.size()); ++i) {
        const uint32_t type = be32(super, 12 + i * 8), blob_at = be32(super, 16 + i * 8);
        if (blob_at + 8 > uint32_t(super.size())) return fail(QStringLiteral("bad signature"));
        const uint32_t length = be32(super, blob_at + 4);
        if (length < 8 || blob_at + length > uint32_t(super.size())) return fail(QStringLiteral("bad signature"));
        code.blobs[type] = super.mid(blob_at, length);
    }
    const auto directory = code.blobs.find(0);
    if (directory == code.blobs.end()) return fail(QStringLiteral("no code directory"));
    code.directory = directory->second;
    code.blobs.erase(directory);
    const QByteArray& cd = code.directory;
    if (cd.size() < 44 || be32(cd, 0) != kCodeDirectory) return fail(QStringLiteral("bad code directory"));
    const uint32_t version = be32(cd, 8);
    const uint32_t hash_offset = be32(cd, 16), code_slots = be32(cd, 28), code_limit = be32(cd, 32);
    const auto hash_size = uint8_t(cd[36]), hash_type = uint8_t(cd[37]), page_shift = uint8_t(cd[39]);
    if (hash_type != 2 || hash_size != 32 || page_shift < 12 || page_shift > 16) {
        return fail(QStringLiteral("code directory is not SHA-256 with pages"));
    }
    if (version >= 0x20100 && cd.size() >= 48 && be32(cd, 44) != 0) return fail(QStringLiteral("scattered code directory"));
    if (version >= 0x20300 && cd.size() >= 64 && cd.mid(56, 8) != QByteArray(8, '\0')) {
        return fail(QStringLiteral("64-bit code limit"));
    }
    if (code_limit != offset) return fail(QStringLiteral("code directory does not cover the whole executable"));
    const uint32_t page = 1u << page_shift;
    if (code_slots != (code_limit + page - 1) / page) return fail(QStringLiteral("code directory has the wrong number of pages"));
    if (quint64(hash_offset) + quint64(code_slots) * 32 > quint64(cd.size())) return fail(QStringLiteral("bad code directory"));
    for (uint32_t index = 0; index < code_slots; ++index) {
        const uint32_t start = index * page;
        const uint32_t end = std::min(start + page, code_limit);
        if (sha256(slice.mid(start, end - start)) != cd.mid(hash_offset + index * 32, 32)) {
            return fail(QStringLiteral("code page %1 was changed").arg(index));
        }
    }
    const auto string_at = [&](uint32_t at) {
        return at == 0 || at >= uint32_t(cd.size()) ? QString() : QString::fromUtf8(cd.constData() + at);
    };
    code.identifier = string_at(be32(cd, 20));
    if (version >= 0x20200 && cd.size() >= 52) code.team = string_at(be32(cd, 48));
    for (const uint32_t slot : {kSlotRequirements, kSlotEntitlements, kSlotDerEntitlements}) {
        const auto blob = code.blobs.find(slot);
        const QByteArray sealed = code.special(slot);
        const bool matches = blob == code.blobs.end() ? sealed.isEmpty() : sealed == sha256(blob->second);
        if (!matches) return fail(QStringLiteral("signature slot %1 was changed").arg(slot));
    }
    for (const uint32_t slot : {4u, 6u}) {
        if (!code.special(slot).isEmpty()) return fail(QStringLiteral("unknown sealed slot %1").arg(slot));
    }
    if (code.team != QLatin1String(kRobloxTeam)) {
        return fail(QStringLiteral("signed by team \"%1\", not Roblox").arg(code.team));
    }
    return code;
}

std::optional<Code> read_code_file(const QString& path, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read it");
        return std::nullopt;
    }
    const auto all = slices(file.readAll(), error);
    if (!all) return std::nullopt;
    std::optional<Code> x86_64;
    QList<QByteArray> cdhashes;
    for (const auto& [cpu, slice] : *all) {
        if (cpu == kCpuX86_64) {
            x86_64 = read_code(slice, error);
            if (!x86_64) return std::nullopt;
            cdhashes << x86_64->cdhash();
        } else if (const QByteArray other = other_cdhash(slice); !other.isEmpty()) {
            cdhashes << other;
        }
    }
    if (!x86_64) {
        *error = QStringLiteral("no x86_64 code in it");
        return std::nullopt;
    }
    x86_64->cdhashes = cdhashes;
    return x86_64;
}

template <typename T, void (*Free)(T*)>
using Owned = std::unique_ptr<T, decltype([](T* p) { Free(p); })>;

bool has_extension(X509* cert, const char* oid) {
    ASN1_OBJECT* object = OBJ_txt2obj(oid, 1);
    const bool found = object && X509_get_ext_by_OBJ(cert, object, -1) >= 0;
    ASN1_OBJECT_free(object);
    return found;
}

bool has_unit(X509* cert, const char* unit) {
    const X509_NAME* subject = X509_get_subject_name(cert);
    for (int i = X509_NAME_get_index_by_NID(subject, NID_organizationalUnitName, -1); i >= 0;
         i = X509_NAME_get_index_by_NID(subject, NID_organizationalUnitName, i)) {
        const ASN1_STRING* value = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, i));
        if (QByteArray(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)), ASN1_STRING_length(value)) == unit) {
            return true;
        }
    }
    return false;
}

// The signature over `directory` is one by Roblox's Developer ID, chained to
// the pinned Apple root. Certificates expire long before the code they signed
// is retired, so the chain is checked without the clock, as macOS does for a
// timestamped signature.
QString signed_by_roblox(const QByteArray& directory, const QByteArray& wrapped) {
    if (wrapped.size() <= 8) return QStringLiteral("not signed");
    const QByteArray der = wrapped.mid(8);
    const auto* p = reinterpret_cast<const unsigned char*>(der.constData());
    Owned<CMS_ContentInfo, CMS_ContentInfo_free> cms(d2i_CMS_ContentInfo(nullptr, &p, der.size()));
    if (!cms) return QStringLiteral("bad signature");
    Owned<BIO, BIO_free_all> content(BIO_new_mem_buf(directory.constData(), int(directory.size())));
    // The digest and the signer's signature only; the chain is checked below,
    // against Apple's root and with code signing's rules rather than e-mail's.
    if (CMS_verify(cms.get(), nullptr, nullptr, content.get(), nullptr, CMS_BINARY | CMS_NO_SIGNER_CERT_VERIFY) != 1) {
        return QStringLiteral("signature does not match the code directory");
    }
    STACK_OF(X509)* signers = CMS_get0_signers(cms.get());
    if (!signers || sk_X509_num(signers) != 1) {
        sk_X509_free(signers);
        return QStringLiteral("not exactly one signer");
    }
    X509* leaf = sk_X509_value(signers, 0);
    sk_X509_free(signers);

    const auto* root_bytes = kAppleRootCa;
    Owned<X509, X509_free> root(d2i_X509(nullptr, &root_bytes, long(sizeof kAppleRootCa)));
    Owned<X509_STORE, X509_STORE_free> store(X509_STORE_new());
    if (!root || !store || X509_STORE_add_cert(store.get(), root.get()) != 1) return QStringLiteral("Apple's root did not load");
    STACK_OF(X509)* certificates = CMS_get1_certs(cms.get());
    Owned<X509_STORE_CTX, X509_STORE_CTX_free> context(X509_STORE_CTX_new());
    QString failure;
    if (!context || X509_STORE_CTX_init(context.get(), store.get(), leaf, certificates) != 1) {
        failure = QStringLiteral("could not check the signer");
    } else {
        X509_STORE_CTX_set_purpose(context.get(), X509_PURPOSE_ANY);
        // Apple marks its Developer ID markers critical, and OpenSSL knows
        // none of them; they are checked by name below instead.
        X509_VERIFY_PARAM_set_flags(X509_STORE_CTX_get0_param(context.get()),
                                    X509_V_FLAG_NO_CHECK_TIME | X509_V_FLAG_IGNORE_CRITICAL);
        if (X509_verify_cert(context.get()) != 1) {
            failure = QStringLiteral("signer is not under Apple's root: %1")
                          .arg(QString::fromUtf8(X509_verify_cert_error_string(X509_STORE_CTX_get_error(context.get()))));
        } else {
            STACK_OF(X509)* chain = X509_STORE_CTX_get0_chain(context.get());
            if (sk_X509_num(chain) != 3 || !has_extension(sk_X509_value(chain, 1), kDeveloperIdCa)) {
                failure = QStringLiteral("signer is not a Developer ID");
            } else if (!has_extension(leaf, kDeveloperIdApp)) {
                failure = QStringLiteral("signer is not a Developer ID application");
            } else if (!has_unit(leaf, kRobloxTeam)) {
                failure = QStringLiteral("signer is not Roblox");
            }
        }
    }
    sk_X509_pop_free(certificates, X509_free);
    return failure;
}

// An XML property list: dictionaries, arrays, strings, data, booleans and
// numbers, as CodeResources and Info.plist hold them.
QVariant read_plist_value(QXmlStreamReader& xml) {
    const QStringView name = xml.name();
    if (name == u"dict") {
        QVariantHash dict;
        QString key;
        while (xml.readNextStartElement()) {
            if (xml.name() == u"key") {
                key = xml.readElementText();
            } else {
                dict.insert(key, read_plist_value(xml));
            }
        }
        return dict;
    }
    if (name == u"array") {
        QVariantList list;
        while (xml.readNextStartElement()) list << read_plist_value(xml);
        return list;
    }
    if (name == u"true" || name == u"false") {
        const bool value = name == u"true";
        xml.skipCurrentElement();
        return value;
    }
    if (name == u"data") return QByteArray::fromBase64(xml.readElementText().toLatin1());
    return xml.readElementText();
}

std::optional<QVariantHash> read_plist(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QXmlStreamReader xml(&file);
    while (xml.readNextStartElement()) {
        if (xml.name() == u"plist") continue;
        const QVariant value = read_plist_value(xml);
        if (xml.hasError() || value.typeId() != QMetaType::QVariantHash) return std::nullopt;
        return value.toHash();
    }
    return std::nullopt;
}

struct Bundle {
    QString seal_root;  // where the seal's paths start
    QString executable;
    QString info_plist;
};

// Where a .app or .framework keeps its sealed files, executable and Info.plist.
std::optional<Bundle> bundle_at(const QString& path, QString* error) {
    Bundle bundle;
    if (path.endsWith(QStringLiteral(".framework"))) {
        bundle.seal_root = QFileInfo(path + QStringLiteral("/Versions/Current")).canonicalFilePath();
        bundle.info_plist = bundle.seal_root + QStringLiteral("/Resources/Info.plist");
    } else {
        bundle.seal_root = path + QStringLiteral("/Contents");
        bundle.info_plist = bundle.seal_root + QStringLiteral("/Info.plist");
    }
    const auto info = read_plist(bundle.info_plist);
    QString name = info ? info->value(QStringLiteral("CFBundleExecutable")).toString() : QString();
    // An empty CFBundleExecutable names the bundle's own executable, as on
    // macOS: Studio's inner RobloxStudio.app has one.
    if (info && name.isEmpty()) name = QFileInfo(path).completeBaseName();
    if (bundle.seal_root.isEmpty() || name.isEmpty() || name.contains(QLatin1Char('/'))) {
        *error = QStringLiteral("%1 is not a bundle").arg(QFileInfo(path).fileName());
        return std::nullopt;
    }
    bundle.executable = path.endsWith(QStringLiteral(".framework")) ? bundle.seal_root + QLatin1Char('/') + name
                                                                     : bundle.seal_root + QStringLiteral("/MacOS/") + name;
    return bundle;
}

// A bundle's executable checked, and every file of it against its seal; the
// executable's code, for its caller to pin.
std::optional<Code> check_bundle(const QString& path, int depth, QString* error);

// Nested code, a bundle or a single executable file: its code-directory
// hashes, one per architecture.
std::optional<QList<QByteArray>> nested_cdhashes(const QString& path, int depth, QString* error) {
    if (QFileInfo(path).isDir()) {
        const auto code = check_bundle(path, depth + 1, error);
        return code ? std::optional(code->cdhashes) : std::nullopt;
    }
    const auto code = read_code_file(path, error);
    if (code && (!code->special(kSlotInfo).isEmpty() || !code->special(kSlotResources).isEmpty())) {
        *error = QStringLiteral("seals files it does not have");
        return std::nullopt;
    }
    return code ? std::optional(code->cdhashes) : std::nullopt;
}

std::optional<Code> check_bundle(const QString& path, int depth, QString* error) {
    const QString name = QFileInfo(path).fileName();
    const auto fail = [&](const QString& why) {
        *error = QStringLiteral("%1: %2").arg(name, why);
        return std::nullopt;
    };
    if (depth > kMaxDepth) return fail(QStringLiteral("nested too deeply"));
    QString why;
    const auto bundle = bundle_at(path, &why);
    if (!bundle) return fail(why);
    auto code = read_code_file(bundle->executable, &why);
    if (!code) return fail(QStringLiteral("%1: %2").arg(QFileInfo(bundle->executable).fileName(), why));

    const QString resources_path = bundle->seal_root + QStringLiteral("/_CodeSignature/CodeResources");
    if (sha256_of_file(bundle->info_plist) != code->special(kSlotInfo)) return fail(QStringLiteral("Info.plist was changed"));
    if (sha256_of_file(resources_path) != code->special(kSlotResources)) return fail(QStringLiteral("its seal was changed"));
    const auto seal = read_plist(resources_path);
    if (!seal) return fail(QStringLiteral("its seal cannot be read"));
    const QVariantHash files = seal->value(QStringLiteral("files2")).toHash();

    // Every sealed file present and unchanged.
    QStringList nested_roots;
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QString file = bundle->seal_root + QLatin1Char('/') + it.key();
        const QVariantHash entry = it.value().toHash();
        const QFileInfo info(file);
        if (!info.exists() && !info.isSymLink()) {
            if (entry.value(QStringLiteral("optional")).toBool()) continue;
            return fail(QStringLiteral("%1 is missing").arg(it.key()));
        }
        if (entry.contains(QStringLiteral("symlink"))) {
            if (!info.isSymLink() || QFile::symLinkTarget(file) != QFileInfo(info.absolutePath() + QLatin1Char('/') +
                                                                              entry.value(QStringLiteral("symlink")).toString())
                                                                     .absoluteFilePath()) {
                return fail(QStringLiteral("%1 was changed").arg(it.key()));
            }
        } else if (entry.contains(QStringLiteral("cdhash"))) {
            const auto cdhashes = nested_cdhashes(file, depth, &why);
            if (!cdhashes) return fail(why);
            if (!cdhashes->contains(entry.value(QStringLiteral("cdhash")).toByteArray())) {
                return fail(QStringLiteral("%1 is not the code that was sealed").arg(it.key()));
            }
            if (info.isDir()) nested_roots << it.key() + QLatin1Char('/');
        } else if (entry.contains(QStringLiteral("hash2"))) {
            if (info.isSymLink() || sha256_of_file(file) != entry.value(QStringLiteral("hash2")).toByteArray()) {
                return fail(QStringLiteral("%1 was changed").arg(it.key()));
            }
        } else {
            return fail(QStringLiteral("%1 has no SHA-256 seal").arg(it.key()));
        }
    }

    // Nothing added beside them, except what the seal's rules leave out.
    QList<QRegularExpression> omitted;
    const QVariantHash rules = seal->value(QStringLiteral("rules2")).toHash();
    for (auto it = rules.cbegin(); it != rules.cend(); ++it) {
        if (it.value().toHash().value(QStringLiteral("omit")).toBool()) omitted << QRegularExpression(it.key());
    }
    const QString executable = bundle->executable.mid(bundle->seal_root.size() + 1);
    QDirIterator walk(bundle->seal_root, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                      QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QString relative = walk.next().mid(bundle->seal_root.size() + 1);
        if (relative == executable || relative.startsWith(QStringLiteral("_CodeSignature/")) || files.contains(relative)) {
            continue;
        }
        // The client's FastFlags, the one file Stud writes among its code.
        if (depth == 0 && relative == QLatin1String(kSettingsFile)) continue;
        if (std::any_of(nested_roots.cbegin(), nested_roots.cend(),
                        [&](const QString& root) { return relative.startsWith(root); })) {
            continue;
        }
        if (std::any_of(omitted.cbegin(), omitted.cend(),
                        [&](const QRegularExpression& rule) { return rule.match(relative).hasMatch(); })) {
            continue;
        }
        return fail(QStringLiteral("%1 was added").arg(relative));
    }
    return code;
}

}  // namespace

QString verify_app(const QString& app, const QString& identifier) {
    QString error;
    const auto code = check_bundle(app, 0, &error);
    if (!code) return error;
    if (code->identifier.compare(identifier, Qt::CaseInsensitive) != 0) {
        return QStringLiteral("its executable is \"%1\", not %2").arg(code->identifier, identifier);
    }
    const auto signature = code->blobs.find(kSlotSignature);
    if (signature == code->blobs.end()) return QStringLiteral("not signed");
    return signed_by_roblox(code->directory, signature->second);
}

}  // namespace stud::ui::codesign
