// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef TEST_XFDF_FIXTURE_HPP
#define TEST_XFDF_FIXTURE_HPP

#include <QFile>
#include <QTest>

inline void addXfdfRotationRows()
{
    QTest::addColumn<int>("rotation");
    QTest::addColumn<bool>("inherited");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("unrotated") << 0 << false << true;
    QTest::newRow("90") << 90 << false << false;
    QTest::newRow("180") << 180 << false << false;
    QTest::newRow("270") << 270 << false << false;
    QTest::newRow("inherited-90") << 90 << true << false;
    QTest::newRow("negative-90") << -90 << false << false;
    QTest::newRow("whole-turn") << 360 << false << true;
    QTest::newRow("inherited-whole-turn") << -360 << true << true;
}

// The first page is explicitly unrotated and annotated. The second is empty
// and carries (or inherits) /Rotate, so rejection must check the entire PDF.
inline bool writeXfdfRotationPdf(const QString& path, int rotation, bool inherited)
{
    const QByteArray rotate = " /Rotate " + QByteArray::number(rotation);
    const QList<QByteArray> objects {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2" + (inherited ? rotate : QByteArray()) + " >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 500] /Resources << >> /Rotate 0 /Annots [5 0 R] >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 500] /Resources << >>" + (inherited ? QByteArray() : rotate)
            + " >>",
        "<< /Type /Annot /Subtype /Text /Rect [20 30 40 50] /Contents (fixture note) /P 3 0 R >>",
    };
    QByteArray data("%PDF-1.7\n");
    QList<qsizetype> offsets;
    for (qsizetype index = 0; index < objects.size(); ++index) {
        offsets.append(data.size());
        data += QByteArray::number(index + 1) + " 0 obj\n" + objects.at(index) + "\nendobj\n";
    }
    const qsizetype xref = data.size();
    data += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const qsizetype offset : offsets)
        data += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    data += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n"
        + QByteArray::number(xref) + "\n%%EOF\n";
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

#endif
