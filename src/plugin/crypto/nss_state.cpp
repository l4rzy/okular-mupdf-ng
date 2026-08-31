// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_error_internal.hpp"
#include "plugin/crypto/nss_handles.hpp"
#include "plugin/crypto/nss_internal.hpp"

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <nss.h>
#include <prerror.h>
#pragma pop_macro("slots")

#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include <mutex>

#include "shared/logging.hpp"

namespace Mu::Plugin::Crypto::Internal {

std::mutex& nssMutex()
{
    static std::mutex mutex;
    return mutex;
}

QString nssErrorMessage(PRErrorCode error)
{
    // Translate the current NSS error code without changing global NSS state.
    const char* name = PR_ErrorToName(error);
    return QStringLiteral("NSS operation failed: %1 (%2)")
        .arg(name ? QString::fromLatin1(name) : QStringLiteral("unknown error"))
        .arg(error);
}

namespace {

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

namespace {

using Mu::Plugin::Crypto::NssRuntimeMode;

QString g_nssDatabaseIdentity;
NssRuntimeMode g_nssMode = NssRuntimeMode::Unavailable;

bool isPersistentMode(NssRuntimeMode mode)
{
    return mode == NssRuntimeMode::ReadOnly || mode == NssRuntimeMode::ReadWrite;
}

void initializeInternalToken()
{
    Mu::Plugin::Crypto::NssSlot slot(PK11_GetInternalKeySlot());
    if (!slot) {
        MU_LOG(warning, "Mu::Plugin::Crypto", "Unable to access the NSS internal key slot");
        return;
    }
    if (!PK11_NeedUserInit(slot.get()))
        return;
    if (PK11_InitPin(slot.get(), "", "") != SECSuccess) {
        const PRErrorCode error = PR_GetError();
        MU_LOG(warning,
               "Mu::Plugin::Crypto",
               std::string("Unable to initialize the NSS internal key slot (PR_GetError code: ") + std::to_string(error)
                   + ")");
    }
}

void recordNssRuntime(NssRuntimeMode mode, const QString& databaseIdentity = { })
{
    CERT_SetUsePKIXForValidation(PR_TRUE);
    g_nssMode = mode;
    g_nssDatabaseIdentity = isPersistentMode(mode) ? databaseIdentity : QString { };
}

void logPersistentInitializationFailure(const char* access, const QString& databaseIdentity, PRErrorCode error)
{
    MU_LOG(warning,
           "Mu::Plugin::Crypto",
           std::string("Error initializing ") + access + " NSS cert DB at " + databaseIdentity.toStdString()
               + " (PR_GetError code: " + std::to_string(error) + ")");
}

// Creates the directory of a *defaulted* database selection. chooseNssDbPath()
// only returns directories that already exist, except for the final
// ~/.pki/nssdb fallback, so this is the single point where a missing default
// database directory is created. Explicit caller paths are deliberately never
// created: a typo must degrade to NoDB mode instead of silently initializing
// an empty database.
void ensureDefaultDatabaseDirectory(const QString& databaseIdentity)
{
    const QString databaseDir = Mu::Plugin::Crypto::Internal::stripNssScheme(databaseIdentity);
    if (databaseDir.isEmpty() || QFileInfo(databaseDir).isDir())
        return;
    if (!QDir().mkpath(databaseDir))
        MU_LOG(warning,
               "Mu::Plugin::Crypto",
               std::string("Unable to create the default NSS database directory at ") + databaseDir.toStdString());
}

} // namespace

namespace Mu::Plugin::Crypto {

NssRuntimeMode initializeNss(const QString& databasePath)
{
    const QString requestedPath = databasePath.trimmed();
    const bool defaultedDatabase = requestedPath.isEmpty();
    const QString configuredPath = defaultedDatabase ? defaultSystemNssDbPath() : requestedPath;
    const QString databaseIdentity = Internal::canonicalNssDatabasePath(configuredPath);
    if (defaultedDatabase)
        ensureDefaultDatabaseDirectory(databaseIdentity);
    std::lock_guard<std::mutex> lock(Internal::nssMutex());

    if (NSS_IsInitialized()) {
        // NSS initialization is process-global and one-way. An explicit path
        // must identify the already-active persistent database.
        if (g_nssMode == NssRuntimeMode::Unavailable)
            return NssRuntimeMode::Unavailable;
        if (!requestedPath.isEmpty() && (!isPersistentMode(g_nssMode) || g_nssDatabaseIdentity != databaseIdentity))
            return NssRuntimeMode::Unavailable;
        return g_nssMode;
    }

    if (!databaseIdentity.isEmpty()) {
        const QByteArray nssPathBytes = databaseIdentity.toUtf8();
        if (NSS_InitReadWrite(nssPathBytes.constData()) == SECSuccess) {
            initializeInternalToken();
            recordNssRuntime(NssRuntimeMode::ReadWrite, databaseIdentity);
            return g_nssMode;
        }
        const PRErrorCode readWriteError = PR_GetError();

        if (NSS_Init(nssPathBytes.constData()) == SECSuccess) {
            recordNssRuntime(NssRuntimeMode::ReadOnly, databaseIdentity);
            return g_nssMode;
        }
        logPersistentInitializationFailure("read/write", databaseIdentity, readWriteError);
        logPersistentInitializationFailure("read-only", databaseIdentity, PR_GetError());
    }

    // NoDB still supports CMS integrity checks using certificates embedded in
    // the signature, but it does not provide a persistent trust or key store.
    MU_LOG(warning, "Mu::Plugin::Crypto", "Falling back to NSS NoDB mode");
    if (NSS_NoDB_Init(nullptr) == SECSuccess) {
        recordNssRuntime(NssRuntimeMode::NoDb);
        return g_nssMode;
    }

    MU_LOG(critical, "Mu::Plugin::Crypto", "Failed to initialize NSS even in NoDB mode");
    return NssRuntimeMode::Unavailable;
}

NssRuntimeMode activeNssMode()
{
    std::lock_guard<std::mutex> lock(Internal::nssMutex());
    return NSS_IsInitialized() ? g_nssMode : NssRuntimeMode::Unavailable;
}

QString activeNssDatabasePath()
{
    std::lock_guard<std::mutex> lock(Internal::nssMutex());
    if (!NSS_IsInitialized() || !isPersistentMode(g_nssMode) || g_nssDatabaseIdentity.isEmpty())
        return { };
    return Internal::stripNssScheme(g_nssDatabaseIdentity);
}

bool isNssDatabaseActive(const QString& databasePath)
{
    const QString requestedPath = databasePath.trimmed();
    const QString requestedIdentity = Internal::canonicalNssDatabasePath(requestedPath);
    std::lock_guard<std::mutex> lock(Internal::nssMutex());
    return NSS_IsInitialized() && isPersistentMode(g_nssMode)
        && (requestedPath.isEmpty() || g_nssDatabaseIdentity == requestedIdentity);
}

} // namespace Mu::Plugin::Crypto
