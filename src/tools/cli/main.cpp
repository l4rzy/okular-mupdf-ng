// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// mupdfng-cli: thin command-line adapter over the plugin worker bridge.
// All worker IPC lives in Mu::Plugin::WorkerClient; this file only maps CLI
// commands onto its blocking calls and prints the results.

#include <QCoreApplication>
#include <QEventLoop>
#include <QTextStream>
#include <QTimer>

#include "plugin/util/document_type.hpp"
#include "plugin/worker_client.hpp"
#include "tools/cli/cli_args.hpp"

#ifndef TESSDATA_DIR
#define TESSDATA_DIR "/usr/share/tessdata"
#endif

namespace {

using namespace Mu::Tools::Cli;

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream& err()
{
    static QTextStream stream(stderr);
    return stream;
}

int failUsage(const Command& command)
{
    if (!command.error.empty())
        err() << "mupdfng-cli: " << QString::fromStdString(command.error) << "\n\n";
    err() << helpTextFor(command.kind);
    return ExitUsage;
}

int showHelp(const Command& command)
{
    out() << helpTextFor(command.kind);
    out().flush();
    return ExitOk;
}

QString tessDataFor(const OcrOptions& options)
{
    if (!options.tessData.empty())
        return QString::fromStdString(options.tessData);
    return QStringLiteral(TESSDATA_DIR);
}

/// Opens the document through the client. Returns the page count, or -1.
qsizetype openDocument(Mu::Plugin::WorkerClient& client,
                       const QString& file,
                       const QString& password,
                       Mu::Model::DocumentType type)
{
    QList<Mu::Model::PageInfo> pages;
    const auto status = client.open(file, password, pages, type);
    if (status == Mu::Model::OpenStatus::NeedsPassword) {
        err() << "mupdfng-cli: document needs a password (--password)\n";
        return -1;
    }
    if (status != Mu::Model::OpenStatus::Success) {
        err() << "mupdfng-cli: could not open " << file << "\n";
        return -1;
    }
    return pages.size();
}

int runOcr(Mu::Plugin::WorkerClient& client, const OcrOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Pdf) {
        err() << "mupdfng-cli: OCR is only supported for PDF documents\n";
        return ExitJobFailed;
    }
    const qsizetype pageCount =
        openDocument(client, file, QString::fromStdString(options.shared.password), Mu::Model::DocumentType::Pdf);
    if (pageCount < 0)
        return ExitJobFailed;
    if (options.page >= pageCount) {
        err() << "mupdfng-cli: page " << options.page << " out of range (0.." << pageCount - 1 << ")\n";
        return ExitJobFailed;
    }

    const std::optional<quint64> jobId =
        client.startOcrPage(options.page, QString::fromStdString(options.language), options.dpi);
    if (!jobId) {
        err() << "mupdfng-cli: worker rejected the OCR job\n";
        return ExitJobFailed;
    }

    // The worker recognizes asynchronously; wait for its completion signal.
    QEventLoop loop;
    quint64 doneId = 0;
    QObject::connect(&client, &Mu::Plugin::WorkerClient::ocrDone, &loop, [&](quint64 id, int) {
        doneId = id;
        loop.quit();
    });
    QTimer::singleShot(options.shared.timeoutSeconds * 1000, &loop, &QEventLoop::quit);
    loop.exec();
    if (doneId != *jobId) {
        client.cancelOcrJobs();
        err() << "mupdfng-cli: OCR timed out\n";
        return ExitTimeout;
    }

    const Mu::Model::OcrResult result = client.ocrResult(*jobId);
    if (result.status != Mu::Model::OcrStatus::Success) {
        err() << "mupdfng-cli: OCR failed\n";
        return ExitJobFailed;
    }
    for (const auto& box : result.boxes)
        out() << box.left << ' ' << box.top << ' ' << box.right << ' ' << box.bottom << ' '
              << QString::fromStdString(box.text) << '\n';
    out().flush();
    return ExitOk;
}

int runExportPdf(Mu::Plugin::WorkerClient& client, const ExportPdfOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Epub) {
        err() << "mupdfng-cli: export-pdf only supports EPUB documents\n";
        return ExitJobFailed;
    }
    const qsizetype pageCount =
        openDocument(client, file, QString::fromStdString(options.shared.password), Mu::Model::DocumentType::Epub);
    if (pageCount < 0)
        return ExitJobFailed;
    QVector<int> pages;
    pages.reserve(static_cast<qsizetype>(options.pages.size()));
    for (int page : options.pages) {
        if (page >= pageCount) {
            err() << "mupdfng-cli: page " << page << " out of range (0.." << pageCount - 1 << ")\n";
            return ExitJobFailed;
        }
        pages.push_back(page);
    }
    if (!client.savePdfToFile(QString::fromStdString(options.output), pages)) {
        err() << "mupdfng-cli: export failed\n";
        return ExitJobFailed;
    }
    out() << "exported " << QString::fromStdString(options.output) << '\n';
    out().flush();
    return ExitOk;
}

int run(const Command& command)
{
    if (!command.error.empty())
        return failUsage(command);
    if (command.kind == Command::Kind::Help)
        return showHelp(command);
    if (command.kind == Command::Kind::Version) {
        out() << versionText();
        out().flush();
        return ExitOk;
    }

    QCoreApplication::setApplicationName(QStringLiteral("mupdfng-cli"));
    Mu::Plugin::WorkerClient client;
    const SharedOptions& shared = command.kind == Command::Kind::Ocr ? command.ocr.shared : command.exportPdf.shared;
    QStringList tessDirs;
    if (command.kind == Command::Kind::Ocr)
        tessDirs.push_back(tessDataFor(command.ocr));
    if (!client.start(QString::fromStdString(shared.worker), tessDirs)) {
        err() << "mupdfng-cli: could not start the worker process\n";
        return ExitJobFailed;
    }

    int code = ExitJobFailed;
    if (command.kind == Command::Kind::Ocr)
        code = runOcr(client, command.ocr);
    else if (command.kind == Command::Kind::ExportPdf)
        code = runExportPdf(client, command.exportPdf);

    client.close();
    client.stop();
    return code;
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const Command command = Mu::Tools::Cli::parseArgs(application.arguments());
    return run(command);
}
