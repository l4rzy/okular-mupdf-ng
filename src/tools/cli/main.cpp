// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

// mupdfng-cli: thin command-line adapter over the plugin worker bridge.
// All worker IPC lives in Mu::Plugin::WorkerClient; this file only maps CLI
// commands onto its blocking calls and prints the results.

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
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

#include <cstdio>
#include <unistd.h>
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

/// Tagged status lines: [INFO]/[OK] go to stdout, [WARN]/[ERROR] to stderr.
/// Tags are colorized only when their stream is a terminal that wants color;
/// piped output (scripts, tests) stays plain. OCR data lines stay bare so
/// scripts can parse them; every successful command still ends with an [OK]
/// trailer.
bool streamSupportsColor(FILE* stream)
{
    return ::isatty(::fileno(stream)) != 0 && qEnvironmentVariableIsEmpty("NO_COLOR") && qgetenv("TERM") != "dumb";
}

QString colorTag(const QString& tag, const char* code, FILE* stream)
{
    if (!streamSupportsColor(stream))
        return tag;
    return QStringLiteral("\033[%1m%2\033[0m").arg(QLatin1String(code), tag);
}

void info(const QString& message)
{
    out() << colorTag(QStringLiteral("[INFO]"), "34", stdout) << ' ' << message << '\n';
}

void ok(const QString& message)
{
    out() << colorTag(QStringLiteral("[OK]"), "32", stdout) << ' ' << message << '\n';
}

void warn(const QString& message)
{
    // Bold yellow renders as orange on standard terminal themes; plain red
    // would collide with [ERROR].
    err() << colorTag(QStringLiteral("[WARN]"), "1;33", stderr) << ' ' << message << '\n';
}

void reportError(const QString& message)
{
    err() << colorTag(QStringLiteral("[ERROR]"), "31", stderr) << ' ' << message << '\n';
}

QString plural(int count, const QString& singular, const QString& pluralForm)
{
    return count == 1 ? QStringLiteral("1 %1").arg(singular) : QStringLiteral("%1 %2").arg(count).arg(pluralForm);
}

int failUsage(const Command& command)
{
    if (!command.error.empty())
        reportError(QString::fromStdString(command.error) + QLatin1Char('\n'));
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
    warn(QStringLiteral("could not start the worker; showing build-time version"));
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
        reportError(QStringLiteral("document needs a password (--password)"));
        return -1;
    }
    if (status != Mu::Model::OpenStatus::Success) {
        reportError(QStringLiteral("could not open %1").arg(file));
        return -1;
    }
    return pages.size();
}

int runOcr(Mu::Plugin::WorkerClient& client, const OcrOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Pdf) {
        reportError(QStringLiteral("OCR is only supported for PDF documents"));
        return ExitJobFailed;
    }
    const QString output = QString::fromStdString(options.output);
    // Never clobber the source document: QSaveFile replaces atomically.
    if (!output.isEmpty() && output != QLatin1String("-") && !QFileInfo(output).canonicalFilePath().isEmpty()
        && QFileInfo(output).canonicalFilePath() == QFileInfo(file).canonicalFilePath()) {
        reportError(QStringLiteral("output must not be the input file"));
        return ExitJobFailed;
    }
    const qsizetype pageCount =
        openDocument(client, file, QString::fromStdString(options.shared.password), Mu::Model::DocumentType::Pdf);
    if (pageCount < 0)
        return ExitJobFailed;
    if (options.page >= pageCount) {
        reportError(QStringLiteral("page %1 out of range (0..%2)").arg(options.page).arg(pageCount - 1));
        return ExitJobFailed;
    }

    const std::optional<quint64> jobId =
        client.startOcrPage(options.page, QString::fromStdString(options.language), options.dpi);
    if (!jobId) {
        reportError(QStringLiteral("worker rejected the OCR job"));
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
        reportError(QStringLiteral("OCR timed out"));
        return ExitTimeout;
    }

    const Mu::Model::OcrResult result = client.ocrResult(*jobId);
    if (result.status != Mu::Model::OcrStatus::Success) {
        reportError(QStringLiteral("OCR failed"));
        return ExitJobFailed;
    }
    // Text-only lines, one per box in worker order; embedded line breaks are
    // flattened so the output stays one line per box.
    QString recognized;
    for (const auto& box : result.boxes) {
        QString line = QString::fromStdString(box.text);
        line.replace(QLatin1Char('\n'), QLatin1Char(' '));
        line.replace(QLatin1Char('\r'), QLatin1Char(' '));
        recognized += line;
    }
    const QString recognizedCount =
        plural(static_cast<int>(result.boxes.size()), QStringLiteral("text box"), QStringLiteral("text boxes"));
    if (output.isEmpty() || output == QLatin1String("-")) {
        out() << recognized;
        ok(QStringLiteral("Recognized %1 on page %2").arg(recognizedCount).arg(options.page));
        out().flush();
        return ExitOk;
    }
    QSaveFile target(output);
    if (!target.open(QIODevice::WriteOnly)) {
        reportError(QStringLiteral("could not open output file"));
        return ExitJobFailed;
    }
    const QByteArray data = recognized.toUtf8();
    if (target.write(data) != data.size() || !target.commit()) {
        reportError(QStringLiteral("could not write %1").arg(output));
        return ExitJobFailed;
    }
    ok(QStringLiteral("Recognized %1 on page %2, wrote %3").arg(recognizedCount).arg(options.page).arg(output));
    out().flush();
    return ExitOk;
}

