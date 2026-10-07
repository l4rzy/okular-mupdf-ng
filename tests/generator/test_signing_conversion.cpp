// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/certificate.hpp"
#include "generator/conversion/signing.hpp"
#include "plugin/util/signing_timestamp.hpp"

#include <QDateTime>
#include <QTest>
#include <QTimeZone>

using Mu::Generator::Conversion::toModelSignatureAppearance;

using Mu::Generator::Config::SignatureEmblem;

class TestGeneratorSigningConversion : public QObject {
    Q_OBJECT

private slots:

    void buildsDefaultAppearanceFromSignatureData()
    {
        Okular::NewSignatureData data;
        data.setCertNickname("test-cert");
        data.setCertSubjectCommonName("Test Signer");
        data.setReason("unit test reason");
        data.setLocation("unit test location");

        const auto appearanceResult = toModelSignatureAppearance(data, { });
        QVERIFY(appearanceResult);
        const auto& appearance = *appearanceResult;

        // Every element is rendered by default, using the Okular emblem.
        QCOMPARE(appearance.elements,
                 Mu::Model::SignatureElementDefault & ~static_cast<std::uint8_t>(Mu::Model::SignatureElement::Logo));
        QVERIFY(!appearance.emblemImage.empty());
        QCOMPARE(appearance.reason, std::string("unit test reason"));
        QCOMPARE(appearance.location, std::string("unit test location"));
        QVERIFY(appearance.signingEpochSeconds > 0);
        QVERIFY(appearance.backgroundImage.empty());
        // The display date describes exactly the captured epoch.
        const QDateTime epoch = QDateTime::fromSecsSinceEpoch(appearance.signingEpochSeconds);
        QCOMPARE(QString::fromStdString(appearance.signingDisplayDate),
                 Mu::Plugin::Util::SigningTimestamp::displayDate(epoch));
    }

    void buildsSimpleAppearanceFromSignatureData()
    {
        Okular::NewSignatureData data;
        data.setCertNickname("test-cert");
        data.setCertSubjectCommonName("Test Signer");
        data.setReason("unit test reason");
        data.setLocation("unit test location");

        const auto appearanceResult = toModelSignatureAppearance(data, { true, false });
        QVERIFY(appearanceResult);
        const auto& appearance = *appearanceResult;

        // Simple renders name, reason, and time while retaining location
        // for the signature dictionary.
        QCOMPARE(appearance.elements, Mu::Model::SignatureElementSimple);
        QCOMPARE(appearance.reason, std::string("unit test reason"));
        QCOMPARE(appearance.location, std::string("unit test location"));
        QVERIFY(appearance.signingEpochSeconds > 0);
        const QDateTime epoch = QDateTime::fromSecsSinceEpoch(appearance.signingEpochSeconds);
        QCOMPARE(QString::fromStdString(appearance.signingDisplayDate),
                 Mu::Plugin::Util::SigningTimestamp::displayDate(epoch));
    }

    void rendersAppearanceTimestampInUtc()
    {
        Okular::NewSignatureData data;
        data.setCertNickname("test-cert");
        data.setCertSubjectCommonName("Test Signer");

        const auto appearanceResult = toModelSignatureAppearance(data, { false, true });
        QVERIFY(appearanceResult);
        const auto& appearance = *appearanceResult;

        QVERIFY(appearance.signingEpochSeconds > 0);
        const QDateTime utc = QDateTime::fromSecsSinceEpoch(appearance.signingEpochSeconds, QTimeZone::UTC);
        QCOMPARE(QString::fromStdString(appearance.signingDisplayDate),
                 Mu::Plugin::Util::SigningTimestamp::displayDate(utc));
        QVERIFY(QString::fromStdString(appearance.signingDisplayDate).endsWith(QStringLiteral("UTC")));
    }

    void selectsSignatureEmblem_data()
    {
        QTest::addColumn<bool>("simple");
        QTest::addColumn<SignatureEmblem>("emblem");
        QTest::newRow("complete-none") << false << SignatureEmblem::None;
        QTest::newRow("complete-mupdf") << false << SignatureEmblem::MuPDF;
        QTest::newRow("simple-none") << true << SignatureEmblem::None;
        QTest::newRow("simple-mupdf") << true << SignatureEmblem::MuPDF;
        QTest::newRow("complete-okular") << false << SignatureEmblem::Okular;
        QTest::newRow("simple-okular") << true << SignatureEmblem::Okular;
    }

