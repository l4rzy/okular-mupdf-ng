// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/nss.hpp"
#include "plugin/crypto/nss_error_internal.hpp"
#include "plugin/crypto/nss_internal.hpp"

#pragma push_macro("slots")
#undef slots
#include <cert.h>
#include <nss.h>
#include <prerror.h>
#pragma pop_macro("slots")

#include <QDir>
#include <QFileInfo>

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

QString stripNssScheme(const QString& path)
{
    // NSS returns scheme-qualified paths, while callers expose the filesystem
    // path used to configure the database.
    if (path.startsWith(QStringLiteral("sql:")))
        return path.mid(4);
    if (path.startsWith(QStringLiteral("dbm:")))
        return path.mid(4);
    return path;
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
    const QString envNssDb = qEnvironmentVariable("NSS_DEFAULT_DB");
    const QString userPkiDb = QDir::homePath() + QStringLiteral("/.pki/nssdb");
    const QString sysPkiDb = QStringLiteral("/etc/pki/nssdb");
    return Internal::chooseNssDbPath(envNssDb,
                                     !envNssDb.isEmpty() && QFileInfo::exists(envNssDb),
                                     userPkiDb,
                                     QFileInfo::exists(userPkiDb),
                                     sysPkiDb,
                                     QFileInfo::exists(sysPkiDb));
}

} // namespace Mu::Plugin::Crypto

namespace {

QString g_nssDatabasePath;
bool g_hasPersistentNssDatabase = false;

QString normalizedNssDatabasePath(const QString& databasePath)
{
    // NSS expects a scheme-qualified database path; normalize equivalent
    // caller forms before comparing or initializing the process-global state.
    const QString targetPath =
        databasePath.trimmed().isEmpty() ? Mu::Plugin::Crypto::defaultSystemNssDbPath() : databasePath.trimmed();
    if (targetPath.startsWith(QStringLiteral("sql:")) || targetPath.startsWith(QStringLiteral("dbm:")))
        return targetPath;
    return QStringLiteral("sql:") + targetPath;
}

bool tryInitPersistentNssDatabase(const QByteArray& nssPathBytes, const QString& targetPath)
{
    // Record persistent state only after NSS accepts the database path; failed
    // initialization must not make later callers believe a store is active.
    if (NSS_InitReadWrite(nssPathBytes.constData()) != SECSuccess)
        return false;
    CERT_SetUsePKIXForValidation(PR_TRUE);
    g_nssDatabasePath = targetPath;
    g_hasPersistentNssDatabase = true;
    return true;
}

bool tryInitNoDbNssDatabase()
{
    // NoDB is an explicit fallback for temporary NSS operations and must never
    // be reported as a persistent user certificate store.
    if (NSS_NoDB_Init(nullptr) != SECSuccess)
        return false;
    CERT_SetUsePKIXForValidation(PR_TRUE);
    g_nssDatabasePath.clear();
    g_hasPersistentNssDatabase = false;
    return true;
}

} // namespace

namespace Mu::Plugin::Crypto {

bool ensureNssInitialized(const QString& databasePath, bool allowNoDbFallback)
{
    // Initialization is one-way for the process. Once NSS is active, reject a
    // conflicting requested database rather than silently switching stores.
    const QString requestedPath = databasePath.trimmed();
    const QString targetPath = normalizedNssDatabasePath(requestedPath);
    const QByteArray nssPathBytes = targetPath.toUtf8();
    std::lock_guard<std::mutex> lock(Internal::nssMutex());

    if (NSS_IsInitialized()) {
        if (!requestedPath.isEmpty() && (!g_hasPersistentNssDatabase || g_nssDatabasePath != targetPath))
            return false;
        return g_hasPersistentNssDatabase || allowNoDbFallback;
    }
    if (tryInitPersistentNssDatabase(nssPathBytes, targetPath))
        return true;

    const PRErrorCode err = PR_GetError();
    MU_LOG(warning,
           "Mu::Plugin::Crypto",
           std::string("Error initializing NSS cert DB at ") + targetPath.toStdString()
               + " (PR_GetError code: " + std::to_string(err) + ")");

    if (!allowNoDbFallback)
        return false;

    // NoDB mode is useful for operations that only need temporary certificate
    // material, but it cannot expose the user's persistent certificate store.
    MU_LOG(warning, "Mu::Plugin::Crypto", "Falling back to NSS NoDB mode");
    if (tryInitNoDbNssDatabase())
        return true;

    MU_LOG(critical, "Mu::Plugin::Crypto", "Failed to initialize NSS even in NoDB mode");
    return false;
}

QString activeNssDatabasePath()
{
    std::lock_guard<std::mutex> lock(Internal::nssMutex());
    if (!NSS_IsInitialized() || !g_hasPersistentNssDatabase || g_nssDatabasePath.isEmpty())
        return { };
    return Internal::stripNssScheme(g_nssDatabasePath);
}

bool isNssDatabaseActive(const QString& databasePath)
{
    // Compare normalized paths while holding the same mutex used by startup.
    const QString requestedPath = databasePath.trimmed();
    std::lock_guard<std::mutex> lock(Internal::nssMutex());
    return NSS_IsInitialized() && g_hasPersistentNssDatabase
        && (requestedPath.isEmpty() || g_nssDatabasePath == normalizedNssDatabasePath(requestedPath));
}

bool hasPersistentNssDatabase()
{
    std::lock_guard<std::mutex> lock(Internal::nssMutex());
    return NSS_IsInitialized() && g_hasPersistentNssDatabase;
}

} // namespace Mu::Plugin::Crypto
