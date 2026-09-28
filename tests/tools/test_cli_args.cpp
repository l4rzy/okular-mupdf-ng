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
            Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "--page", "3", "--lang", "deu", "--dpi", "150" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::Ocr);
        QCOMPARE(command.ocr.file, std::string("doc.pdf"));
        QCOMPARE(command.ocr.page, 3);
        QCOMPARE(command.ocr.language, std::string("deu"));
        QCOMPARE(command.ocr.dpi, 150);
    }

    void ocrDefaultsAreSane()
    {
        const Cli::Command command = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "--page", "0" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.ocr.language, std::string("eng"));
        QCOMPARE(command.ocr.dpi, 300);
        QCOMPARE(command.ocr.shared.timeoutSeconds, 120);
        QVERIFY(command.ocr.shared.worker.empty());
    }

    void ocrRequiresPage()
    {
        const Cli::Command missing = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf" });
        QCOMPARE(missing.kind, Cli::Command::Kind::Ocr);
        QVERIFY(!missing.error.empty());

        const Cli::Command negative = Cli::parseArgs({ "mupdfng-cli", "ocr", "doc.pdf", "--page", "-1" });
        QVERIFY(!negative.error.empty());

        const Cli::Command noFile = Cli::parseArgs({ "mupdfng-cli", "ocr", "--page", "0" });
        QVERIFY(!noFile.error.empty());
    }

    void parsesExportCommand()
    {
        const Cli::Command command =
            Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub", "-o", "out.pdf", "--pages", "0,2,5" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::ExportPdf);
        QCOMPARE(command.exportPdf.file, std::string("book.epub"));
        QCOMPARE(command.exportPdf.output, std::string("out.pdf"));
        QCOMPARE(command.exportPdf.pages, std::vector<int>({ 0, 2, 5 }));
    }

    void exportParsesRangesAndDefaults()
    {
        const Cli::Command ranged =
            Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub", "-o", "out.pdf", "--pages", "1-3,5" });
        QVERIFY(ranged.error.empty());
        QCOMPARE(ranged.exportPdf.pages, std::vector<int>({ 1, 2, 3, 5 }));

        const Cli::Command all = Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub", "-o", "out.pdf" });
        QVERIFY(all.error.empty());
        QVERIFY(all.exportPdf.pages.empty());
        QVERIFY(!all.exportPdf.useLayout);
    }

    void exportParsesUseLayout()
    {
        const Cli::Command command =
            Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub", "-o", "out.pdf", "--use-layout" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::ExportPdf);
        QVERIFY(command.exportPdf.useLayout);
    }

    void parsesExportXfdfCommand()
    {
        const Cli::Command command = Cli::parseArgs(
            { "mupdfng-cli", "export-xfdf", "doc.pdf", "-o", "out.xfdf", "--password", "secret", "--timeout", "30" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::ExportXfdf);
        QCOMPARE(command.exportXfdf.file, std::string("doc.pdf"));
        QCOMPARE(command.exportXfdf.output, std::string("out.xfdf"));
        QCOMPARE(command.exportXfdf.shared.password, std::string("secret"));
        QCOMPARE(command.exportXfdf.shared.timeoutSeconds, 30);
    }

    void exportXfdfRequiresInputAndOutput()
    {
        const Cli::Command missingOutput = Cli::parseArgs({ "mupdfng-cli", "export-xfdf", "doc.pdf" });
        QCOMPARE(missingOutput.kind, Cli::Command::Kind::ExportXfdf);
        QVERIFY(!missingOutput.error.empty());

        const Cli::Command missingInput = Cli::parseArgs({ "mupdfng-cli", "export-xfdf", "-o", "out.xfdf" });
        QVERIFY(!missingInput.error.empty());

        const Cli::Command help = Cli::parseArgs({ "mupdfng-cli", "export-xfdf", "--help" });
        QCOMPARE(help.kind, Cli::Command::Kind::ExportXfdf);
        QVERIFY(help.helpRequested);
        QVERIFY(help.error.empty());
    }

    void parsesApplyXfdfCommand()
    {
        const Cli::Command command = Cli::parseArgs({ "mupdfng-cli",
                                                      "apply-xfdf",
                                                      "doc.pdf",
                                                      "--xfdf",
                                                      "notes.xfdf",
                                                      "-o",
                                                      "out.pdf",
                                                      "--password",
                                                      "secret",
                                                      "--timeout",
                                                      "45" });
        QVERIFY(command.error.empty());
        QCOMPARE(command.kind, Cli::Command::Kind::ApplyXfdf);
        QCOMPARE(command.applyXfdf.file, std::string("doc.pdf"));
        QCOMPARE(command.applyXfdf.xfdf, std::string("notes.xfdf"));
        QCOMPARE(command.applyXfdf.output, std::string("out.pdf"));
        QCOMPARE(command.applyXfdf.shared.password, std::string("secret"));
        QCOMPARE(command.applyXfdf.shared.timeoutSeconds, 45);
    }

    void applyXfdfRequiresInputXfdfAndOutput()
    {
        const Cli::Command missingXfdf = Cli::parseArgs({ "mupdfng-cli", "apply-xfdf", "doc.pdf", "-o", "out.pdf" });
        QCOMPARE(missingXfdf.kind, Cli::Command::Kind::ApplyXfdf);
        QVERIFY(!missingXfdf.error.empty());

        const Cli::Command missingOutput =
            Cli::parseArgs({ "mupdfng-cli", "apply-xfdf", "doc.pdf", "--xfdf", "notes.xfdf" });
        QVERIFY(!missingOutput.error.empty());

        const Cli::Command missingInput =
            Cli::parseArgs({ "mupdfng-cli", "apply-xfdf", "--xfdf", "notes.xfdf", "-o", "out.pdf" });
        QVERIFY(!missingInput.error.empty());

        const Cli::Command help = Cli::parseArgs({ "mupdfng-cli", "apply-xfdf", "--help" });
        QCOMPARE(help.kind, Cli::Command::Kind::ApplyXfdf);
        QVERIFY(help.helpRequested);
        QVERIFY(help.error.empty());
    }

    void exportRequiresOutput()
    {
        const Cli::Command missing = Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub" });
        QCOMPARE(missing.kind, Cli::Command::Kind::ExportPdf);
        QVERIFY(!missing.error.empty());

        const Cli::Command badPages =
            Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub", "-o", "out.pdf", "--pages", "3-1" });
        QVERIFY(!badPages.error.empty());

        const Cli::Command unknown =
            Cli::parseArgs({ "mupdfng-cli", "export-pdf", "book.epub", "-o", "out.pdf", "--bogus", "x" });
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

        const Cli::Command exportHelp = Cli::parseArgs({ "mupdfng-cli", "export-pdf", "--help" });
        QCOMPARE(exportHelp.kind, Cli::Command::Kind::ExportPdf);
        QVERIFY(exportHelp.helpRequested);
        QVERIFY(exportHelp.error.empty());

        const Cli::Command unknown = Cli::parseArgs({ "mupdfng-cli", "frobnicate" });
        QVERIFY(!unknown.error.empty());

        QVERIFY(!Cli::helpTextFor(Cli::Command::Kind::Help).isEmpty());
        QVERIFY(!Cli::helpTextFor(Cli::Command::Kind::Ocr).isEmpty());
        QVERIFY(!Cli::helpTextFor(Cli::Command::Kind::ExportPdf).isEmpty());
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::ExportPdf).contains(QStringLiteral("--use-layout")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::Help).contains(QStringLiteral("export-xfdf")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::Help).contains(QStringLiteral("apply-xfdf")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::ExportXfdf).contains(QStringLiteral("--password")));
        QVERIFY(Cli::helpTextFor(Cli::Command::Kind::ApplyXfdf).contains(QStringLiteral("--xfdf")));
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
