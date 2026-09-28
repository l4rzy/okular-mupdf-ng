// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// mupdfng-cli: thin command-line adapter over the plugin worker bridge.
// All worker IPC lives in Mu::Plugin::WorkerClient; this file only maps CLI
// commands onto its blocking calls and prints the results.

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QSaveFile>
#include <QSizeF>
#include <QTextStream>
#include <QTimer>

#include "generator/config/settings.hpp"
#include "plugin/util/document_type.hpp"
#include "plugin/worker_client.hpp"
#include "plugin/xfdf/export.hpp"
#include "plugin/xfdf/import.hpp"
#include "shared/compat.hpp"
#include "tools/cli/cli_args.hpp"

#include <utility>

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

QString buildTimeMupdfVersion()
{
    return QString::fromUtf8(::Mu::MUPDF_VERSION.data(), static_cast<qsizetype>(::Mu::MUPDF_VERSION.size()));
}

/// Queries the worker handshake for the runtime MuPDF engine version. Returns an
/// empty string when the worker cannot be started.
QString workerEngineVersion()
{
    Mu::Plugin::WorkerClient client;
    if (!client.start(QString()))
        return { };
    const QString version = QString::fromStdString(client.engineVersion());
    client.stop();
    return version;
}

int runVersion()
{
    const QString version = workerEngineVersion();
    if (!version.isEmpty()) {
        out() << versionText(version, QStringLiteral("Worker"));
        out().flush();
        return ExitOk;
    }
    err() << "mupdfng-cli: could not start the worker; showing build-time version\n";
    out() << versionText(buildTimeMupdfVersion(), QString());
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
                       Mu::Model::DocumentType type,
                       QList<Mu::Model::PageInfo>* pageInfo = nullptr)
{
    QList<Mu::Model::PageInfo> openedPages;
    QList<Mu::Model::PageInfo>& pages = pageInfo ? *pageInfo : openedPages;
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
    if (options.useLayout) {
        // Refresh the KConfigXT singleton and build worker-facing settings from
        // the same persisted EPUB layout Okular uses. No session paper color is
        // available here, so default to white.
        Mu::Generator::Config::reloadSettings();
        const Mu::Model::DocumentSettings settings =
            Mu::Generator::Config::readWorkerSettings().documentSettings(0xFFFFFF);
        if (!client.setSettings(settings)) {
            err() << "mupdfng-cli: failed to apply EPUB layout settings\n";
            return ExitJobFailed;
        }
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
    // withReferences=true selects the EPUB export path (metadata, links, TOC),
    // matching Main::exportTo in the generator; false would be a plain page copy.
    if (!client.savePdfToFile(QString::fromStdString(options.output), pages, /*withReferences=*/true)) {
        err() << "mupdfng-cli: export failed\n";
        return ExitJobFailed;
    }
    out() << "exported " << QString::fromStdString(options.output) << '\n';
    out().flush();
    return ExitOk;
}

int runExportXfdf(Mu::Plugin::WorkerClient& client, const ExportXfdfOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Pdf) {
        err() << "mupdfng-cli: export-xfdf only supports PDF documents\n";
        return ExitJobFailed;
    }

    QList<Mu::Model::PageInfo> pageInfo;
    if (openDocument(
            client, file, QString::fromStdString(options.shared.password), Mu::Model::DocumentType::Pdf, &pageInfo)
        < 0)
        return ExitJobFailed;

    QVector<Mu::Plugin::Xfdf::Page> pages;
    pages.reserve(pageInfo.size());
    for (const Mu::Model::PageInfo& page : pageInfo) {
        Mu::Plugin::Xfdf::Page xfdfPage;
        xfdfPage.widthPoints = page.geometry.widthPoints;
        xfdfPage.heightPoints = page.geometry.heightPoints;
        xfdfPage.annotations.reserve(static_cast<qsizetype>(page.annotations.size()));
        for (const Mu::Model::Annotation& annotation : page.annotations)
            xfdfPage.annotations.append(annotation);
        pages.append(std::move(xfdfPage));
    }

    QSaveFile output(QString::fromStdString(options.output));
    if (!output.open(QIODevice::WriteOnly)) {
        err() << "mupdfng-cli: could not open output file\n";
        return ExitJobFailed;
    }
    const QByteArray data = Mu::Plugin::Xfdf::annotationsToXfdf(pages).toUtf8();
    if (output.write(data) != data.size() || !output.commit()) {
        err() << "mupdfng-cli: XFDF export failed\n";
        return ExitJobFailed;
    }
    out() << "exported " << QString::fromStdString(options.output) << '\n';
    out().flush();
    return ExitOk;
}

