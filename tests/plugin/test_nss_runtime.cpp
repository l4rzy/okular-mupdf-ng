// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/crypto/certificate_database.hpp"
#include "plugin/crypto/nss.hpp"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

class EnvironmentGuard {
public:
    explicit EnvironmentGuard(const char* name)
        : m_name(name)
        , m_wasSet(qEnvironmentVariableIsSet(name))
        , m_value(qgetenv(name))
    {
    }

    ~EnvironmentGuard()
    {
        if (m_wasSet)
            qputenv(m_name.constData(), m_value);
        else
            qunsetenv(m_name.constData());
    }

    EnvironmentGuard(const EnvironmentGuard&) = delete;
    EnvironmentGuard& operator=(const EnvironmentGuard&) = delete;

private:
    QByteArray m_name;
    bool m_wasSet;
    QByteArray m_value;
};

class PermissionGuard {
public:
    explicit PermissionGuard(const QString& directory)
        : m_directory(directory)
        , m_directoryPermissions(QFileInfo(directory).permissions())
    {
        const QFileInfoList files = QDir(directory).entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& file : files)
            m_files.emplace_back(file.filePath(), file.permissions());
    }

    ~PermissionGuard()
    {
        QFile::setPermissions(m_directory, m_directoryPermissions);
        for (const auto& [path, permissions] : m_files)
            QFile::setPermissions(path, permissions);
    }

    [[nodiscard]] bool makeReadOnly()
    {
        for (const auto& file : m_files) {
            if (!QFile::setPermissions(file.first, QFileDevice::ReadOwner))
                return false;
        }
        return QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    }

    PermissionGuard(const PermissionGuard&) = delete;
    PermissionGuard& operator=(const PermissionGuard&) = delete;

private:
    QString m_directory;
    QFile::Permissions m_directoryPermissions;
    std::vector<std::pair<QString, QFile::Permissions>> m_files;
};

bool runCommand(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted()) {
        qWarning("%s could not start: %s", qPrintable(program), qPrintable(process.errorString()));
        return false;
    }
    const bool succeeded =
        process.waitForFinished() && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (!succeeded)
        qWarning("%s failed: %s", qPrintable(program), qPrintable(process.readAllStandardError().trimmed()));
    return succeeded;
}

bool initializeEmptyDatabase(const QString& directory)
{
    return runCommand(QStringLiteral("certutil"),
                      { QStringLiteral("-N"),
                        QStringLiteral("-d"),
                        QStringLiteral("sql:") + directory,
                        QStringLiteral("--empty-password") });
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return { };
    return file.readAll();
}

} // namespace

class TestNssRuntime : public QObject {
    Q_OBJECT

private slots:

