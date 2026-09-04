// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/settings.hpp"

#include <QDir>
#include <QMimeDatabase>

#include "mupdfsettings.h"
#include "plugin/caching/ocr_cache.hpp"

namespace Mu::Generator::Config {

namespace {

int graphicsAntialiasingBitsForConfig(int value) noexcept
{
    // KDE stores a named level; MuPDF consumes the corresponding bit count.
    switch (value) {
    case MuPDFSettings::EnumGraphicsAntialiasingBits::Disabled:
        return 0;
    case MuPDFSettings::EnumGraphicsAntialiasingBits::Minimum:
        return 2;
    case MuPDFSettings::EnumGraphicsAntialiasingBits::Low:
        return 4;
    case MuPDFSettings::EnumGraphicsAntialiasingBits::Medium:
        return 6;
    case MuPDFSettings::EnumGraphicsAntialiasingBits::High:
    default:
        return 8;
    }
}

int textAntialiasingBitsForConfig(int value) noexcept
{
    // Keep text and graphics mappings explicit because their generated enums
    // are independent even though their numeric levels currently match.
    switch (value) {
    case MuPDFSettings::EnumTextAntialiasingBits::Disabled:
        return 0;
    case MuPDFSettings::EnumTextAntialiasingBits::Minimum:
        return 2;
    case MuPDFSettings::EnumTextAntialiasingBits::Low:
        return 4;
    case MuPDFSettings::EnumTextAntialiasingBits::Medium:
        return 6;
    case MuPDFSettings::EnumTextAntialiasingBits::High:
    default:
        return 8;
    }
}

std::int64_t memoryCacheBytesForConfig(int value) noexcept
{
    // Convert the UI's MiB choice once, before sending byte units over IPC.
    switch (value) {
    case MuPDFSettings::EnumMemoryLimit::Size32MiB:
        return 32LL * 1024 * 1024;
    case MuPDFSettings::EnumMemoryLimit::Size128MiB:
        return 128LL * 1024 * 1024;
    case MuPDFSettings::EnumMemoryLimit::Size256MiB:
        return 256LL * 1024 * 1024;
    case MuPDFSettings::EnumMemoryLimit::Size64MiB:
    default:
        return 64LL * 1024 * 1024;
    }
}

} // namespace

void reloadSettings()
{
    // KConfigXT owns persistence; this only refreshes its generated singleton.
    MuPDFSettings::self()->read();
}

SandboxEnforcement readSandboxEnforcement()
{
    // Generated enums are ints; map through the generated constants so a
    // renamed or reordered choice cannot silently flip the security policy.
    switch (MuPDFSettings::sandboxEnforcement()) {
    case MuPDFSettings::EnumSandboxEnforcement::Strict:
        return SandboxEnforcement::Strict;
    case MuPDFSettings::EnumSandboxEnforcement::Relaxed:
    default:
        return SandboxEnforcement::Relaxed;
    }
}

EpubSettings readEpubSettings()
{
    // Keep custom CSS encoded exactly as stored; CssEditor owns the UI form.
    return { MuPDFSettings::epubFontSize(),
             MuPDFSettings::epubFontFamily(),
             MuPDFSettings::epubPageSize(),
             MuPDFSettings::epubCustomCss() };
}

WorkerSettings readWorkerSettings()
{
    // One reader keeps the session-scope pair coherent: rendering values are
    // normalized into worker-facing units; EPUB values stay encoded exactly
    // as stored (CssEditor owns the UI form).
    return {
        { graphicsAntialiasingBitsForConfig(MuPDFSettings::graphicsAntialiasingBits()),
          textAntialiasingBitsForConfig(MuPDFSettings::textAntialiasingBits()),
          static_cast<int>(MuPDFSettings::imageRenderingQuality()),
          MuPDFSettings::imageInterpolation(),
          memoryCacheBytesForConfig(MuPDFSettings::memoryLimit()) },
        readEpubSettings(),
    };
}

OcrSettings readOcrSettings()
{
    // Normalize the configured traineddata name to the cache/worker language
    // token and translate the trigger enum into explicit policy flags.
    // A "-" language means no models are installed: never fire OCR.
    OcrSettings settings;
    const QString language = MuPDFSettings::ocrLanguage();
    if (language == QStringLiteral("-") || language.isEmpty()) {
        settings.language = QStringLiteral("-");
        settings.dpi = static_cast<int>(Plugin::Caching::OCR::Cache::qualityToDpi(MuPDFSettings::ocrQuality()));
        settings.asynchronous = MuPDFSettings::ocrAsync();
        settings.notify = MuPDFSettings::ocrNotify();
        settings.force = false;
        settings.autoTrigger = false;
        return settings;
    }
    QString normalized = language;
    if (!normalized.endsWith(QStringLiteral(".traineddata")))
        normalized = QStringLiteral("eng.traineddata");
    settings.language = Plugin::Caching::OCR::Cache::stripLangSuffix(normalized);
    settings.dpi = static_cast<int>(Plugin::Caching::OCR::Cache::qualityToDpi(MuPDFSettings::ocrQuality()));
    settings.asynchronous = MuPDFSettings::ocrAsync();
    settings.notify = MuPDFSettings::ocrNotify();

    switch (MuPDFSettings::ocrTriggerMode()) {
    case MuPDFSettings::EnumOcrTriggerMode::Five:
        settings.autoTrigger = true;
        settings.triggerThreshold = 5;
        break;
    case MuPDFSettings::EnumOcrTriggerMode::Twenty:
        settings.autoTrigger = true;
        settings.triggerThreshold = 20;
        break;
    case MuPDFSettings::EnumOcrTriggerMode::Always:
        settings.force = true;
        break;
    case MuPDFSettings::EnumOcrTriggerMode::Never:
    default:
        settings.autoTrigger = false;
        break;
    }
    return settings;
}

QStringList readTessDataDirectories()
{
    // Apply the same path filtering used by the worker sandbox setup.
    return normalizeTessDataDirectories(MuPDFSettings::tessDataDirectories());
}

QStringList normalizeTessDataDirectories(const QStringList& directories)
{
    // Landlock receives canonical absolute read roots; reject relative paths
    // and preserve order while removing duplicates.
    QStringList normalized;
    for (const QString& directory : directories) {
        if (!directory.startsWith(QLatin1Char('/')))
            continue;
        const QString absolutePath = QDir::cleanPath(directory);
        if (!normalized.contains(absolutePath))
            normalized.append(absolutePath);
    }
    return normalized;
}

QString readCertificateDatabasePath(const QString& defaultPath)
{
    // The default toggle selects the system path; otherwise preserve the
    // explicitly configured database path for certificate management.
    return MuPDFSettings::useDefaultCertDB() ? defaultPath : MuPDFSettings::dBCertificatePath();
}

Model::DocumentType documentTypeForFile(const QString& fileName)
{
    // MatchContent handles files whose extension is missing or misleading.
    const auto mime = QMimeDatabase().mimeTypeForFile(fileName, QMimeDatabase::MatchContent);
    return documentTypeForMime(mime.name());
}

Model::DocumentType documentTypeForData(const QByteArray& data)
{
    // Content sniffing is the only available source of type information for
    // documents opened from memory.
    const auto mime = QMimeDatabase().mimeTypeForData(data);
    return documentTypeForMime(mime.name());
}

} // namespace Mu::Generator::Config