int runApplyXfdf(Mu::Plugin::WorkerClient& client, const ApplyXfdfOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Pdf) {
        err() << "mupdfng-cli: apply-xfdf only supports PDF documents\n";
        return ExitJobFailed;
    }

    QFile xfdfFile(QString::fromStdString(options.xfdf));
    if (!xfdfFile.open(QIODevice::ReadOnly)) {
        err() << "mupdfng-cli: could not open " << QString::fromStdString(options.xfdf) << "\n";
        return ExitJobFailed;
    }
    if (xfdfFile.size() > Mu::Plugin::Xfdf::MaxXfdfBytes) {
        err() << "mupdfng-cli: XFDF file is too large\n";
        return ExitJobFailed;
    }
    const QByteArray xfdf = xfdfFile.readAll();

    QList<Mu::Model::PageInfo> pageInfo;
    if (openDocument(
            client, file, QString::fromStdString(options.shared.password), Mu::Model::DocumentType::Pdf, &pageInfo)
        < 0)
        return ExitJobFailed;

    QVector<QSizeF> pageSizes;
    Mu::Plugin::Xfdf::XfdfParseLimits parseLimits;
    std::size_t existingAnnotations = 0;
    pageSizes.reserve(pageInfo.size());
    parseLimits.annotationsPerPageRemaining.reserve(pageInfo.size());
    for (const Mu::Model::PageInfo& page : pageInfo) {
        pageSizes.append(QSizeF(page.geometry.widthPoints, page.geometry.heightPoints));
        const std::size_t existingOnPage = page.annotations.size();
        existingAnnotations += existingOnPage;
        parseLimits.annotationsPerPageRemaining.append(
            existingOnPage < Mu::Limit::MaxAnnotationsPerPage ? Mu::Limit::MaxAnnotationsPerPage - existingOnPage : 0);
    }
    parseLimits.annotationsRemaining = existingAnnotations < Mu::Limit::MaxAnnotationsPerDocument
        ? Mu::Limit::MaxAnnotationsPerDocument - existingAnnotations
        : 0;

    QString error;
    const Mu::Plugin::Xfdf::XfdfParseResult parsed =
        Mu::Plugin::Xfdf::xfdfToAnnotations(xfdf, pageSizes, &error, parseLimits);
    if (!error.isEmpty()) {
        err() << "mupdfng-cli: could not parse XFDF: " << error << "\n";
        return ExitJobFailed;
    }

    int applied = 0;
    int skipped = parsed.skipped;
    for (int page = 0; page < parsed.pages.size(); ++page) {
        for (const Mu::Model::Annotation& annotation : parsed.pages.at(page).annotations) {
            if (client.addAnnotation(page, annotation))
                ++applied;
            else
                ++skipped;
        }
    }

    for (const QString& warning : parsed.warnings)
        err() << "mupdfng-cli: " << warning << "\n";

    if (applied == 0) {
        err() << "mupdfng-cli: no annotations applied (" << skipped << " skipped)\n";
        return ExitJobFailed;
    }
    if (!client.saveToFile(QString::fromStdString(options.output))) {
        err() << "mupdfng-cli: could not write " << QString::fromStdString(options.output) << "\n";
        return ExitJobFailed;
    }

    out() << "applied " << applied << " annotations";
    if (skipped > 0)
        out() << " (" << skipped << " skipped)";
    out() << " to " << QString::fromStdString(options.output) << '\n';
    out().flush();
    return ExitOk;
}

int run(const Command& command)
{
    QCoreApplication::setApplicationName(QStringLiteral("mupdfng-cli"));
    if (!command.error.empty())
        return failUsage(command);
    if (command.kind == Command::Kind::Help || command.helpRequested)
        return showHelp(command);
    if (command.kind == Command::Kind::Version)
        return runVersion();

    Mu::Plugin::WorkerClient client;
    const SharedOptions& shared = command.kind == Command::Kind::Ocr ? command.ocr.shared
        : command.kind == Command::Kind::ExportPdf                   ? command.exportPdf.shared
        : command.kind == Command::Kind::ExportXfdf                  ? command.exportXfdf.shared
                                                                     : command.applyXfdf.shared;
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
    else if (command.kind == Command::Kind::ExportXfdf)
        code = runExportXfdf(client, command.exportXfdf);
    else if (command.kind == Command::Kind::ApplyXfdf)
        code = runApplyXfdf(client, command.applyXfdf);

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