int runExportPdf(Mu::Plugin::WorkerClient& client, const ExportOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    const auto type = options.flatten ? Mu::Model::DocumentType::Pdf : Mu::Model::DocumentType::Epub;
    if (Mu::Plugin::Util::documentTypeForFile(file) != type) {
        reportError(options.flatten
                        ? QStringLiteral("flattened PDF export only supports PDF documents")
                        : QStringLiteral("PDF export only supports EPUB documents; use --flatten for PDF input"));
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
            reportError(QStringLiteral("failed to apply EPUB layout settings"));
            return ExitJobFailed;
        }
        info(QStringLiteral("Applied Okular EPUB layout settings"));
    }
    const qsizetype pageCount = openDocument(client, file, QString::fromStdString(options.shared.password), type);
    if (pageCount < 0)
        return ExitJobFailed;
    QVector<int> pages;
    pages.reserve(static_cast<qsizetype>(options.pages.size()));
    for (int page : options.pages) {
        if (page >= pageCount) {
            reportError(QStringLiteral("page %1 out of range (0..%2)").arg(page).arg(pageCount - 1));
            return ExitJobFailed;
        }
        pages.push_back(page);
    }
    info(QStringLiteral("Exporting %1 from %2")
             .arg(plural(static_cast<int>(pages.isEmpty() ? pageCount : pages.size()),
                         QStringLiteral("page"),
                         QStringLiteral("pages")))
             .arg(file));
    const QString output = QString::fromStdString(options.output);
    const bool success =
        options.flatten ? client.flattenPdfToFile(output, pages) : client.savePdfToFile(output, pages, true);
    if (!success) {
        reportError(QStringLiteral("export failed"));
        return ExitJobFailed;
    }
    ok(QStringLiteral("Exported %1 to %2")
           .arg(plural(static_cast<int>(pages.isEmpty() ? pageCount : pages.size()),
                       QStringLiteral("page"),
                       QStringLiteral("pages")))
           .arg(QString::fromStdString(options.output)));
    out().flush();
    return ExitOk;
}

