// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "paperbox_model.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_internal.hpp"
#include <paperbox/logging.hpp>

#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <iterator>
#include <utility>

namespace Mu::Plugin::Crypto::Internal {

namespace {

// =============================================================================
// Database Path Normalization & Selection
// =============================================================================

inline std::pair<QString, QString> splitNssScheme(const QString& value)
{
    if (value.startsWith(QStringLiteral("sql:"), Qt::CaseInsensitive))
        return { value.left(4).toLower(), value.mid(4) };
    if (value.startsWith(QStringLiteral("dbm:"), Qt::CaseInsensitive))
        return { value.left(4).toLower(), value.mid(4) };
    return { { }, value };
}

} // namespace

QString stripNssScheme(const QString& path)
{
    // NSS returns scheme-qualified paths, while callers expose the filesystem
    // path used to configure the database.
    const auto [scheme, stripped] = splitNssScheme(path);
    return scheme.isEmpty() ? path : stripped;
}

QString canonicalNssDatabasePath(const QString& path)
{
    const QString trimmedPath = path.trimmed();
    if (trimmedPath.isEmpty())
        return { };

    // URL-form values such as "file:/tmp/nssdb" (e.g. stored by KUrlRequester)
    // are local paths, not NSS database schemes, and must not be treated as
    // relative paths.
    QString effectivePath = trimmedPath;
    if (effectivePath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        effectivePath = QUrl(effectivePath).toLocalFile();
        if (effectivePath.isEmpty())
            return { };
        effectivePath = QDir::cleanPath(effectivePath);
    }

    const auto [scheme, stripped] = splitNssScheme(effectivePath);
    if (stripped.isEmpty())
        return { };

    const QString canonicalScheme = scheme.isEmpty() ? QStringLiteral("sql:") : scheme;
    const QString filePath = stripped;

    const QFileInfo fileInfo(filePath);
    QString canonicalPath = fileInfo.canonicalFilePath();
    if (canonicalPath.isEmpty())
        canonicalPath = QDir::cleanPath(fileInfo.absoluteFilePath());
    return canonicalScheme + canonicalPath;
}

QString chooseNssDbPath(const QString& envDb,
                        bool envExists,
                        const QString& userPkiDb,
                        bool userExists,
                        const QString& sysPkiDb,
                        bool sysExists)
{
    // Prefer explicit configuration, then the user's database, and finally the
    // system database so startup remains deterministic across installations.
    if (!envDb.isEmpty() && envExists)
        return envDb;
    if (userExists)
        return userPkiDb;
    if (sysExists)
        return sysPkiDb;
    return userPkiDb;
}

} // namespace Mu::Plugin::Crypto::Internal

namespace Mu::Plugin::Crypto {

QString defaultSystemNssDbPath()
{
    // Prefer an explicit environment override, then the user's database, then
    // the system database. The user path is the final deterministic fallback.
    const QString configuredEnvNssDb = qEnvironmentVariable("NSS_DEFAULT_DB").trimmed();
    const QString canonicalEnvNssDb = Internal::canonicalNssDatabasePath(configuredEnvNssDb);
    // An explicitly configured scheme (e.g. dbm:) is honored, so selection runs
    // on plain paths and the scheme is re-attached only when the environment
    // database is the one returned.
    const bool envHasScheme = !Internal::splitNssScheme(configuredEnvNssDb).first.isEmpty();
    const QString envNssDb = Internal::stripNssScheme(canonicalEnvNssDb);
    const QString userPkiDb =
        Internal::stripNssScheme(Internal::canonicalNssDatabasePath(QDir::homePath() + QStringLiteral("/.pki/nssdb")));
    const QString sysPkiDb =
        Internal::stripNssScheme(Internal::canonicalNssDatabasePath(QStringLiteral("/etc/pki/nssdb")));
    const bool envExists = !envNssDb.isEmpty() && QFileInfo(envNssDb).isDir();
    const QString selectedPath = Internal::chooseNssDbPath(
        envNssDb, envExists, userPkiDb, QFileInfo(userPkiDb).isDir(), sysPkiDb, QFileInfo(sysPkiDb).isDir());
    if (envExists && selectedPath == envNssDb && envHasScheme)
        return canonicalEnvNssDb;
    return selectedPath;
}

} // namespace Mu::Plugin::Crypto

namespace Mu::Plugin::Crypto {
namespace {

void ensureDefaultDatabaseDirectory(const QString& databaseIdentity)
{
    const QString databaseDir = Mu::Plugin::Crypto::Internal::stripNssScheme(databaseIdentity);
    if (databaseDir.isEmpty() || QFileInfo(databaseDir).isDir())
        return;
    if (!QDir().mkpath(databaseDir))
        paperbox::log::write(paperbox::log::Level::Warning,
                             "Mu::Plugin::Crypto",
                             std::string("Unable to create the default NSS database directory at ")
                                 + databaseDir.toStdString());
}

} // namespace

NssRuntimeMode initializeNss(const QString& databasePath)
{
    const auto requestedPath = databasePath.trimmed();
    if (!requestedPath.isEmpty()) {
        const auto identity = Internal::canonicalNssDatabasePath(requestedPath);
        if (identity.isEmpty() && paperbox::nss::activeRuntime().mode != NssRuntimeMode::Unavailable)
            return NssRuntimeMode::Unavailable;
        return paperbox::nss::initializeNss(identity.toStdString());
    }
    const auto runtime = paperbox::nss::activeRuntime();
    if (runtime.mode != NssRuntimeMode::Unavailable)
        return runtime.mode;
    const auto identity = Internal::canonicalNssDatabasePath(defaultSystemNssDbPath());
    ensureDefaultDatabaseDirectory(identity);
    return paperbox::nss::initializeNss(identity.toStdString());
}

NssRuntimeMode activeNssMode()
{
    return paperbox::nss::activeRuntime().mode;
}

QString activeNssDatabasePath()
{
    return Internal::stripNssScheme(QString::fromStdString(paperbox::nss::activeRuntime().database));
}

bool isNssDatabaseActive(const QString& databasePath)
{
    const auto runtime = paperbox::nss::activeRuntime();
    const auto requested = databasePath.trimmed();
    return !runtime.database.empty()
        && (requested.isEmpty() || runtime.database == Internal::canonicalNssDatabasePath(requested).toStdString());
}

QList<Model::Certificate> signingCertificates()
{
    auto certificates = paperbox::nss::signingCertificates();
    return { std::make_move_iterator(certificates.begin()), std::make_move_iterator(certificates.end()) };
}

QString signingCertificateCommonName(const QString& nickname)
{
    return QString::fromStdString(paperbox::nss::signingCertificateCommonName(nickname.toStdString()));
}

bool checkSigningCertificatePassword(const QString& nickname, const QString& password)
{
    auto bytes = password.toUtf8();
    const auto result = paperbox::nss::checkSigningCertificatePassword(
        nickname.toStdString(), std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    bytes.fill('\0');
    return result;
}

} // namespace Mu::Plugin::Crypto