    void selectsSignatureEmblem()
    {
        QFETCH(bool, simple);
        QFETCH(SignatureEmblem, emblem);
        Mu::Generator::Config::SignatureAppearanceOptions options;
        options.simple = simple;
        options.emblem = emblem;
        const auto appearanceResult = toModelSignatureAppearance(Okular::NewSignatureData { }, options);
        QVERIFY(appearanceResult);
        const auto& appearance = *appearanceResult;
        QCOMPARE(!appearance.emblemImage.empty(), emblem == SignatureEmblem::Okular);
        const auto logo = static_cast<std::uint8_t>(Mu::Model::SignatureElement::Logo);
        QCOMPARE((appearance.elements & logo) != 0, emblem == SignatureEmblem::MuPDF);
        const auto profile = simple ? Mu::Model::SignatureElementSimple : Mu::Model::SignatureElementDefault;
        QCOMPARE(appearance.elements & ~logo, profile & ~logo);
    }

    void passesDrawBorderOptionToAppearance()
    {
        Okular::NewSignatureData data;
        data.setCertNickname("test-cert");
        data.setCertSubjectCommonName("Test Signer");

        const auto borderedResult = toModelSignatureAppearance(data, { false, false, true });
        QVERIFY(borderedResult);
        const auto& bordered = *borderedResult;
        QVERIFY(bordered.drawBorder);

        const auto plainResult = toModelSignatureAppearance(data, { });
        QVERIFY(plainResult);
        const auto& plain = *plainResult;
        QVERIFY(!plain.drawBorder);
    }

    void roundTripsCertificateInfo()
    {
        Mu::Model::Certificate source;
        source.null = false;
        source.version = 3;
        source.serialNumber = { 0x01, 0x02, 0x03 };
        source.issuerCommonName = "Test Issuer CN";
        source.issuerDistinguishedName = "CN=Test Issuer";
        source.issuerEmail = "issuer@example.com";
        source.issuerOrganization = "Issuer Org";
        source.subjectCommonName = "Test Subject CN";
        source.subjectDistinguishedName = "CN=Test Subject";
        source.subjectEmail = "subject@example.com";
        source.subjectOrganization = "Subject Org";
        source.nickname = "test-nickname";
        source.validityStart = { true, 1700000000000 };
        source.validityEnd = { true, 1730000000000 };
        source.keyUsage = 0xa0;
        source.publicKey = { 0x04, 0x05 };
        source.publicKeyType = static_cast<std::int32_t>(Mu::Model::PublicKeyAlgorithm::Dsa);
        source.publicKeyStrength = 2048;
        source.selfSigned = true;
        source.der = { 0x30, 0x82 };

        const Okular::CertificateInfo info = Mu::Generator::Conversion::toOkularCertificateInfo(source);

        QVERIFY(!info.isNull());
        QCOMPARE(info.version(), 3);
        QCOMPARE(info.serialNumber(), QByteArray("\x01\x02\x03", 3));
        QCOMPARE(info.issuerInfo(Okular::CertificateInfo::CommonName, Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("Test Issuer CN"));
        QCOMPARE(
            info.issuerInfo(Okular::CertificateInfo::DistinguishedName, Okular::CertificateInfo::EmptyString::Empty),
            QStringLiteral("CN=Test Issuer"));
        QCOMPARE(info.issuerInfo(Okular::CertificateInfo::EmailAddress, Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("issuer@example.com"));
        QCOMPARE(info.issuerInfo(Okular::CertificateInfo::Organization, Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("Issuer Org"));
        QCOMPARE(info.subjectInfo(Okular::CertificateInfo::CommonName, Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("Test Subject CN"));
        QCOMPARE(
            info.subjectInfo(Okular::CertificateInfo::DistinguishedName, Okular::CertificateInfo::EmptyString::Empty),
            QStringLiteral("CN=Test Subject"));
        QCOMPARE(info.subjectInfo(Okular::CertificateInfo::EmailAddress, Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("subject@example.com"));
        QCOMPARE(info.subjectInfo(Okular::CertificateInfo::Organization, Okular::CertificateInfo::EmptyString::Empty),
                 QStringLiteral("Subject Org"));
        QCOMPARE(info.nickName(), QStringLiteral("test-nickname"));
        QCOMPARE(info.validityStart(), QDateTime::fromMSecsSinceEpoch(1700000000000, QTimeZone::UTC));
        QCOMPARE(info.validityEnd(), QDateTime::fromMSecsSinceEpoch(1730000000000, QTimeZone::UTC));
        QVERIFY(info.isSelfSigned());
        QCOMPARE(static_cast<std::uint32_t>(info.keyUsageExtensions()), 0xa0u);
        QCOMPARE(info.publicKey(), QByteArray("\x04\x05", 2));
        QCOMPARE(info.publicKeyType(), Okular::CertificateInfo::DsaKey);
        QCOMPARE(info.publicKeyStrength(), 2048);
        QCOMPARE(info.certificateData(), QByteArray("\x30\x82", 2));
    }
};

#include "test_signing_conversion.moc"

int main(int argc, char** argv)
{
    TestGeneratorSigningConversion test;
    return QTest::qExec(&test, argc, argv);
}
