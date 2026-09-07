// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/signing.hpp"
#include "plugin/util/signing_timestamp.hpp"

#include <QDateTime>
#include <QTest>

using Mu::Generator::Conversion::toModelSignatureAppearance;

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

        const auto appearance = toModelSignatureAppearance(data);

        // Every element is rendered by default (parity with the previous
        // hardcoded MuPDF default appearance).
        QCOMPARE(appearance.elements, Mu::Model::SignatureElementDefault);
        QCOMPARE(appearance.reason, std::string("unit test reason"));
        QCOMPARE(appearance.location, std::string("unit test location"));
        QVERIFY(appearance.signingEpochSeconds > 0);
        QVERIFY(appearance.backgroundImage.empty());
        // The display date describes exactly the captured epoch.
        const QDateTime epoch = QDateTime::fromSecsSinceEpoch(appearance.signingEpochSeconds);
        QCOMPARE(QString::fromStdString(appearance.signingDisplayDate),
                 Mu::Plugin::Util::SigningTimestamp::displayDate(epoch));
    }
};

#include "test_signing_conversion.moc"

int main(int argc, char** argv)
{
    TestGeneratorSigningConversion test;
    return QTest::qExec(&test, argc, argv);
}