    void readWriteMode()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("certutil")).isEmpty())
            QSKIP("certutil is required for the NSS runtime test");

        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString database = root.filePath(QStringLiteral("database"));
        QVERIFY(QDir().mkpath(database));
        QVERIFY(initializeEmptyDatabase(database));
        const QString alias = root.filePath(QStringLiteral("database-alias"));
        QVERIFY(QFile::link(database, alias));

        const QString canonicalDatabase = QFileInfo(database).canonicalFilePath();
        EnvironmentGuard environment("NSS_DEFAULT_DB");
        qputenv("NSS_DEFAULT_DB", (alias + QStringLiteral("/.")).toUtf8());
        QCOMPARE(::Mu::Plugin::Crypto::defaultSystemNssDbPath(), canonicalDatabase);
        qputenv("NSS_DEFAULT_DB", (QStringLiteral("sql:") + alias + QStringLiteral("/.")).toUtf8());
        QCOMPARE(::Mu::Plugin::Crypto::defaultSystemNssDbPath(), QStringLiteral("sql:") + canonicalDatabase);
        QCOMPARE(::Mu::Plugin::Crypto::initializeNss(), ::Mu::Plugin::Crypto::NssRuntimeMode::ReadWrite);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssMode(), ::Mu::Plugin::Crypto::NssRuntimeMode::ReadWrite);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssDatabasePath(), canonicalDatabase);

        struct ActivePathCase {
            QString path;
            bool expected;
        };

        const std::array cases {
            ActivePathCase { database, true },
            ActivePathCase { QStringLiteral("sql:") + database, true },
            ActivePathCase { database + QStringLiteral("/."), true },
            ActivePathCase { alias, true },
            ActivePathCase { QStringLiteral("dbm:") + database, false },
            ActivePathCase { root.filePath(QStringLiteral("other")), false },
        };
        for (const auto& test : cases)
            QCOMPARE(::Mu::Plugin::Crypto::isNssDatabaseActive(test.path), test.expected);

        QCOMPARE(::Mu::Plugin::Crypto::initializeNss(root.filePath(QStringLiteral("other"))),
                 ::Mu::Plugin::Crypto::NssRuntimeMode::Unavailable);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssMode(), ::Mu::Plugin::Crypto::NssRuntimeMode::ReadWrite);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssDatabasePath(), canonicalDatabase);

        QString error;
        QVERIFY(
            ::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(root.filePath(QStringLiteral("other")), &error)
                .isEmpty());
        QVERIFY(error.contains(QStringLiteral("not active"), Qt::CaseInsensitive));
    }

    void readOnlyMode()
    {
        if (::geteuid() == 0)
            QSKIP("read-only NSS database permissions cannot be tested as root");
        if (QStandardPaths::findExecutable(QStringLiteral("certutil")).isEmpty())
            QSKIP("certutil is required for the NSS runtime test");

        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString database = root.filePath(QStringLiteral("database"));
        QVERIFY(QDir().mkpath(database));
        QVERIFY(initializeEmptyDatabase(database));
        PermissionGuard permissions(database);
        QVERIFY(permissions.makeReadOnly());

        QCOMPARE(::Mu::Plugin::Crypto::initializeNss(database), ::Mu::Plugin::Crypto::NssRuntimeMode::ReadOnly);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssMode(), ::Mu::Plugin::Crypto::NssRuntimeMode::ReadOnly);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssDatabasePath(), QFileInfo(database).canonicalFilePath());

        QString error;
        QVERIFY(::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(database, &error).isEmpty());
        QVERIFY2(error.isEmpty(), qPrintable(error));

        QVERIFY(!::Mu::Plugin::Crypto::CertificateDatabase::importCertificate(
            database, QByteArrayLiteral("not a certificate"), QStringLiteral("nickname"), &error));
        QVERIFY(error.contains(QStringLiteral("read-only"), Qt::CaseInsensitive));

        const auto signingResult = ::Mu::Plugin::Crypto::createDetachedCmsFromDigest(
            QStringLiteral("nickname"), { }, std::array<std::uint8_t, 32> { });
        QCOMPARE(signingResult.result, ::Mu::Model::SigningResult::GenericError);
        QVERIFY(signingResult.details.contains(QStringLiteral("writable"), Qt::CaseInsensitive));
        QVERIFY(::Mu::Plugin::Crypto::signingCertificates().isEmpty());
    }

    void noDbMode()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("openssl")).isEmpty())
            QSKIP("openssl is required for the NSS NoDB runtime test");

        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QByteArray content = QByteArrayLiteral("detached PDF signature content\n");
        const QString contentPath = root.filePath(QStringLiteral("content.bin"));
        const QString keyPath = root.filePath(QStringLiteral("key.pem"));
        const QString certificatePath = root.filePath(QStringLiteral("certificate.pem"));
        const QString signaturePath = root.filePath(QStringLiteral("signature.der"));
        QVERIFY(writeFile(contentPath, content));
        QVERIFY(runCommand(QStringLiteral("openssl"),
                           { QStringLiteral("req"),
                             QStringLiteral("-x509"),
                             QStringLiteral("-newkey"),
                             QStringLiteral("rsa:2048"),
                             QStringLiteral("-keyout"),
                             keyPath,
                             QStringLiteral("-out"),
                             certificatePath,
                             QStringLiteral("-nodes"),
                             QStringLiteral("-subj"),
                             QStringLiteral("/CN=NoDB PDF Signer"),
                             QStringLiteral("-days"),
                             QStringLiteral("1"),
                             QStringLiteral("-sha256"),
                             QStringLiteral("-addext"),
                             QStringLiteral("keyUsage=critical,digitalSignature"),
                             QStringLiteral("-addext"),
                             QStringLiteral("extendedKeyUsage=emailProtection") }));
        QVERIFY(runCommand(QStringLiteral("openssl"),
                           { QStringLiteral("cms"),
                             QStringLiteral("-sign"),
                             QStringLiteral("-binary"),
                             QStringLiteral("-in"),
                             contentPath,
                             QStringLiteral("-signer"),
                             certificatePath,
                             QStringLiteral("-inkey"),
                             keyPath,
                             QStringLiteral("-outform"),
                             QStringLiteral("DER"),
                             QStringLiteral("-out"),
                             signaturePath,
                             QStringLiteral("-md"),
                             QStringLiteral("sha256"),
                             QStringLiteral("-nosmimecap") }));

        const QString missingDatabase = root.filePath(QStringLiteral("missing/database"));
        QCOMPARE(::Mu::Plugin::Crypto::initializeNss(missingDatabase), ::Mu::Plugin::Crypto::NssRuntimeMode::NoDb);
        QCOMPARE(::Mu::Plugin::Crypto::activeNssMode(), ::Mu::Plugin::Crypto::NssRuntimeMode::NoDb);
        QVERIFY(::Mu::Plugin::Crypto::activeNssDatabasePath().isEmpty());
        QVERIFY(!::Mu::Plugin::Crypto::isNssDatabaseActive(missingDatabase));

        const QByteArray signature = readFile(signaturePath);
        QVERIFY(!signature.isEmpty());
        QBuffer source;
        source.setData(content);
        QVERIFY(source.open(QIODevice::ReadOnly));
        ::Mu::Model::SignatureField field;
        field.signedField = true;
        field.byteRange = { 0, content.size(), content.size(), 0 };
        field.cmsSignature.assign(signature.cbegin(), signature.cend());
        ::Mu::Plugin::Crypto::validateDetachedPdfSignature(field, source);
        QCOMPARE(field.signatureStatus, ::Mu::Model::SignatureStatus::Valid);
        QCOMPARE(field.certificateStatus, ::Mu::Model::CertificateStatus::UntrustedIssuer);

        QString error;
        QVERIFY(::Mu::Plugin::Crypto::CertificateDatabase::listCertificates(missingDatabase, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("persistent"), Qt::CaseInsensitive));
        const auto signingResult = ::Mu::Plugin::Crypto::createDetachedCmsFromDigest(
            QStringLiteral("nickname"), { }, std::array<std::uint8_t, 32> { });
        QCOMPARE(signingResult.result, ::Mu::Model::SigningResult::GenericError);
        QVERIFY(signingResult.details.contains(QStringLiteral("writable"), Qt::CaseInsensitive));
    }
};

QTEST_GUILESS_MAIN(TestNssRuntime)

#include "test_nss_runtime.moc"
