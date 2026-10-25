// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "../support/xfdf_fixture.hpp"
#include "generator/conversion/annotation.hpp"
#include "generator/conversion/xfdf.hpp"
#include "plugin/xfdf/export.hpp"
#include "plugin/xfdf/import.hpp"

#include <KActionCollection>
#include <KConfigGroup>
#include <KSharedConfig>
#include <KXMLGUIBuilder>
#include <KXMLGUIClient>
#include <KXMLGUIFactory>
#include <QAction>
#include <QApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMimeDatabase>
#include <QScopeGuard>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

#include <okular/core/annotations.h>
#include <okular/core/document.h>
#include <okular/core/generator.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <vector>

using Mu::Generator::Conversion::annotationsToXfdf;
using Mu::Plugin::Xfdf::normalizedRectToUserSpace;
using Mu::Plugin::Xfdf::Page;

namespace Xfdf = Mu::Plugin::Xfdf;
using Mu::Model::Annotation;

namespace {

/// Builds a 200x100 page (user-space points).
Okular::Page* makePage(double width = 200, double height = 100)
{
    // Okular::Page takes ownership of annotations added to it, so callers only
    // own the returned page.
    return new Okular::Page(1, width, height, Okular::Rotation0);
}

/// Serializes via the generator overload at 72 dpi, where device pixels equal
/// PDF points, so page dimensions pass through unscaled.
QString generatorXfdf(std::initializer_list<Okular::Page*> pages)
{
    return annotationsToXfdf(QVector<Okular::Page*>(pages), QSizeF(72, 72));
}

Annotation baseAnnotation(Mu::Model::AnnotationType type, double x0, double y0, double x1, double y1)
{
    Annotation annotation;
    annotation.subtype = type;
    annotation.x0 = x0;
    annotation.y0 = y0;
    annotation.x1 = x1;
    annotation.y1 = y1;
    annotation.color = 0xffff0000U; // opaque red
    annotation.contents = "note";
    annotation.author = "alice";
    annotation.uuid = "uuid-1";
    return annotation;
}

} // namespace

class TestGeneratorXfdf : public QObject {
    Q_OBJECT

    QTemporaryDir m_root;

    static bool importFile(Okular::Document& document,
                           const QString& path,
                           const std::function<void(QFileDialog*)>& choose = { },
                           QAction* menuAction = nullptr)
    {
        auto* action = menuAction
            ? menuAction
            : document.guiClient()->actionCollection()->action(QStringLiteral("mupdf_import_xfdf"));
        if (!action || !action->isEnabled())
            return false;
        bool selected = false;
        bool accepted = path.isEmpty();
        QTimer timer;
        timer.setInterval(10);
        connect(&timer, &QTimer::timeout, &timer, [&] {
            auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            timer.stop();
            selected = true;
            connect(dialog, &QFileDialog::accepted, &timer, [&] { accepted = true; });
            if (choose)
                choose(dialog);
            if (path.isEmpty())
                dialog->reject();
            else {
                dialog->setDirectory(QFileInfo(path).absolutePath());
                // QFileSystemModel populates a newly selected directory asynchronously.
                QTimer::singleShot(100, dialog, [dialog, path] {
                    auto* input = dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
                    if (!input) {
                        dialog->reject();
                        return;
                    }
                    input->setText(path);
                    QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
                });
                QTimer::singleShot(3000, dialog, &QFileDialog::reject);
            }
        });
        timer.start();
        action->trigger();
        return selected && accepted;
    }

private slots:

