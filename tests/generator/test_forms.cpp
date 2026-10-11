// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <KConfigGroup>
#include <KSharedConfig>
#include <QDir>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QPrinter>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <okular/core/document.h>
#include <okular/core/form.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

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

// Non-dumpable workers can hide /proc/<pid>/exe. Identify our private control
// endpoint instead; SO_PEERCRED supplies its PID without inspecting the process.
pid_t workerPid()
{
    const QString prefix = QStringLiteral("worker-%1-").arg(QCoreApplication::applicationPid());
    for (const auto& name : QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot)) {
        bool valid = false;
        const int fd = name.toInt(&valid);
        sockaddr_un address { };
        socklen_t size = sizeof(address);
        if (!valid || ::getpeername(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0
            || address.sun_family != AF_UNIX)
            continue;
        const QString endpoint =
            QFileInfo(
                QString::fromLocal8Bit(address.sun_path,
                                       static_cast<qsizetype>(::strnlen(address.sun_path, sizeof(address.sun_path)))))
                .fileName();
        if (!endpoint.startsWith(prefix) || !endpoint.endsWith(QLatin1String("-ctrl.sock")))
            continue;
        ucred peer { };
        size = sizeof(peer);
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) == 0 && peer.pid > 0 && peer.pid != ::getpid()
            && peer.uid == ::geteuid())
            return peer.pid;
    }
    return -1;
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

    void discardsUnsavedEditsOnWorkerRecovery_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("password");
        QTest::addColumn<bool>("pendingRedo");
        QTest::addColumn<bool>("savedBaseline");
        QTest::addColumn<bool>("javascript");
        QTest::newRow("mixed-fields") << QStringLiteral("application.pdf") << QString() << false << false << false;
        QTest::newRow("pending-redo") << QStringLiteral("application.pdf") << QString() << true << false << false;
        QTest::newRow("encrypted") << QStringLiteral("encrypted.pdf") << QStringLiteral("forms-test") << false << false
                                   << false;
        QTest::newRow("saved-baseline") << QStringLiteral("application.pdf") << QString() << false << true << false;
        QTest::newRow("javascript-dependency")
            << QStringLiteral("javascript.pdf") << QString() << false << false << true;
    }

    void discardsUnsavedEditsOnWorkerRecovery()
    {
        QFETCH(QString, file);
        QFETCH(QString, password);
        QFETCH(bool, pendingRedo);
        QFETCH(bool, savedBaseline);
        QFETCH(bool, javascript);
#if !TEST_FORM_JAVASCRIPT
        if (javascript)
            QSKIP("PDF form JavaScript support is disabled in this configuration");
#endif
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("PdfFormJavaScriptEnabled", javascript);
        config->sync();
        Okular::Document document(nullptr);
        const auto restoreJavaScript = qScopeGuard([&] {
            document.closeDocument();
            KConfigGroup(config, QStringLiteral("General")).writeEntry("PdfFormJavaScriptEnabled", false);
            config->sync();
            document.reparseConfig();
        });
        const QString originalSource = QStringLiteral(TEST_FORM_DIR "/") + file;
        QString source = originalSource;
        QCOMPARE(openDocument(document, source, password), Okular::Document::OpenSuccess);
        document.reparseConfig();
        const char* fieldName = javascript ? "Quantity" : "Name";
        auto* field = findField<Okular::FormFieldText>(document, 0, fieldName);
        QVERIFY(field);
        const QString original = field->text();
        QString baseline = original;
        QString error;
        if (savedBaseline) {
            baseline = QStringLiteral("Saved baseline");
            document.editFormText(0, field, baseline, static_cast<int>(baseline.size()), 0, 0, original);
            source = m_root.filePath(QStringLiteral("recovery-baseline.pdf"));
            QVERIFY2(document.saveChanges(source, &error), qPrintable(error));
            document.closeDocument();
            QCOMPARE(openDocument(document, source), Okular::Document::OpenSuccess);
            field = findField<Okular::FormFieldText>(document, 0, fieldName);
            QVERIFY(field);
            QCOMPARE(field->text(), baseline);
        }
        auto* dependent = javascript ? findField<Okular::FormFieldText>(document, 1, "Total") : nullptr;
        auto* agree = javascript ? nullptr : findField<Okular::FormFieldButton>(document, 0, "Agree");
        auto* email = javascript ? nullptr : findField<Okular::FormFieldButton>(document, 0, "Delivery");
        auto* post = javascript ? nullptr : findField<Okular::FormFieldButton>(document, 1, "Delivery");
        auto* list = javascript ? nullptr : findField<Okular::FormFieldChoice>(document, 0, "Languages");
        auto* combo = javascript ? nullptr : findField<Okular::FormFieldChoice>(document, 0, "City");
        auto* notes = javascript ? nullptr : findField<Okular::FormFieldText>(document, 1, "Notes");
        QVERIFY(javascript ? dependent != nullptr : agree && email && post && list && combo && notes);
        const QString originalNotes = notes ? notes->text() : QString();
        const QString edit = javascript ? QStringLiteral("4") : QStringLiteral("Unsaved edit");
        const QString editedNotes = QStringLiteral("Unsaved\nrotated page");
        const QString editedCity = QStringLiteral("Montréal");
        QSignalSpy warnings(&document, &Okular::Document::warning);
        const auto checkCleanControls = [&] {
            QVERIFY(agree && email && post && list && combo && notes);
            QVERIFY(!agree->state());
            QVERIFY(email->state());
            QVERIFY(!post->state());
            QCOMPARE(list->currentChoices(), QList<int>({ 0 }));
            QCOMPARE(combo->currentChoices(), QList<int>({ 0 }));
            QCOMPARE(notes->text(), originalNotes);
        };

        // Repeat on the same live proxies to catch stale handles after another restart.
        for (int cycle = 0; cycle < 2; ++cycle) {
            document.editFormText(0, field, edit, static_cast<int>(edit.size()), 0, 0, baseline);
            QCOMPARE(field->text(), edit);
            if (javascript) {
                QCOMPARE(dependent->text(), QStringLiteral("72.00"));
            } else {
                document.editFormButtons(0, { agree }, { true });
                document.editFormButtons(1, { email, post }, { false, true });
                document.editFormList(0, list, { 0, 2 });
                document.editFormCombo(0, combo, editedCity, static_cast<int>(editedCity.size()), 0, 0);
                document.editFormText(1, notes, editedNotes, static_cast<int>(editedNotes.size()), 0, 0, originalNotes);
                QVERIFY(agree->state());
                QVERIFY(!email->state());
                QVERIFY(post->state());
                QCOMPARE(list->currentChoices(), QList<int>({ 0, 2 }));
                QCOMPARE(combo->editChoice(), editedCity);
                QCOMPARE(notes->text(), editedNotes);
            }
            QVERIFY(document.canUndo());
            QVERIFY(!document.isHistoryClean());
            if (pendingRedo) {
                document.undo();
                QCOMPARE(notes->text(), originalNotes);
                QVERIFY(document.canRedo());
            }
            warnings.clear();
            const pid_t pid = workerPid();
            QVERIFY(pid > 0);
            QCOMPARE(::kill(pid, SIGKILL), 0);
            const auto warned = [&] {
                for (const auto& warning : warnings)
                    if (warning.front().toString().contains(QStringLiteral("unsaved form changes"))
                        && warning.front().toString().contains(QStringLiteral("Those changes were lost")))
                        return true;
                return false;
            };
            QTRY_VERIFY_WITH_TIMEOUT(warned(), 10000);
            QTRY_VERIFY_WITH_TIMEOUT(!document.canUndo() && !document.canRedo(), 10000);
            QVERIFY(document.isHistoryClean());
            QCOMPARE(findField<Okular::FormFieldText>(document, 0, fieldName), field);
            QCOMPARE(field->text(), baseline);
            if (javascript) {
                QCOMPARE(dependent->text(), QStringLiteral("54.00"));
            } else {
                checkCleanControls();
                QVERIFY(!QTest::currentTestFailed());
                QVERIFY(combo->editChoice().isEmpty());
            }
        }

        // Saving immediately after recovery must contain clean source values.
        const QString recovered = m_root.filePath(QString::fromLatin1(QTest::currentDataTag()) + "-recovered.pdf");
        QVERIFY2(document.saveChanges(recovered, &error), qPrintable(error));
        // Existing proxies must support new edits and history after their worker handles change.
        document.editFormText(0, field, edit, static_cast<int>(edit.size()), 0, 0, baseline);
        QCOMPARE(field->text(), edit);
        if (javascript)
            QCOMPARE(dependent->text(), QStringLiteral("72.00"));
        document.undo();
        QCOMPARE(field->text(), baseline);
        if (javascript)
            QCOMPARE(dependent->text(), QStringLiteral("54.00"));
        document.redo();
        QCOMPARE(field->text(), edit);
        if (javascript) {
            QCOMPARE(dependent->text(), QStringLiteral("72.00"));
            document.editFormText(0, field, QStringLiteral("-1"), 2, 0, 0, edit);
            QCOMPARE(field->text(), edit);
            QCOMPARE(dependent->text(), QStringLiteral("72.00"));
        }
        const QString saved = m_root.filePath(QString::fromLatin1(QTest::currentDataTag()) + "-after-recovery.pdf");
        QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
        document.closeDocument();
        QStringList paths { recovered, saved, source, originalSource };
        paths.removeDuplicates();
        for (const auto& path : paths) {
            QCOMPARE(openDocument(document, path, password), Okular::Document::OpenSuccess);
            field = findField<Okular::FormFieldText>(document, 0, fieldName);
            QVERIFY(field);
            QCOMPARE(field->text(), path == saved ? edit : path == originalSource ? original : baseline);
            if (javascript) {
                dependent = findField<Okular::FormFieldText>(document, 1, "Total");
                QVERIFY(dependent);
                QCOMPARE(dependent->text(), path == saved ? QStringLiteral("72.00") : QStringLiteral("54.00"));
            } else {
                agree = findField<Okular::FormFieldButton>(document, 0, "Agree");
                email = findField<Okular::FormFieldButton>(document, 0, "Delivery");
                post = findField<Okular::FormFieldButton>(document, 1, "Delivery");
                list = findField<Okular::FormFieldChoice>(document, 0, "Languages");
                combo = findField<Okular::FormFieldChoice>(document, 0, "City");
                notes = findField<Okular::FormFieldText>(document, 1, "Notes");
                checkCleanControls();
                QVERIFY(!QTest::currentTestFailed());
            }
            document.closeDocument();
        }
    }

    void persistsJavaScriptEdits_data()
    {
        QTest::addColumn<QByteArray>("name");
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("total");
        QTest::addColumn<bool>("accepted");
        QTest::newRow("quantity") << QByteArray("Quantity") << QStringLiteral("4") << QStringLiteral("72.00") << true;
        QTest::newRow("unit-price") << QByteArray("UnitPrice") << QStringLiteral("25") << QStringLiteral("67.50")
                                    << true;
        QTest::newRow("discount") << QByteArray("DiscountPercent") << QStringLiteral("20") << QStringLiteral("48.00")
                                  << true;
        QTest::newRow("zero-quantity") << QByteArray("Quantity") << QStringLiteral("0") << QStringLiteral("0.00")
                                       << true;
        QTest::newRow("no-discount") << QByteArray("DiscountPercent") << QStringLiteral("0") << QStringLiteral("60.00")
                                     << true;
        QTest::newRow("full-discount") << QByteArray("DiscountPercent") << QStringLiteral("100")
                                       << QStringLiteral("0.00") << true;
        QTest::newRow("negative-quantity")
            << QByteArray("Quantity") << QStringLiteral("-1") << QStringLiteral("54.00") << false;
        QTest::newRow("negative-price") << QByteArray("UnitPrice") << QStringLiteral("-1") << QStringLiteral("54.00")
                                        << false;
        QTest::newRow("nonnumeric-quantity")
            << QByteArray("Quantity") << QStringLiteral("invalid") << QStringLiteral("54.00") << false;
        QTest::newRow("negative-discount")
            << QByteArray("DiscountPercent") << QStringLiteral("-1") << QStringLiteral("54.00") << false;
        QTest::newRow("excess-discount") << QByteArray("DiscountPercent") << QStringLiteral("101")
                                         << QStringLiteral("54.00") << false;
    }

    void persistsJavaScriptEdits()
    {
#if !TEST_FORM_JAVASCRIPT
        QSKIP("PDF form JavaScript support is disabled in this configuration");
#endif
        QFETCH(QByteArray, name);
        QFETCH(QString, value);
        QFETCH(QString, total);
        QFETCH(bool, accepted);
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup(config, QStringLiteral("General")).writeEntry("PdfFormJavaScriptEnabled", true);
        config->sync();
        Okular::Document document(nullptr);
        const auto restoreJavaScript = qScopeGuard([&] {
            KConfigGroup(config, QStringLiteral("General")).writeEntry("PdfFormJavaScriptEnabled", false);
            config->sync();
            document.reparseConfig();
        });
        const QString source = QStringLiteral(TEST_FORM_DIR "/javascript.pdf");
        QCOMPARE(openDocument(document, source), Okular::Document::OpenSuccess);
        document.reparseConfig();
        QCOMPARE(document.pages(), 2u);
        auto* field = findField<Okular::FormFieldText>(document, 0, name.constData());
        auto* dependent = findField<Okular::FormFieldText>(document, 1, "Total");
        QVERIFY(field && dependent);
        QVERIFY(dependent->isReadOnly());
        const QString original = field->text();
        const QString originalTotal = QStringLiteral("54.00");
        QCOMPARE(dependent->text(), originalTotal);
        const QString expected = accepted ? value : original;

        document.editFormText(0, field, value, static_cast<int>(value.size()), 0, 0, original);
        QCOMPARE(field->text(), expected);
        QCOMPARE(dependent->text(), total);
        QVERIFY(document.canUndo());
        document.undo();
        QCOMPARE(field->text(), original);
        QCOMPARE(dependent->text(), originalTotal);
        QVERIFY(document.canRedo());
        document.redo();
        QCOMPARE(field->text(), expected);
        QCOMPARE(dependent->text(), total);

        const QString saved =
            m_root.filePath(QStringLiteral("javascript-") + QString::fromLatin1(QTest::currentDataTag()) + ".pdf");
        QString error;
        QVERIFY2(document.saveChanges(saved, &error), qPrintable(error));
        document.closeDocument();
        QCOMPARE(openDocument(document, saved), Okular::Document::OpenSuccess);
        field = findField<Okular::FormFieldText>(document, 0, name.constData());
        dependent = findField<Okular::FormFieldText>(document, 1, "Total");
        QVERIFY(field && dependent);
        QCOMPARE(field->text(), expected);
        QCOMPARE(dependent->text(), total);
        // The saved copy must retain its validation and calculation actions.
        const QString reopenedEdit = accepted ? original : value;
        document.editFormText(0, field, reopenedEdit, static_cast<int>(reopenedEdit.size()), 0, 0, expected);
        QCOMPARE(field->text(), original);
        QCOMPARE(dependent->text(), originalTotal);
        document.undo();
        QCOMPARE(field->text(), expected);
        QCOMPARE(dependent->text(), total);
        document.closeDocument();
        QCOMPARE(openDocument(document, source), Okular::Document::OpenSuccess);
        field = findField<Okular::FormFieldText>(document, 0, name.constData());
        dependent = findField<Okular::FormFieldText>(document, 1, "Total");
        QVERIFY(field && dependent);
        QCOMPARE(field->text(), original);
        QCOMPARE(dependent->text(), originalTotal);
    }
};

QTEST_MAIN(TestGeneratorForms)
#include "test_forms.moc"
