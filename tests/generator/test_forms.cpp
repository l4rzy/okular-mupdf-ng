// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KConfigGroup>
#include <KSharedConfig>
#include <QMimeDatabase>
#include <QPrinter>
#include <QTemporaryDir>
#include <QTest>
#include <okular/core/document.h>
#include <okular/core/form.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

namespace {

template <typename Field> Field* findField(const Okular::Document& document, int page, const char* name)
{
    for (auto* field : document.page(page)->formFields()) {
        if (field->name() == QLatin1String(name))
            return dynamic_cast<Field*>(field);
    }
    return nullptr;
}

Okular::Document::OpenResult
openDocument(Okular::Document& document, const QString& path, const QString& password = { })
{
    return document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path), password);
}

} // namespace

class TestGeneratorForms : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", m_root.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", m_root.filePath("cache").toUtf8());
        QCoreApplication::setLibraryPaths({ QStringLiteral(TEST_PLUGIN_ROOT) });
        qputenv("PATH", QByteArray(TEST_WORKER_DIR) + ':' + qgetenv("PATH"));
        Okular::SettingsCore::instance(QStringLiteral("mupdfng-form-test"));
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        // Exercise the actual document in runners without a fully hardened sandbox.
        general.writeEntry("SandboxEnforcement", "Relaxed");
        general.writeEntry("PdfFormJavaScriptEnabled", false);
        KConfigGroup ocr(config, QStringLiteral("OCR"));
        ocr.writeEntry("OcrTriggerMode", "Never");
        config->sync();
    }

    void persistsTextEdits_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("password");
        QTest::addColumn<int>("page");
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QString>("value");
        QTest::newRow("plain") << QStringLiteral("application.pdf") << QString() << 0 << QByteArray("Name")
                               << QStringLiteral("Jane Smith");
        QTest::newRow("unicode") << QStringLiteral("application.pdf") << QString() << 0 << QByteArray("Name")
                                 << QStringLiteral("Zoë 李");
        QTest::newRow("maximum-length") << QStringLiteral("application.pdf") << QString() << 0 << QByteArray("Name")
                                        << QString(16, QChar(0x00e9));
        QTest::newRow("multiline-rotated") << QStringLiteral("application.pdf") << QString() << 1 << QByteArray("Notes")
                                           << QStringLiteral("First line\nDeuxième ligne");
        QTest::newRow("libreoffice-unicode") << QStringLiteral("libreoffice.pdf") << QString() << 0
                                             << QByteArray("Text Box 2") << QStringLiteral("Zoë 李");
        QTest::newRow("libreoffice-multiline")
            << QStringLiteral("libreoffice.pdf") << QString() << 0 << QByteArray("Text Box 1")
            << QStringLiteral("First line\nDeuxième ligne");
        QTest::newRow("encrypted") << QStringLiteral("encrypted.pdf") << QStringLiteral("forms-test") << 0
                                   << QByteArray("Name") << QStringLiteral("Encrypted edit");
    }

    void persistsTextEdits()
    {
        QFETCH(QString, file);
        QFETCH(QString, password);
        QFETCH(int, page);
        QFETCH(QByteArray, name);
        QFETCH(QString, value);
        Okular::Document document(nullptr);
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/") + file, password),
                 Okular::Document::OpenSuccess);
        QVERIFY(document.canSaveChanges(Okular::Document::SaveFormsCapability));
        auto* field = findField<Okular::FormFieldText>(document, page, name.constData());
        QVERIFY(field);
        if (page == 1) {
            QCOMPARE(field->textType(), Okular::FormFieldText::Multiline);
            QVERIFY(document.page(page)->width() > document.page(page)->height());
        }
        const QString original = field->text();
        document.editFormText(page, field, value, static_cast<int>(value.size()), 0, 0, original);
        QCOMPARE(field->text(), value);
        QVERIFY(document.canUndo());
        document.undo();
        QCOMPARE(field->text(), original);
        QVERIFY(document.canRedo());
        document.redo();
        QCOMPARE(field->text(), value);

        const QString saved = m_root.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".pdf");
        QString error;
        QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
        document.closeDocument();
        QCOMPARE(openDocument(document, saved, password), Okular::Document::OpenSuccess);
        field = findField<Okular::FormFieldText>(document, page, name.constData());
        QVERIFY(field);
        QCOMPARE(field->text(), value);
        document.closeDocument();
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/") + file, password),
                 Okular::Document::OpenSuccess);
        field = findField<Okular::FormFieldText>(document, page, name.constData());
        QVERIFY(field);
        QCOMPARE(field->text(), original);
    }

    void rejectsTextEdits_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QString>("value");
        QTest::newRow("read-only") << QByteArray("Reference") << QStringLiteral("Changed");
        QTest::newRow("over-maximum-length") << QByteArray("Name") << QString(17, QChar(0x00e9));
    }

    void rejectsTextEdits()
    {
        QFETCH(QByteArray, name);
        QFETCH(QString, value);
        Okular::Document document(nullptr);
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/application.pdf")),
                 Okular::Document::OpenSuccess);
        auto* field = findField<Okular::FormFieldText>(document, 0, name.constData());
        QVERIFY(field);
        const QString original = field->text();
        if (name == "Reference")
            QVERIFY(field->isReadOnly());
        else
            QCOMPARE(field->maximumLength(), 16);
        field->setText(value);
        QCOMPARE(field->text(), original);
        const QString saved = m_root.filePath(QString::fromLatin1(QTest::currentDataTag()) + ".pdf");
        QString error;
        QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
        document.closeDocument();
        QCOMPARE(openDocument(document, saved), Okular::Document::OpenSuccess);
        field = findField<Okular::FormFieldText>(document, 0, name.constData());
        QVERIFY(field);
        QCOMPARE(field->text(), original);
    }

    void persistsButtonsAndChoices()
    {
        Okular::Document document(nullptr);
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/application.pdf")),
                 Okular::Document::OpenSuccess);
        auto* agree = findField<Okular::FormFieldButton>(document, 0, "Agree");
        auto* email = findField<Okular::FormFieldButton>(document, 0, "Delivery");
        auto* post = findField<Okular::FormFieldButton>(document, 1, "Delivery");
        auto* languages = findField<Okular::FormFieldChoice>(document, 0, "Languages");
        auto* city = findField<Okular::FormFieldChoice>(document, 0, "City");
        QVERIFY(agree && email && post && languages && city);
        QVERIFY(!agree->state());
        QVERIFY(email->state());
        QVERIFY(!post->state());
        QVERIFY(languages->multiSelect());
        QVERIFY(city->isEditable());

        document.editFormButtons(0, { agree }, { true });
        QVERIFY(agree->state());
        document.undo();
        QVERIFY(!agree->state());
        document.redo();
        QVERIFY(agree->state());
        document.editFormButtons(1, { email, post }, { false, true });
        QVERIFY(!email->state());
        QVERIFY(post->state());
        document.undo();
        QVERIFY(email->state());
        QVERIFY(!post->state());
        document.redo();
        QVERIFY(!email->state());
        QVERIFY(post->state());
        document.editFormList(0, languages, { 0, 2 });
        QCOMPARE(languages->currentChoices(), QList<int>({ 0, 2 }));
        document.undo();
        QCOMPARE(languages->currentChoices(), QList<int>({ 0 }));
        document.redo();
        QCOMPARE(languages->currentChoices(), QList<int>({ 0, 2 }));
        const QString customCity = QStringLiteral("Montréal");
        document.editFormCombo(0, city, customCity, static_cast<int>(customCity.size()), 0, 0);
        QCOMPARE(city->editChoice(), customCity);
        document.undo();
        QCOMPARE(city->currentChoices(), QList<int>({ 0 }));
        document.redo();
        QCOMPARE(city->editChoice(), customCity);

        const QString saved = m_root.filePath("buttons-choices.pdf");
        QString error;
        QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
        document.closeDocument();
        QCOMPARE(openDocument(document, saved), Okular::Document::OpenSuccess);
        agree = findField<Okular::FormFieldButton>(document, 0, "Agree");
        email = findField<Okular::FormFieldButton>(document, 0, "Delivery");
        post = findField<Okular::FormFieldButton>(document, 1, "Delivery");
        languages = findField<Okular::FormFieldChoice>(document, 0, "Languages");
        city = findField<Okular::FormFieldChoice>(document, 0, "City");
        QVERIFY(agree && email && post && languages && city);
        QVERIFY(agree->state());
        QVERIFY(!email->state());
        QVERIFY(post->state());
        QCOMPARE(languages->currentChoices(), QList<int>({ 0, 2 }));
        QCOMPARE(city->editChoice(), customCity);
    }

    void persistsLibreOfficeChoices()
    {
        Okular::Document document(nullptr);
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/libreoffice.pdf")),
                 Okular::Document::OpenSuccess);
        auto* list = findField<Okular::FormFieldChoice>(document, 0, "List Box 1_2");
        auto* combo = findField<Okular::FormFieldChoice>(document, 0, "Combo Box 1");
        QVERIFY(list && combo);
        QVERIFY(list->multiSelect());
        QVERIFY(combo->isEditable());
        document.editFormList(0, list, { 0, 2 });
        QCOMPARE(list->currentChoices(), QList<int>({ 0, 2 }));
        const QString custom = QStringLiteral("Montréal");
        document.editFormCombo(0, combo, custom, static_cast<int>(custom.size()), 0, 0);
        QCOMPARE(combo->editChoice(), custom);
        const QString saved = m_root.filePath("libreoffice-choices.pdf");
        QString error;
        QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
        document.closeDocument();
        QCOMPARE(openDocument(document, saved), Okular::Document::OpenSuccess);
        list = findField<Okular::FormFieldChoice>(document, 0, "List Box 1_2");
        combo = findField<Okular::FormFieldChoice>(document, 0, "Combo Box 1");
        QVERIFY(list && combo);
        QCOMPARE(list->currentChoices(), QList<int>({ 0, 2 }));
        QCOMPARE(combo->editChoice(), custom);
    }

    void resetsWithoutJavaScript()
    {
        Okular::Document document(nullptr);
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/application.pdf")),
                 Okular::Document::OpenSuccess);
        auto* name = findField<Okular::FormFieldText>(document, 0, "Name");
        auto* reset = findField<Okular::FormFieldButton>(document, 0, "Reset");
        QVERIFY(name && reset);
        name->setText(QStringLiteral("Changed"));
        QCOMPARE(name->text(), QStringLiteral("Changed"));
        QVERIFY(reset->activationAction());
        document.processAction(reset->activationAction());
        QCOMPARE(name->text(), QStringLiteral("Original"));
    }

    void printsUnsavedValues()
    {
        Okular::Document document(nullptr);
        QCOMPARE(openDocument(document, QStringLiteral(TEST_FORM_DIR "/application.pdf")),
                 Okular::Document::OpenSuccess);
        auto* name = findField<Okular::FormFieldText>(document, 0, "Name");
        QVERIFY(name);
        const QString value = QStringLiteral("Printed value");
        document.editFormText(0, name, value, static_cast<int>(value.size()), 0, 0, name->text());
        QCOMPARE(name->text(), value);
        const QString printed = m_root.filePath("printed.pdf");
        QPrinter printer(QPrinter::HighResolution);
        printer.setOutputFormat(QPrinter::PdfFormat);
        printer.setOutputFileName(printed);
        QCOMPARE(document.print(printer), Okular::Document::NoPrintError);
        QCOMPARE(name->text(), value);
        document.closeDocument();
        QCOMPARE(openDocument(document, printed), Okular::Document::OpenSuccess);
        QVERIFY(document.page(0)->formFields().isEmpty());
        document.requestTextPage(0);
        QVERIFY(document.page(0)->text().contains(value));
    }
};

QTEST_MAIN(TestGeneratorForms)
#include "test_forms.moc"