    void initTestCase()
    {
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.filePath("config").toUtf8());
        qputenv("XDG_CACHE_HOME", m_root.filePath("cache").toUtf8());
        qputenv("XDG_DATA_HOME", m_root.filePath("data").toUtf8());
        QCoreApplication::setLibraryPaths({ QStringLiteral(TEST_PLUGIN_ROOT) });
        qputenv("PATH", QByteArray(TEST_WORKER_DIR) + ':' + qgetenv("PATH"));
        Okular::SettingsCore::instance(QStringLiteral("xfdf-test"));
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup(config, QStringLiteral("OCR")).writeEntry("OcrTriggerMode", "Never");
        config->sync();
    }

    void importsAnnotations_data()
    {
        QTest::addColumn<QByteArray>("xml");
        QTest::addColumn<int>("added");
        QTest::addColumn<bool>("fatal");
        const QByteArray note = "<text page=\"0\" rect=\"20,30,40,50\"><contents>imported</contents></text>";
        QTest::newRow("valid") << "<xfdf><annots>" + note + "</annots></xfdf>" << 1 << false;
        QTest::newRow("multi-page") << "<xfdf><annots>" + note
                + "<text page=\"1\" rect=\"20,30,40,50\"/></annots></xfdf>"
                                    << 2 << false;
        QTest::newRow("skipped-entry") << "<xfdf><annots>" + note + "<unknown/></annots></xfdf>" << 1 << false;
        QTest::newRow("native-stamp") << QByteArray(
            "<xfdf><annots><stamp page=\"0\" rect=\"20,30,40,50\" icon=\"Approved\"/></annots></xfdf>")
                                      << 1 << false;
        QTest::newRow("stamp-file-path") << "<xfdf><annots>" + note
                + "<stamp page=\"0\" rect=\"20,30,40,50\" icon=\"/tmp/untrusted-stamp.svg\"/></annots></xfdf>"
                                         << 1 << false;
        QTest::newRow("invalid-page") << QByteArray(
            "<xfdf><annots><text page=\"99\" rect=\"0,0,10,10\"/></annots></xfdf>")
                                      << 0 << false;
        QTest::newRow("unsupported-only") << QByteArray("<xfdf><annots><unknown/></annots></xfdf>") << 0 << false;
        QTest::newRow("empty-input") << QByteArray() << 0 << true;
        QTest::newRow("malformed-after-note") << "<xfdf><annots>" + note << 0 << true;
        QTest::newRow("doctype") << QByteArray("<!DOCTYPE xfdf><xfdf><annots/></xfdf>") << 0 << true;
        QTest::newRow("too-large") << QByteArray(Xfdf::MaxXfdfBytes + 1, ' ') << 0 << true;
    }

    void importsAnnotations()
    {
        QFETCH(QByteArray, xml);
        QFETCH(int, added);
        QFETCH(bool, fatal);
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        const QString path = directory.filePath("notes.xfdf");
        QFile input(path);
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(xml), xml.size());
        input.close();
        Okular::Document document(nullptr);
        QCOMPARE(
            document.openDocument(pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
            Okular::Document::OpenSuccess);
        QSignalSpy errors(&document, &Okular::Document::error);
        QSignalSpy notices(&document, &Okular::Document::notice);
        QVERIFY(importFile(document, path));
        QCOMPARE(document.page(0)->annotations().size() + document.page(1)->annotations().size(), 1 + added);
        QCOMPARE(errors.count(), fatal ? 1 : 0);
        QCOMPARE(notices.count(), added ? 1 : 0);
        QCOMPARE(document.canUndo(), added > 0);
        for (int i = 0; i < added; ++i)
            document.undo();
        QCOMPARE(document.page(0)->annotations().size(), 1);
        QCOMPARE(document.page(1)->annotations().size(), 0);
        for (int i = 0; i < added; ++i)
            document.redo();
        QCOMPARE(document.page(0)->annotations().size() + document.page(1)->annotations().size(), 1 + added);
        if (added) {
            const QString saved = directory.filePath("saved.pdf");
            QString error;
            QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
            Okular::Document reopened(nullptr);
            QCOMPARE(reopened.openDocument(
                         saved, QUrl::fromLocalFile(saved), QMimeDatabase().mimeTypeForName("application/pdf")),
                     Okular::Document::OpenSuccess);
            QCOMPARE(reopened.page(0)->annotations().size() + reopened.page(1)->annotations().size(), 1 + added);
        }
    }

    void importsWithViewRotation_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::addColumn<bool>("changeDuringImport");
        for (int rotation = 0; rotation < 4; ++rotation)
            QTest::newRow(qPrintable(QString::number(rotation))) << rotation << false;
        QTest::newRow("rotation-changes-during-import") << 0 << true;
    }

    void importsWithViewRotation()
    {
        QFETCH(int, rotation);
        QFETCH(bool, changeDuringImport);
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        const QByteArray xml = R"(<xfdf><annots>
          <text page="0" rect="30,50,90,150" name="duplicate"><contents>note</contents></text>
          <line page="0" rect="30,50,90,150" start="30,50" end="90,150"/>
          <highlight page="0" rect="30,50,90,150" coords="30,150,90,150,30,50,90,50"/>
          <ink page="0" rect="30,50,90,150"><inklist><gesture>30,50;90,150</gesture></inklist></ink>
          <freetext page="0" rect="30,50,90,150" callout="30,50,60,100,90,150" intent="FreeTextCallout"/>
        </annots></xfdf>)";
        const QString path = directory.filePath("notes.xml");
        QFile input(path);
        QVERIFY(input.open(QIODevice::WriteOnly));
        QCOMPARE(input.write(xml), xml.size());
        input.close();
        Okular::Document document(nullptr);
        QCOMPARE(
            document.openDocument(pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
            Okular::Document::OpenSuccess);
        document.setRotation(rotation);
        bool rotated = false;
        connect(&document, &Okular::Document::canUndoChanged, &document, [&](bool available) {
            if (changeDuringImport && available && !rotated) {
                rotated = true;
                document.setRotation((rotation + 1) % 4);
            }
        });
        QString error;
        const auto expected = Xfdf::xfdfToAnnotations(xml, { QSizeF(300, 500), QSizeF(300, 500) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(expected.applied, 5);
        for (int repeat = 0; repeat < 2; ++repeat) {
            QVERIFY(importFile(document, path));
            const auto annotations = document.page(0)->annotations();
            QCOMPARE(annotations.size(), 1 + (repeat + 1) * 5);
            for (int i = 0; i < 5; ++i) {
                const auto* annotation = annotations.at(1 + repeat * 5 + i);
                QVERIFY(!annotation->nativeId().toString().isEmpty());
                QVERIFY(annotation->uniqueName() != "duplicate");
                const auto actual = Mu::Generator::Conversion::toModel(annotation);
                QVERIFY(actual);
                const auto& source = expected.pages.at(0).annotations.at(i);
                QCOMPARE(actual->subtype, source.subtype);
                QCOMPARE(actual->x0, source.x0);
                QCOMPARE(actual->y0, source.y0);
                QCOMPARE(actual->x1, source.x1);
                QCOMPARE(actual->y1, source.y1);
                const auto checkPoints = [](const auto& actualPoints, const auto& expectedPoints) {
                    QCOMPARE(actualPoints.size(), expectedPoints.size());
                    for (std::size_t j = 0; j < actualPoints.size(); ++j) {
                        QCOMPARE(actualPoints[j].x, expectedPoints[j].x);
                        QCOMPARE(actualPoints[j].y, expectedPoints[j].y);
                    }
                };
                checkPoints(actual->extras.points, source.extras.points);
                checkPoints(actual->extras.callout, source.extras.callout);
                QCOMPARE(actual->extras.inkPaths.size(), source.extras.inkPaths.size());
                for (std::size_t j = 0; j < actual->extras.inkPaths.size(); ++j)
                    checkPoints(actual->extras.inkPaths[j], source.extras.inkPaths[j]);
                QCOMPARE(actual->extras.quads.size(), source.extras.quads.size());
                for (std::size_t j = 0; j < actual->extras.quads.size(); ++j) {
                    const auto& a = actual->extras.quads[j];
                    const auto& b = source.extras.quads[j];
                    checkPoints(std::vector { a.lowerLeft, a.lowerRight, a.upperRight, a.upperLeft },
                                std::vector { b.lowerLeft, b.lowerRight, b.upperRight, b.upperLeft });
                }
            }
        }
        QSet<QString> names;
        for (const auto* annotation : document.page(0)->annotations())
            names.insert(annotation->uniqueName());
        QCOMPARE(names.size(), 11);
    }

    void rejectsRotatedImport_data() { addXfdfRotationRows(); }

    void rejectsRotatedImport()
    {
        QFETCH(int, rotation);
        QFETCH(bool, inherited);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, rotation, inherited));
        const QString path = directory.filePath("notes.xfdf");
        QFile input(path);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write("<xfdf><annots><text page=\"0\" rect=\"20,30,40,50\"/></annots></xfdf>");
        input.close();
        Okular::Document document(nullptr);
        QCOMPARE(
            document.openDocument(pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
            Okular::Document::OpenSuccess);
        QVERIFY(importFile(document, path));
        QCOMPARE(document.page(0)->annotations().size(), accepted ? 2 : 1);
        QCOMPARE(document.canUndo(), accepted);
    }

    void cancelsImport_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("cancel-dialog") << 0;
        QTest::newRow("close-document-before-accept") << 1;
        QTest::newRow("disable-annotations-before-accept") << 2;
    }

    void cancelsImport()
    {
        QFETCH(int, change);
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        const QString path = directory.filePath("notes.xfdf");
        QFile input(path);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write("<xfdf><annots><text page=\"0\" rect=\"20,30,40,50\"/></annots></xfdf>");
        input.close();
        Okular::Document document(nullptr);
        QCOMPARE(
            document.openDocument(pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
            Okular::Document::OpenSuccess);
        QSignalSpy notices(&document, &Okular::Document::notice);
        QVERIFY(importFile(document, change == 0 ? QString() : path, [&](QFileDialog*) {
            if (change == 1)
                document.closeDocument();
            else if (change == 2)
                document.setAnnotationEditingEnabled(false);
        }));
        QCOMPARE(notices.count(), 0);
        QVERIFY(!document.canUndo());
        if (change != 1)
            QCOMPARE(document.page(0)->annotations().size(), 1);
    }

    void validatesInputFile_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("fifo") << 0;
        QTest::newRow("symlink-to-fifo") << 1;
        QTest::newRow("device") << 2;
        QTest::newRow("symlink-to-regular-file") << 3;
    }

    void validatesInputFile()
    {
        QFETCH(int, kind);
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        const QString source = directory.filePath("source.xfdf");
        const QString path = directory.filePath("notes.xml");
        if (kind < 2) {
            QCOMPARE(::mkfifo(QFile::encodeName(source).constData(), 0600), 0);
            if (kind == 1)
                QVERIFY(QFile::link(source, path));
        } else if (kind == 2) {
            QVERIFY(QFile::link(QStringLiteral("/dev/zero"), path));
        } else {
            QFile input(source);
            QVERIFY(input.open(QIODevice::WriteOnly));
            input.write("<xfdf><annots><text page=\"0\" rect=\"20,30,40,50\"/></annots></xfdf>");
            input.close();
            QVERIFY(QFile::link(source, path));
        }
        Okular::Document document(nullptr);
        QCOMPARE(
            document.openDocument(pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
            Okular::Document::OpenSuccess);
        QSignalSpy errors(&document, &Okular::Document::error);
        QVERIFY(importFile(document, kind == 0 ? source : path));
        QCOMPARE(errors.count(), kind == 3 ? 0 : 1);
        QCOMPARE(document.page(0)->annotations().size(), kind == 3 ? 2 : 1);
        QCOMPARE(document.canUndo(), kind == 3);
    }

    void respectsAnnotationPermission()
    {
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        Okular::Document document(nullptr);
        QCOMPARE(
            document.openDocument(pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
            Okular::Document::OpenSuccess);
        const auto action = [&] {
            return document.guiClient()->actionCollection()->action(QStringLiteral("mupdf_import_xfdf"));
        };
        QVERIFY(action());
        QVERIFY(action()->isEnabled());
        document.setAnnotationEditingEnabled(false);
        QVERIFY(!action()->isEnabled());
        document.setAnnotationEditingEnabled(true);
        QVERIFY(action()->isEnabled());
    }

    void checksRemainingCapacity()
    {
        std::unique_ptr<Okular::Page> page(makePage());
        for (std::size_t i = 0; i < Mu::Limit::MaxAnnotationsPerPage; ++i)
            page->addAnnotation(new Okular::TextAnnotation());
        QString error;
        const auto result = Mu::Generator::Conversion::annotationsFromXfdf(
            "<xfdf><annots><text page=\"0\" rect=\"0,0,10,10\"/></annots></xfdf>",
            { page.get() },
            QSizeF(72, 72),
            &error);
        QVERIFY(!error.isEmpty());
        QCOMPARE(result.applied, 0);
    }

    void restrictsImportedStampNames_data()
    {
        QTest::addColumn<QString>("symbol");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("default") << QString() << true;
        QTest::newRow("native") << QStringLiteral("Approved") << true;
        QTest::newRow("file-path") << QStringLiteral("/tmp/stamp.png") << false;
        QTest::newRow("relative-path") << QStringLiteral("../stamp.png") << false;
        QTest::newRow("theme-icon") << QStringLiteral("document-open") << false;
    }

    void restrictsImportedStampNames()
    {
        QFETCH(QString, symbol);
        QFETCH(bool, accepted);
        auto model = baseAnnotation(Mu::Model::AnnotationType::Stamp, 0.1, 0.1, 0.2, 0.2);
        model.extras.style.appearance = Mu::Model::AnnotationAppearance();
        model.extras.style.appearance->icon = symbol.toStdString();
        QCOMPARE(bool(Mu::Generator::Conversion::annotationFromXfdf(model, Okular::Rotation0)), accepted);
    }

    void sharesEditMenu()
    {
        QTemporaryDir directory;
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        const QString path = directory.filePath("notes.xfdf");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("<xfdf><annots><text page=\"0\" rect=\"20,30,40,50\"/></annots></xfdf>");
        file.close();
        QMainWindow window;
        KXMLGUIBuilder builder(&window);
        KXMLGUIFactory factory(&builder);

        struct MenuClient : KXMLGUIClient {
            using KXMLGUIClient::setXML;
        } host;

        host.actionCollection()->addAction(QStringLiteral("existing"),
                                           new QAction(QStringLiteral("Existing"), &window));
        host.setXML(QStringLiteral("<gui name=\"test\"><MenuBar><Menu name=\"edit\"><text>Edit</text>"
                                   "<Action name=\"existing\"/></Menu></MenuBar></gui>"));
        factory.addClient(&host);
        Okular::Document first(nullptr), second(nullptr);
        auto cleanup = qScopeGuard([&] {
            for (auto* document : { &first, &second })
                if (auto* client = document->guiClient(); client && client->factory())
                    factory.removeClient(client);
            factory.removeClient(&host);
        });
        for (auto* document : { &first, &second }) {
            QCOMPARE(document->openDocument(
                         pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName("application/pdf")),
                     Okular::Document::OpenSuccess);
            factory.addClient(document->guiClient());
        }
        const auto menu = [&] {
            return qobject_cast<QMenu*>(factory.container(QStringLiteral("edit"), &host));
        };
        QTRY_VERIFY(menu() && menu()->actions().size() == 3);
        QVERIFY(menu()->actions().at(1)->isSeparator());
        QCOMPARE(menu()->actions().at(2)->objectName(), QStringLiteral("mupdf_import_xfdf"));
        QVERIFY(importFile(first, path, { }, menu()->actions().at(2)));
        QCOMPARE(first.page(0)->annotations().size(), 2);
        QCOMPARE(second.page(0)->annotations().size(), 1);
        factory.removeClient(first.guiClient());
        QTRY_VERIFY(menu() && menu()->actions().size() == 3);
        QVERIFY(menu()->actions().at(1)->isSeparator());
        QVERIFY(importFile(second, path, { }, menu()->actions().at(2)));
        QCOMPARE(first.page(0)->annotations().size(), 2);
        QCOMPARE(second.page(0)->annotations().size(), 2);
    }

    void checksPdfRotation_data() { addXfdfRotationRows(); }

    void checksPdfRotation()
    {
        QFETCH(int, rotation);
        QFETCH(bool, inherited);
        QFETCH(bool, accepted);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, rotation, inherited));
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(
                     pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                 Okular::Document::OpenSuccess);
        const Okular::ExportFormat format(QStringLiteral("XFDF"),
                                          QMimeDatabase().mimeTypeForName(QStringLiteral("application/xml")));
        const QString output = directory.filePath("output.xfdf");
        for (const bool existing : { false, true }) {
            if (existing) {
                QFile destination(output);
                QVERIFY(destination.open(QIODevice::WriteOnly));
                destination.write("preserve destination");
            }
            QCOMPARE(document.exportTo(output, format), accepted);
            if (accepted || existing) {
                QFile result(output);
                QVERIFY(result.open(QIODevice::ReadOnly));
                const QByteArray data = result.readAll();
                if (accepted)
                    QVERIFY(data.contains("fixture note"));
                else
                    QCOMPARE(data, QByteArray("preserve destination"));
            } else {
                QVERIFY(!QFile::exists(output));
            }
            if (QFile::exists(output))
                QVERIFY(QFile::remove(output));
        }
    }

    void preservesCoordinatesWithViewRotation_data()
    {
        QTest::addColumn<int>("rotation");
        QTest::newRow("0") << int(Okular::Rotation0);
        QTest::newRow("90") << int(Okular::Rotation90);
        QTest::newRow("180") << int(Okular::Rotation180);
        QTest::newRow("270") << int(Okular::Rotation270);
    }

    void preservesCoordinatesWithViewRotation()
    {
        QFETCH(int, rotation);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString pdf = directory.filePath("source.pdf");
        QVERIFY(writeXfdfRotationPdf(pdf, 0, false));
        Okular::Document document(nullptr);
        QCOMPARE(document.openDocument(
                     pdf, QUrl::fromLocalFile(pdf), QMimeDatabase().mimeTypeForName(QStringLiteral("application/pdf"))),
                 Okular::Document::OpenSuccess);
        const QString output = directory.filePath("output.xfdf");
        const Okular::ExportFormat format(QStringLiteral("XFDF"),
                                          QMimeDatabase().mimeTypeForName(QStringLiteral("application/xml")));
        QVERIFY(document.exportTo(output, format));
        QFile file(output);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray original = file.readAll();
        QVERIFY(original.contains("fixture note"));
        file.close();

        document.setRotation(rotation);
        QCOMPARE(document.rotation(), static_cast<Okular::Rotation>(rotation));
        QVERIFY(document.exportTo(output, format));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), original);
    }

    void emptyDocumentHasEmptyAnnots()
    {
        const QString xfdf = generatorXfdf({ });
        QVERIFY(xfdf.contains(QStringLiteral("<annots>")));
        QVERIFY(xfdf.contains(QStringLiteral("</annots>")));
        QVERIFY(xfdf.contains(QStringLiteral("http://ns.adobe.com/xfdf/")));
        QVERIFY(!xfdf.contains(QStringLiteral("page=")));
    }

    void serializesModelPagesWithoutOkularAdapter()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        page.annotations.append(baseAnnotation(Mu::Model::AnnotationType::Text, 0.1, 0.2, 0.5, 0.6));

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(xfdf.contains(QStringLiteral("<text ")));
        QVERIFY(xfdf.contains(QStringLiteral("page=\"0\"")));
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,40,100,80\"")));
        QVERIFY(xfdf.contains(QStringLiteral("title=\"alice\"")));
    }

    void scalesDevicePixelsBackToPoints()
    {
        // Okular stores page dimensions in device pixels (points * dpi/72). At
        // 96 dpi a 200x100 pt page is 266.67x133.33 px; serializing must undo the
        // scaling so annotation coordinates stay on the PDF point grid.
        auto* page = makePage(200.0 * 96.0 / 72.0, 100.0 * 96.0 / 72.0);
        auto* annotation = new Okular::TextAnnotation();
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.6));
        page->addAnnotation(annotation);

        const QString xfdf = annotationsToXfdf({ page }, QSizeF(96, 96));
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,40,100,80\"")));
    }

    void convertsRectToUserSpace()
    {
        // Normalized (0.1, 0.2)-(0.5, 0.6) on a 200x100 page: Y flips because
        // Okular counts downward while PDF user-space counts upward.
        const Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::Text, 0.1, 0.2, 0.5, 0.6);
        const Mu::Model::Quad quad = normalizedRectToUserSpace(annotation, 200, 100);
        QCOMPARE(quad.lowerLeft.x, 20.0);
        QCOMPARE(quad.lowerLeft.y, 40.0);
        QCOMPARE(quad.upperRight.x, 100.0);
        QCOMPARE(quad.upperRight.y, 80.0);
    }

    void emitsTextAnnotation()
    {
        auto* page = makePage();
        auto* annotation = new Okular::TextAnnotation();
        annotation->setContents(QStringLiteral("hello <world> & \"friends\""));
        annotation->setAuthor(QStringLiteral("bob"));
        annotation->setUniqueName(QStringLiteral("u1"));
        annotation->setCreationDate(QDateTime::fromMSecsSinceEpoch(-1000, QTimeZone::UTC));
        annotation->setModificationDate(QDateTime::fromMSecsSinceEpoch(0, QTimeZone::UTC));
        annotation->setFlags(Okular::Annotation::Hidden | Okular::Annotation::FixedSize
                             | Okular::Annotation::DenyPrint);
        annotation->style().setColor(QColor(0, 255, 0));
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.6));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<text ")));
        QVERIFY(xfdf.contains(QStringLiteral("page=\"0\"")));
        QVERIFY(xfdf.contains(QStringLiteral("color=\"#00ff00\"")));
        QVERIFY(xfdf.contains(QStringLiteral("title=\"bob\"")));
        QVERIFY(xfdf.contains(QStringLiteral("flags=\"hidden,nozoom\"")));
        QVERIFY(xfdf.contains(QStringLiteral("creationdate=\"D:19691231235959+00'00'\"")));
        QVERIFY(xfdf.contains(QStringLiteral("date=\"D:19700101000000+00'00'\"")));
        // Special characters are XML-escaped in contents.
        QVERIFY(xfdf.contains(QStringLiteral("hello &lt;world&gt; &amp; &quot;friends&quot;")));
    }

    void stripsXmlIllegalControlCharacters()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::Text, 0.1, 0.2, 0.5, 0.6);
        // Vertical tab and a NUL are illegal in XML 1.0; tab and newline are kept.
        annotation.contents = std::string("a\vb\0c\td\ne", 9);
        annotation.author = "x\fy";
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(!xfdf.contains(QLatin1Char('\v')));
        QVERIFY(!xfdf.contains(QLatin1Char('\f')));
        QVERIFY(!xfdf.contains(QLatin1Char('\0')));
        QVERIFY(xfdf.contains(QStringLiteral("<contents>abc\td\ne</contents>")));
        QVERIFY(xfdf.contains(QStringLiteral("title=\"xy\"")));

        QXmlStreamReader reader(xfdf);
        while (!reader.atEnd())
            reader.readNext();
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    }

    void replacesNonFiniteCoordinatesWithZero()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::Square,
                                               std::nan(""),
                                               std::numeric_limits<double>::infinity(),
                                               std::numeric_limits<double>::infinity(),
                                               std::nan(""));
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        // No NaN/inf tokens may reach the output; they collapse to 0.
        QVERIFY(!xfdf.contains(QStringLiteral("nan")));
        QVERIFY(!xfdf.contains(QStringLiteral("inf")));
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"0,0,0,0\"")));

        QXmlStreamReader reader(xfdf);
        while (!reader.atEnd())
            reader.readNext();
        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    }

    void emitsHighlightWithQuad()
    {
        auto* page = makePage();
        auto* annotation = new Okular::HighlightAnnotation();
        annotation->setHighlightType(Okular::HighlightAnnotation::Highlight);
        annotation->style().setColor(QColor(255, 255, 0));
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.3));
        Okular::HighlightAnnotation::Quad quad;
        // Okular quad order is points 0..3 = lowerLeft..upperLeft.
        quad.setPoint(Okular::NormalizedPoint(0.1, 0.3), 0);
        quad.setPoint(Okular::NormalizedPoint(0.5, 0.3), 1);
        quad.setPoint(Okular::NormalizedPoint(0.5, 0.2), 2);
        quad.setPoint(Okular::NormalizedPoint(0.1, 0.2), 3);
        annotation->highlightQuads().append(quad);
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<highlight")));
        // The worker reports a padded bound, so the rect is derived from the
        // quad union instead: x 0.1..0.5, y 0.2..0.3 on a 200x100 page.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,70,100,80\"")));
        // XFDF retains Acrobat's top-left, top-right, bottom-left, bottom-right
        // order. On a 200x100 page the normalized corners map to user-space Y of
        // (1 - y) * 100, giving 80 for the top edge and 70 for the bottom edge.
        QVERIFY(xfdf.contains(QStringLiteral("coords=\"20,80,100,80,20,70,100,70\"")));
    }

    void emitsMultipleHighlightQuadsInOrder()
    {
        auto* page = makePage();
        auto* annotation = new Okular::HighlightAnnotation();
        annotation->setHighlightType(Okular::HighlightAnnotation::Highlight);
        annotation->style().setColor(QColor(255, 255, 0));
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.3));
        for (double top : { 0.2, 0.4 }) {
            Okular::HighlightAnnotation::Quad quad;
            quad.setPoint(Okular::NormalizedPoint(0.1, top + 0.1), 0);
            quad.setPoint(Okular::NormalizedPoint(0.5, top + 0.1), 1);
            quad.setPoint(Okular::NormalizedPoint(0.5, top), 2);
            quad.setPoint(Okular::NormalizedPoint(0.1, top), 3);
            annotation->highlightQuads().append(quad);
        }
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        // Each quad appends four points; quads keep their document order.
        QVERIFY(xfdf.contains(QStringLiteral("coords=\"20,80,100,80,20,70,100,70,20,60,100,60,20,50,100,50\"")));
        // The rect spans the union of both quads: y 0.2..0.5.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,50,100,80\"")));
    }

    void freeTextCalloutUsesSeparateGeometry()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::FreeText, 0.1, 0.2, 0.3, 0.4);
        // The leader line ends outside the text box and must not expand its rect.
        annotation.extras.callout = { { 0.3, 0.4 }, { 0.8, 0.9 } };
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,60,60,80\"")));
        QVERIFY(xfdf.contains(QStringLiteral("callout=\"60,60,160,10\"")));
        QVERIFY(xfdf.contains(QStringLiteral("intent=\"FreeTextCallout\"")));

        QString error;
        const auto imported = Xfdf::xfdfToAnnotations(xfdf.toUtf8(), { QSizeF(200, 100) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.applied, 1);
        const Annotation& roundTripped = imported.pages.first().annotations.first();
        QCOMPARE(roundTripped.x0, 0.1);
        QCOMPARE(roundTripped.y0, 0.2);
        QCOMPARE(roundTripped.x1, 0.3);
        QCOMPARE(roundTripped.y1, 0.4);
        QCOMPARE(roundTripped.extras.callout.size(), 2U);
        QCOMPARE(roundTripped.extras.callout[0].x, 0.3);
        QCOMPARE(roundTripped.extras.callout[0].y, 0.4);
        QCOMPARE(roundTripped.extras.callout[1].x, 0.8);
        QCOMPARE(roundTripped.extras.callout[1].y, 0.9);
        QCOMPARE(roundTripped.extras.style.intent, Mu::Model::AnnotationIntent::FreeTextCallout);
    }

    void importsThreePointFreeTextCallout()
    {
        const QByteArray xml =
            R"(<xfdf><annots><freetext page="0" rect="20,60,60,80" callout="60,60,100,30,160,10"><contents>note</contents></freetext></annots></xfdf>)";
        QString error;
        const auto imported = Xfdf::xfdfToAnnotations(xml, { QSizeF(200, 100) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.applied, 1);
        const Annotation& annotation = imported.pages.first().annotations.first();
        QCOMPARE(annotation.extras.callout.size(), 3U);
        QCOMPARE(annotation.extras.style.intent, Mu::Model::AnnotationIntent::FreeTextCallout);
    }

    void rejectsMalformedFreeTextCallout()
    {
        const QStringList malformed {
            QStringLiteral("1,2,3"),
            QStringLiteral("1,2,3,4,5,6,7,8"),
            QStringLiteral("1,").repeated(100000),
        };
        for (const QString& callout : malformed) {
            const QByteArray xml =
                QStringLiteral(
                    "<xfdf><annots><freetext page=\"0\" rect=\"20,60,60,80\" callout=\"%1\"/></annots></xfdf>")
                    .arg(callout)
                    .toUtf8();
            QString error;
            const auto imported = Xfdf::xfdfToAnnotations(xml, { QSizeF(200, 100) }, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(imported.applied, 0);
            QCOMPARE(imported.skipped, 1);
        }
    }

    void rejectsCalloutWithIncompatibleIntent()
    {
        const QByteArray xml =
            R"(<xfdf><annots><freetext page="0" rect="20,60,60,80" callout="60,60,160,10" intent="FreeTextTypewriter"/></annots></xfdf>)";
        QString error;
        const auto imported = Xfdf::xfdfToAnnotations(xml, { QSizeF(200, 100) }, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(imported.applied, 0);
        QCOMPARE(imported.skipped, 1);
        QVERIFY(imported.warnings.first().contains(QStringLiteral("callout intent")));
    }

    void doesNotExportCalloutWithIncompatibleIntent()
    {
        Page page;
        page.widthPoints = 200;
        page.heightPoints = 100;
        Annotation annotation = baseAnnotation(Mu::Model::AnnotationType::FreeText, 0.1, 0.2, 0.3, 0.4);
        annotation.extras.callout = { { 0.3, 0.4 }, { 0.8, 0.9 } };
        annotation.extras.style.intent = Mu::Model::AnnotationIntent::FreeTextTypewriter;
        page.annotations.append(annotation);

        const QString xfdf = Xfdf::annotationsToXfdf({ page });

        QVERIFY(!xfdf.contains(QStringLiteral("callout=")));
        QVERIFY(xfdf.contains(QStringLiteral("intent=\"FreeTextTypewriter\"")));
    }

    void emitsLineEndpointsAndEndingStyles()
    {
        auto* page = makePage();
        auto* annotation = new Okular::LineAnnotation();
        annotation->setLinePoints({ Okular::NormalizedPoint(0.1, 0.1), Okular::NormalizedPoint(0.2, 0.2) });
        annotation->setLineStartStyle(Okular::LineAnnotation::Square);
        annotation->setLineEndStyle(Okular::LineAnnotation::ClosedArrow);
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("start=\"20,90\" end=\"40,80\" tail=\"Square\" head=\"ClosedArrow\"")));
        // The rect spans both endpoints rather than the padded worker bound.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,80,40,90\"")));
        QVERIFY(!xfdf.contains(QStringLiteral(" points=")));
    }

    void emitsPolylineAndPolygonVertices()
    {
        auto* page = makePage();
        auto* polyline = new Okular::LineAnnotation();
        polyline->setLinePoints({ Okular::NormalizedPoint(0.1, 0.1),
                                  Okular::NormalizedPoint(0.2, 0.2),
                                  Okular::NormalizedPoint(0.3, 0.3) });
        polyline->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.3, 0.3));
        page->addAnnotation(polyline);
        auto* polygon = new Okular::LineAnnotation();
        polygon->setLinePoints({ Okular::NormalizedPoint(0.1, 0.1),
                                 Okular::NormalizedPoint(0.2, 0.2),
                                 Okular::NormalizedPoint(0.3, 0.3) });
        polygon->setLineClosed(true);
        polygon->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.3, 0.3));
        page->addAnnotation(polygon);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        const QString vertices = QStringLiteral("<vertices>20,90;40,80;60,70</vertices>");
        QCOMPARE(xfdf.count(QStringLiteral("<polyline ")), 1);
        QCOMPARE(xfdf.count(QStringLiteral("<polygon ")), 1);
        QCOMPARE(xfdf.count(vertices), 2);
        // Both shapes share the same vertex bounds: x 0.1..0.3, y 0.1..0.3.
        QCOMPARE(xfdf.count(QStringLiteral("rect=\"20,70,60,90\"")), 2);
    }

    void emitsFreeTextDefaultAppearanceWithEscapedFontName()
    {
        auto* page = makePage();
        auto* annotation = new Okular::TextAnnotation();
        annotation->setTextType(Okular::TextAnnotation::InPlace);
        QFont font(QStringLiteral("DejaVu Sans"));
        font.setPointSizeF(12);
        annotation->setTextFont(font);
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.2, 0.5, 0.6));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<defaultappearance>0 0 0 rg /DejaVu#20Sans 12 Tf</defaultappearance>")));
    }

    void emitsInkGestures()
    {
        auto* page = makePage();
        auto* annotation = new Okular::InkAnnotation();
        QList<Okular::NormalizedPoint> firstPath { Okular::NormalizedPoint(0.1, 0.1),
                                                   Okular::NormalizedPoint(0.2, 0.2) };
        QList<Okular::NormalizedPoint> secondPath { Okular::NormalizedPoint(0.2, 0.3),
                                                    Okular::NormalizedPoint(0.3, 0.4) };
        annotation->setInkPaths({ firstPath, secondPath });
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
        page->addAnnotation(annotation);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(xfdf.contains(QStringLiteral("<ink")));
        QCOMPARE(xfdf.count(QStringLiteral("<inklist>")), 1);
        QCOMPARE(xfdf.count(QStringLiteral("<gesture>")), 2);
        QVERIFY(xfdf.contains(
            QStringLiteral("<inklist><gesture>20,90;40,80</gesture><gesture>40,70;60,60</gesture></inklist>")));
        // The rect spans every ink point: x 0.1..0.3, y 0.1..0.4.
        QVERIFY(xfdf.contains(QStringLiteral("rect=\"20,60,60,90\"")));
    }

    void generatedXfdfIsWellFormedXml()
    {
        auto* page = makePage();
        auto* annotation = new Okular::InkAnnotation();
        annotation->setInkPaths({ { Okular::NormalizedPoint(0.1, 0.1), Okular::NormalizedPoint(0.2, 0.2) },
                                  { Okular::NormalizedPoint(0.2, 0.3), Okular::NormalizedPoint(0.3, 0.4) } });
        annotation->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.3, 0.4));
        page->addAnnotation(annotation);

        QXmlStreamReader reader(generatorXfdf({ page }));
        while (!reader.atEnd())
            reader.readNext();
        delete page;

        QVERIFY2(!reader.hasError(), qPrintable(reader.errorString()));
    }

    void skipsUnsupportedSubtypes()
    {
        auto* page = makePage();
        // A file attachment has no XFDF mapping and must not be emitted.
        auto* attachment = new Okular::FileAttachmentAnnotation();
        attachment->setBoundingRectangle(Okular::NormalizedRect(0.1, 0.1, 0.2, 0.2));
        page->addAnnotation(attachment);

        const QString xfdf = generatorXfdf({ page });
        delete page;

        QVERIFY(!xfdf.contains(QStringLiteral("fileattachment")));
        QVERIFY(!xfdf.contains(QStringLiteral("page=\"0\"")));
    }
};

QTEST_MAIN(TestGeneratorXfdf)
#include "test_xfdf.moc"
