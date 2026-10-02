// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>

#include "shared/compat.hpp"
#include "tools/cli/cli_args.hpp"

namespace Cli = Mu::Tools::Cli;

class TestToolsCliArgs : public QObject {
    Q_OBJECT

private slots:

    void parsesOcrCommand()
    {
        const Cli::Command command =
            Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "3", "--lang", "deu", "--dpi", "150" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::Ocr);
        QCOMPARE(command.ocr.file, std::string("doc.pdf"));
        QCOMPARE(command.ocr.page, 3);
        QCOMPARE(command.ocr.language, std::string("deu"));
        QCOMPARE(command.ocr.dpi, 150);
    }

    void ocrDefaultsAreSane()
    {
        const Cli::Command command = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.ocr.page, 0);
        QCOMPARE(command.ocr.language, std::string("eng"));
        QCOMPARE(command.ocr.dpi, 300);
        QCOMPARE(command.ocr.shared.timeoutSeconds, 120);
        QVERIFY(command.ocr.shared.worker.empty());
        QVERIFY(command.ocr.output.empty());
    }

    void ocrParsesOutputFile()
    {
        const Cli::Command command = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "2", "-o", "out.txt" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::Ocr);
        QCOMPARE(command.ocr.page, 2);
        QCOMPARE(command.ocr.output, std::string("out.txt"));
    }

    void ocrValidatesPage()
    {
        const Cli::Command missing = Cli::parseArgs({ "mupdfng-cli", "ocr" });
        QCOMPARE(missing.kind, Cli::Command::Kind::Ocr);
        QVERIFY(!missing.error.empty());

        const Cli::Command invalid = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "banana" });
        QVERIFY(!invalid.error.empty());

        const Cli::Command negative = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "-1" });
        QVERIFY(!negative.error.empty());
    }

    void parsesExportCommand()
    {
        const Cli::Command command =
            Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "out.pdf", "--pages", "0,2,5" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::Export);
        QCOMPARE(command.exportOptions.file, std::string("book.epub"));
        QCOMPARE(command.exportOptions.output, std::string("out.pdf"));
        QCOMPARE(command.exportOptions.format, Cli::ExportFormat::Pdf);
        QCOMPARE(command.exportOptions.pages, std::vector<int>({ 0, 2, 5 }));
    }

    void exportInfersFormatFromSuffix()
    {
        const Cli::Command xfdf = Cli::parseArgs({ "mupdfng-cli", "export", "doc.pdf", "-o", "out.xfdf" });
        QVERIFY(xfdf.error.empty());
        QCOMPARE(xfdf.exportOptions.format, Cli::ExportFormat::Xfdf);

        const Cli::Command upper = Cli::parseArgs({ "mupdfng-cli", "export", "doc.pdf", "out.XML" });
        QVERIFY(upper.error.empty());
        QCOMPARE(upper.exportOptions.format, Cli::ExportFormat::Xfdf);

        const Cli::Command unknown = Cli::parseArgs({ "mupdfng-cli", "export", "doc.pdf", "out.txt" });
        QCOMPARE(unknown.kind, Cli::Command::Kind::Export);
        QVERIFY(!unknown.error.empty());
    }

    void exportParsesRangesAndDefaults()
    {
        const Cli::Command ranged =
            Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "out.pdf", "--pages", "1-3,5" });
        QVERIFY(ranged.error.empty());
        QCOMPARE(ranged.exportOptions.pages, std::vector<int>({ 1, 2, 3, 5 }));

        const Cli::Command all = Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "-o", "out.pdf" });
        QVERIFY(all.error.empty());
        QVERIFY(all.exportOptions.pages.empty());
        QVERIFY(!all.exportOptions.useLayout);
    }

    void exportParsesUseLayout()
    {
        const Cli::Command command =
            Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "out.pdf", "--use-layout" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::Export);
        QVERIFY(command.exportOptions.useLayout);
    }

    void exportFlattenOptions_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::addColumn<bool>("valid");
        QTest::newRow("default") << QStringList { "doc.pdf", "out.pdf" } << true;
        QTest::newRow("flatten") << QStringList { "doc.pdf", "out.pdf", "--flatten" } << true;
        QTest::newRow("selected") << QStringList { "doc.pdf", "out.pdf", "--flatten", "--pages", "0,2-4" } << true;
        QTest::newRow("xfdf") << QStringList { "doc.pdf", "out.xfdf", "--flatten" } << false;
        QTest::newRow("layout") << QStringList { "doc.pdf", "out.pdf", "--flatten", "--use-layout" } << false;
    }

    void exportFlattenOptions()
    {
        QFETCH(QStringList, arguments);
        QFETCH(bool, valid);
        QStringList argv { "mupdfng-cli", "export" };
        argv.append(arguments);
        const auto command = Cli::parseArgs(argv);
        QCOMPARE(command.error.empty(), valid);
        QCOMPARE(command.exportOptions.flatten, arguments.contains("--flatten"));
    }

    void parsesImportCommand()
    {
        const Cli::Command command = Cli::parseArgs(
            { "mupdfng-cli", "import", "doc.pdf", "notes.xfdf", "out.pdf", "--password", "secret", "--timeout", "45" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::Import);
        QCOMPARE(command.importOptions.file, std::string("doc.pdf"));
        QCOMPARE(command.importOptions.xfdf, std::string("notes.xfdf"));
        QCOMPARE(command.importOptions.output, std::string("out.pdf"));
        QCOMPARE(command.importOptions.shared.password, std::string("secret"));
        QCOMPARE(command.importOptions.shared.timeoutSeconds, 45);
    }

    void importRequiresOperands()
    {
        const Cli::Command missingXfdf = Cli::parseArgs({ "mupdfng-cli", "import", "doc.pdf", "-o", "out.pdf" });
        QCOMPARE(missingXfdf.kind, Cli::Command::Kind::Import);
        QVERIFY(!missingXfdf.error.empty());

        const Cli::Command missingOutput = Cli::parseArgs({ "mupdfng-cli", "import", "doc.pdf", "notes.xfdf" });
        QVERIFY(!missingOutput.error.empty());

        const Cli::Command missingInput = Cli::parseArgs({ "mupdfng-cli", "import", "-o", "out.pdf" });
        QVERIFY(!missingInput.error.empty());

        const Cli::Command help = Cli::parseArgs({ "mupdfng-cli", "import", "--help" });
        QCOMPARE(help.kind, Cli::Command::Kind::Import);
        QVERIFY(help.helpRequested);
        QVERIFY(help.error.empty());
    }

    void exportRequiresExactlyOneOutput()
    {
        const Cli::Command missing = Cli::parseArgs({ "mupdfng-cli", "export", "book.epub" });
        QCOMPARE(missing.kind, Cli::Command::Kind::Export);
        QVERIFY(!missing.error.empty());

        const Cli::Command twice = Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "a.pdf", "-o", "b.pdf" });
        QVERIFY(!twice.error.empty());

        const Cli::Command badPages =
            Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "out.pdf", "--pages", "3-1" });
        QVERIFY(!badPages.error.empty());

        const Cli::Command unknown =
            Cli::parseArgs({ "mupdfng-cli", "export", "book.epub", "out.pdf", "--bogus", "x" });
        QVERIFY(!unknown.error.empty());
    }

    void helpVersionAndUnknownCommands()
    {
        QCOMPARE(Cli::parseArgs({ "mupdfng-cli" }).kind, Cli::Command::Kind::Help);
        QCOMPARE(Cli::parseArgs({ "mupdfng-cli", "--help" }).kind, Cli::Command::Kind::Help);
        QVERIFY(!Cli::parseArgs({ "mupdfng-cli" }).error.empty());

        const Cli::Command ocrHelp = Cli::parseArgs({ "mupdfng-cli", "ocr", "--help" });
        QCOMPARE(ocrHelp.kind, Cli::Command::Kind::Ocr);
        QVERIFY(ocrHelp.helpRequested);
        QVERIFY(ocrHelp.error.empty());

        const Cli::Command exportHelp = Cli::parseArgs({ "mupdfng-cli", "export", "--help" });
        QCOMPARE(exportHelp.kind, Cli::Command::Kind::Export);
        QVERIFY(exportHelp.helpRequested);
        QVERIFY(exportHelp.error.empty());

        const Cli::Command unknown = Cli::parseArgs({ "mupdfng-cli", "frobnicate" });
        QVERIFY(!unknown.error.empty());

        QVERIFY(!Cli::helpTextFor(Cli::Command::Kind::Help).isEmpty());
        QVERIFY(!Cli::helpTextFor(Cli::Command::Kind::Ocr).isEmpty());
        QVERIFY(!Cli::helpTextFor(Cli::Command::Kind::Export).isEmpty());
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::Export).contains(QStringLiteral("--use-layout")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::Help).contains(QStringLiteral("export FILE")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::Help).contains(QStringLiteral("import FILE")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::Import).contains(QStringLiteral("xfdf")));
    }

    void versionFlagAndText()
    {
        for (const QString& flag : { QStringLiteral("--version"), QStringLiteral("-V") }) {
            const Cli::Command command = Cli::parseArgs({ "mupdfng-cli", flag });
            QCOMPARE(command.kind, Cli::Command::Kind::Version);
            QVERIFY(command.error.empty());
        }
        // Tracks the manifest and the supplied engine version without hardcoding them.
        const QString engineVersion = QString::fromUtf8(::Mu::MUPDF_VERSION.data());
        const QString text = Cli::versionText(engineVersion, QStringLiteral("Worker"));
        QVERIFY(text.startsWith(QStringLiteral("mupdfng-cli ")));
        QVERIFY(text.contains(QString::fromUtf8(::Mu::IPC::COMPAT.data())));
        QVERIFY(text.contains(QStringLiteral("Worker: MuPDF")));
        QVERIFY(text.contains(engineVersion));
    }

    void parsesPageLists()
    {
        std::vector<int> pages;
        std::string error;
        QVERIFY(Cli::parsePageList("", pages, error));
        QVERIFY(pages.empty());
        QVERIFY(Cli::parsePageList("4", pages, error));
        QCOMPARE(pages, std::vector<int>({ 4 }));
        QVERIFY(Cli::parsePageList("0,2-4", pages, error));
        QCOMPARE(pages, std::vector<int>({ 0, 2, 3, 4 }));
        QVERIFY(!Cli::parsePageList("a", pages, error));
        QVERIFY(!Cli::parsePageList("2-", pages, error));
        QVERIFY(!Cli::parsePageList("1,,2", pages, error));
    }
};

QTEST_GUILESS_MAIN(TestToolsCliArgs)

#include "test_cli_args.moc"