int runExportXfdf(Mu::Plugin::WorkerClient& client, const ExportOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Pdf) {
        reportError(QStringLiteral("XFDF export only supports PDF documents"));
        return ExitJobFailed;
    }

    QList<Mu::Model::PageInfo> pageInfo;
    if (openDocument(
            client, file, QString::fromStdString(options.shared.password), Mu::Model::DocumentType::Pdf, &pageInfo)
        < 0)
        return ExitJobFailed;

    QVector<Mu::Plugin::Xfdf::Page> pages;
    pages.reserve(pageInfo.size());
    qsizetype found = 0;
    for (const Mu::Model::PageInfo& page : pageInfo) {
        Mu::Plugin::Xfdf::Page xfdfPage;
        xfdfPage.widthPoints = page.geometry.widthPoints;
        xfdfPage.heightPoints = page.geometry.heightPoints;
        xfdfPage.annotations.reserve(static_cast<qsizetype>(page.annotations.size()));
        for (const Mu::Model::Annotation& annotation : page.annotations)
            xfdfPage.annotations.append(annotation);
        found += page.annotations.size();
        pages.append(std::move(xfdfPage));
    }
    info(QStringLiteral("Found %1 in %2")
             .arg(plural(static_cast<int>(found), QStringLiteral("annotation"), QStringLiteral("annotations")))
             .arg(file));

    QSaveFile output(QString::fromStdString(options.output));
    if (!output.open(QIODevice::WriteOnly)) {
        reportError(QStringLiteral("could not open output file"));
        return ExitJobFailed;
    }
    const QByteArray data = Mu::Plugin::Xfdf::annotationsToXfdf(pages).toUtf8();
    if (output.write(data) != data.size() || !output.commit()) {
        reportError(QStringLiteral("XFDF export failed"));
        return ExitJobFailed;
    }
    ok(QStringLiteral("Exported %1 to %2")
           .arg(plural(static_cast<int>(found), QStringLiteral("annotation"), QStringLiteral("annotations")))
           .arg(QString::fromStdString(options.output)));
    out().flush();
    return ExitOk;
}

int runImport(Mu::Plugin::WorkerClient& client, const ImportOptions& options)
{
    const QString file = QString::fromStdString(options.file);
    if (Mu::Plugin::Util::documentTypeForFile(file) != Mu::Model::DocumentType::Pdf) {
        reportError(QStringLiteral("import only supports PDF documents"));
        return ExitJobFailed;
    }

    const QString xfdfPath = QString::fromStdString(options.xfdf);
    QFile xfdfFile(xfdfPath);
    if (!xfdfFile.open(QIODevice::ReadOnly)) {
        reportError(QStringLiteral("could not open %1").arg(xfdfPath));
        return ExitJobFailed;
    }
    if (xfdfFile.size() > Mu::Plugin::Xfdf::MaxXfdfBytes) {
        reportError(QStringLiteral("XFDF file is too large"));
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
        reportError(QStringLiteral("could not parse XFDF: %1").arg(error));
        return ExitJobFailed;
    }
    if (parsed.skipped > 0)
        info(QStringLiteral("Parsed %1 from %2 (%3 skipped)")
                 .arg(plural(parsed.applied, QStringLiteral("annotation"), QStringLiteral("annotations")))
                 .arg(xfdfPath)
                 .arg(parsed.skipped));
    else
        info(QStringLiteral("Parsed %1 from %2")
                 .arg(plural(parsed.applied, QStringLiteral("annotation"), QStringLiteral("annotations")))
                 .arg(xfdfPath));

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
        warn(warning);

    if (applied == 0) {
        reportError(QStringLiteral("no annotations applied (%1 skipped)").arg(skipped));
        return ExitJobFailed;
    }
    if (!client.saveToFile(QString::fromStdString(options.output))) {
        reportError(QStringLiteral("could not write %1").arg(QString::fromStdString(options.output)));
        return ExitJobFailed;
    }

    if (skipped > 0)
        ok(QStringLiteral("Applied %1 to %2 (%3 skipped)")
               .arg(plural(applied, QStringLiteral("annotation"), QStringLiteral("annotations")))
               .arg(QString::fromStdString(options.output))
               .arg(skipped));
    else
        ok(QStringLiteral("Applied %1 to %2")
               .arg(plural(applied, QStringLiteral("annotation"), QStringLiteral("annotations")))
               .arg(QString::fromStdString(options.output)));
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
        : command.kind == Command::Kind::Export                      ? command.exportOptions.shared
                                                                     : command.importOptions.shared;
    QStringList tessDirs;
    if (command.kind == Command::Kind::Ocr)
        tessDirs.push_back(tessDataFor(command.ocr));
    if (!client.start(QString::fromStdString(shared.worker), tessDirs)) {
        reportError(QStringLiteral("could not start the worker process"));
        return ExitJobFailed;
    }

    int code = ExitJobFailed;
    if (command.kind == Command::Kind::Ocr)
        code = runOcr(client, command.ocr);
    else if (command.kind == Command::Kind::Export) {
        if (command.exportOptions.format == ExportFormat::Pdf)
            code = runExportPdf(client, command.exportOptions);
        else
            code = runExportXfdf(client, command.exportOptions);
    } else if (command.kind == Command::Kind::Import)
        code = runImport(client, command.importOptions);

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
