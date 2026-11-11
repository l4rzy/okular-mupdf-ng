// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/model/export.hpp"
#include "generator/conversion/text.hpp"
#include "generator/conversion/xfdf.hpp"
#include "generator/mupdfng.hpp"
#include <KLocalizedString>
#include <QIcon>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QTextStream>
#include <limits>
#include <okular/core/document.h>

namespace Mu::Generator {
namespace {

constexpr int NoticeMs = 3000;

QIcon exportIcon(const QMimeType& mime)
{
    const QIcon fallback = QIcon::fromTheme(mime.genericIconName(), QIcon::fromTheme(QStringLiteral("text-x-generic")));
    const QString iconName = mime.inherits(QStringLiteral("application/vnd.amazon.mobi8-ebook"))
        ? QStringLiteral("application-x-mobi8-ebook")
        : mime.iconName();
    return QIcon::fromTheme(iconName, fallback);
}

struct ExportAction {
    KLocalizedString label;
    const char* mime;
    const char* format;
    Model::ExportProfileId profile;
    KLocalizedString success;
    bool asynchronous = false;
};

// This is the Okular presentation of engine profiles. Engine capabilities decide
// availability; labels and completion notices stay in the UI adapter.
const ExportAction WorkerExports[] {
    { ki18n("PDF"),
      "application/pdf",
      "pdf",
      Model::ExportProfileId::SourceWithReferences,
      ki18n("Export to PDF finished."),
      true },
    { ki18n("Flattened PDF"),
      "application/pdf",
      "pdf",
      Model::ExportProfileId::Flattened,
      ki18n("Export to flattened PDF finished.") },
    { ki18n("ePUB"),
      "application/epub+zip",
      "epub",
      Model::ExportProfileId::Converted,
      ki18n("Export to ePUB finished.") },
    { ki18n("Kindle (AZW3)"),
      "application/vnd.amazon.mobi8-ebook",
      "azw3",
      Model::ExportProfileId::Converted,
      ki18n("Export to AZW3 finished.") },
    { ki18n("Markdown"),
      "text/markdown",
      "md",
      Model::ExportProfileId::Converted,
      ki18n("Export to Markdown finished.") },
};

bool supportsExport(const std::vector<Model::OutputFormatInfo>& formats, const ExportAction& action)
{
    return Model::findExportProfile(formats,
                                    { action.format, action.profile },
                                    action.asynchronous,
                                    std::numeric_limits<std::int32_t>::max())
        != nullptr;
}

} // namespace

// Okular Generator Func: reports the supported export formats.
Okular::ExportFormat::List Main::exportFormats() const
{
    Okular::ExportFormat::List formats { Okular::ExportFormat::standardFormat(Okular::ExportFormat::PlainText) };
    if (const auto available = m_worker.outputFormats()) {
        for (const auto& action : WorkerExports) {
            if (!supportsExport(*available, action))
                continue;
            if (action.profile == Model::ExportProfileId::SourceWithReferences) {
                formats.append(Okular::ExportFormat::standardFormat(Okular::ExportFormat::PDF));
                continue;
            }
            const auto mime = QMimeDatabase().mimeTypeForName(QString::fromLatin1(action.mime));
            if (mime.isValid())
                formats.append(Okular::ExportFormat(exportIcon(mime), action.label.toString(), mime));
        }
    }
    if (m_document.type == Model::DocumentType::Pdf) {
        // Standard PDF annotations can be dumped to an XFDF interchange file.
        // Advertised as application/xml because XFDF lacks a portable MIME
        // registration on many systems; the file dialog then filters *.xml.
        const QMimeType xml = QMimeDatabase().mimeTypeForName(QStringLiteral("application/xml"));
        if (xml.isValid())
            formats.append(Okular::ExportFormat(exportIcon(xml), i18n("Annotation File (XFDF)"), xml));
    }
    return formats;
}

// Okular Generator Func: exports the document to a selected format.
bool Main::exportTo(const QString& fileName, const Okular::ExportFormat& format)
{
    if (!workerReady())
        return false;
    const auto available = m_worker.outputFormats();
    if (!available)
        return false;
    for (const auto& action : WorkerExports) {
        if (!format.mimeType().inherits(QString::fromLatin1(action.mime)) || !supportsExport(*available, action))
            continue;
        const Model::ExportOptions options { action.format, action.profile };
        if (action.asynchronous) {
            Q_EMIT notice(i18n("Starting to export to PDF."), NoticeMs);
            return m_worker.startExport(fileName, options).has_value();
        }
        const bool success = m_worker.exportDocument(fileName, options);
        if (success)
            Q_EMIT notice(action.success.toString(), NoticeMs);
        return success;
    }

    if (format.mimeType().name() == QLatin1String("application/xml")) {
        // Annotations are exported from the live Okular pages, so unsaved
        // in-session changes are included. Only page-rotation validation needs
        // a worker round trip; Okular's page orientation is already normalized.
        if (m_document.type != Model::DocumentType::Pdf)
            return false;
        const auto info = m_worker.getDocumentInfo({ QStringLiteral("hasRotatedPages") });
        const auto rotated = info.values.find("hasRotatedPages");
        if (rotated == info.values.end() || rotated->second != "false") {
            Q_EMIT error(rotated != info.values.end() && rotated->second == "true"
                             ? i18n("XFDF export does not support rotated PDF pages.")
                             : i18n("Could not check PDF page rotation for XFDF export."),
                         NoticeMs);
            return false;
        }
        const QByteArray data = Conversion::annotationsToXfdf(m_okularPages, dpi()).toUtf8();
        // QSaveFile keeps the destination intact until the write fully succeeds.
        QSaveFile file(fileName);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        const bool success = file.write(data) == data.size() && file.commit();
        if (success)
            Q_EMIT notice(i18n("Export to XFDF finished."), NoticeMs);
        return success;
    }

    if (!format.mimeType().inherits(QStringLiteral("text/plain")) || !workerReady())
        return false;

    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    QTextStream stream(&file);
    const int pageCount = document() ? static_cast<int>(document()->pages()) : 0;
    for (int page = 0; page < pageCount; ++page) {
        if (!workerReady())
            return false;
        bool extracted = false;
        const auto boxes =
            m_worker.getTextBoxesForPage(page, dpi().width(), dpi().height(), /*skipAnnots=*/true, &extracted);
        if (!extracted)
            return false;
        stream << Conversion::plainText(boxes);
        if (page + 1 < pageCount)
            stream << QLatin1Char('\n');
        if (stream.status() != QTextStream::Ok)
            return false;
    }
    stream.flush();
    return stream.status() == QTextStream::Ok && file.commit();
}

} // namespace Mu::Generator
