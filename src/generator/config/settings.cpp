// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/settings.hpp"

#include <QDir>

#include <algorithm>

#include "mupdfngsettings.h"
#include "plugin/caching/ocr_cache.hpp"

namespace Mu::Generator::Config {

namespace {

int graphicsAntialiasingBitsForConfig(int value) noexcept
{
    // KDE stores a named level; MuPDF consumes the corresponding bit count.
    switch (value) {
    case MuPDFNGSettings::EnumGraphicsAntialiasingBits::Disabled:
        return 0;
    case MuPDFNGSettings::EnumGraphicsAntialiasingBits::Minimum:
        return 2;
    case MuPDFNGSettings::EnumGraphicsAntialiasingBits::Low:
        return 4;
    case MuPDFNGSettings::EnumGraphicsAntialiasingBits::Medium:
        return 6;
    case MuPDFNGSettings::EnumGraphicsAntialiasingBits::High:
    default:
        return 8;
    }
}

int textAntialiasingBitsForConfig(int value) noexcept
{
    // Keep text and graphics mappings explicit because their generated enums
    // are independent even though their numeric levels currently match.
    switch (value) {
    case MuPDFNGSettings::EnumTextAntialiasingBits::Disabled:
        return 0;
    case MuPDFNGSettings::EnumTextAntialiasingBits::Minimum:
        return 2;
    case MuPDFNGSettings::EnumTextAntialiasingBits::Low:
        return 4;
    case MuPDFNGSettings::EnumTextAntialiasingBits::Medium:
        return 6;
    case MuPDFNGSettings::EnumTextAntialiasingBits::High:
    default:
        return 8;
    }
}

std::int64_t memoryCacheBytesForConfig(int value) noexcept
{
    // Convert the UI's MiB choice once, before sending byte units over IPC.
    switch (value) {
    case MuPDFNGSettings::EnumMemoryLimit::Size32MiB:
        return 32LL * 1024 * 1024;
    case MuPDFNGSettings::EnumMemoryLimit::Size128MiB:
        return 128LL * 1024 * 1024;
    case MuPDFNGSettings::EnumMemoryLimit::Size256MiB:
        return 256LL * 1024 * 1024;
    case MuPDFNGSettings::EnumMemoryLimit::Size64MiB:
    default:
        return 64LL * 1024 * 1024;
    }
}

std::int32_t idleTrimAggressivenessForConfig(int value) noexcept
{
    // Generated enums are ints; map through the generated constants so a
    // renamed or reordered choice cannot silently flip the trim policy.
    switch (value) {
    case MuPDFNGSettings::EnumIdleTrimLevel::Off:
        return Model::IdleTrimLevel::Off;
    case MuPDFNGSettings::EnumIdleTrimLevel::Conservative:
        return Model::IdleTrimLevel::Conservative;
    case MuPDFNGSettings::EnumIdleTrimLevel::Aggressive:
        return Model::IdleTrimLevel::Aggressive;
    case MuPDFNGSettings::EnumIdleTrimLevel::Balanced:
    default:
        return Model::IdleTrimLevel::Balanced;
    }
}

int ocrDebounceMsForConfig(int value) noexcept
{
    // Clamp the hidden scroll-settle delay; hand-edited configs can exceed
    // the kcfg min/max, and a tiny value would OCR-storm while scrolling.
    return std::clamp(value, 100, 2000);
}

} // namespace

void reloadSettings()
{
    // KConfigXT owns persistence; this only refreshes its generated singleton.
    MuPDFNGSettings::self()->read();
}

SandboxEnforcement readSandboxEnforcement()
{
    // Generated enums are ints; map through the generated constants so a
    // renamed or reordered choice cannot silently flip the security policy.
    switch (MuPDFNGSettings::sandboxEnforcement()) {
    case MuPDFNGSettings::EnumSandboxEnforcement::Strict:
        return SandboxEnforcement::Strict;
    case MuPDFNGSettings::EnumSandboxEnforcement::Relaxed:
    default:
        return SandboxEnforcement::Relaxed;
    }
}

bool readDegradedSandboxNotificationEnabled()
{
    return MuPDFNGSettings::notifyDegradedSandbox();
}

EpubSettings readEpubSettings()
{
    // Keep custom CSS encoded exactly as stored; CssEditor owns the UI form.
    return { MuPDFNGSettings::epubFontSize(),
             MuPDFNGSettings::epubFontFamily(),
             MuPDFNGSettings::epubPageSize(),
             MuPDFNGSettings::epubCustomCss() };
}

WorkerSettings readWorkerSettings()
{
    // One reader keeps the session-scope pair coherent: rendering values are
    // normalized into worker-facing units; EPUB values stay encoded exactly
    // as stored (CssEditor owns the UI form).
    return {
        { graphicsAntialiasingBitsForConfig(MuPDFNGSettings::graphicsAntialiasingBits()),
          textAntialiasingBitsForConfig(MuPDFNGSettings::textAntialiasingBits()),
          static_cast<int>(MuPDFNGSettings::imageRenderingQuality()),
          MuPDFNGSettings::imageInterpolation(),
          memoryCacheBytesForConfig(MuPDFNGSettings::memoryLimit()),
          idleTrimAggressivenessForConfig(MuPDFNGSettings::idleTrimLevel()) },
        readEpubSettings(),
    };
}

OcrSettings readOcrSettings()
{
    // Normalize the configured traineddata name to the cache/worker language
    // token and translate the trigger enum into explicit policy flags.
    // A "-" language means no models are installed: never fire OCR. A value
    // that is not a model filename is malformed (only hand-edited config can
    // reach it) and must not silently flip OCR to English: degrade to off.
    OcrSettings settings;
    QString language = MuPDFNGSettings::ocrLanguage();
    // An unset language follows the installed models, including the configured
    // extra tessdata directories: a usable model enables OCR without requiring
    // a settings visit first.
    if (language.isEmpty() || language == QStringLiteral("-"))
        language = autoSelectOcrModel(installedOcrModels());
    if (language == QStringLiteral("-") || language.isEmpty() || !language.endsWith(QStringLiteral(".traineddata"))) {
        settings.language = QStringLiteral("-");
        settings.dpi = static_cast<int>(Plugin::Caching::OCR::Cache::qualityToDpi(MuPDFNGSettings::ocrQuality()));
        settings.notify = MuPDFNGSettings::ocrNotify();
        settings.force = false;
        settings.autoTrigger = false;
        settings.debounceMs = ocrDebounceMsForConfig(MuPDFNGSettings::ocrDebounceMs());
        return settings;
    }
    settings.language = Plugin::Caching::OCR::Cache::stripLangSuffix(language);
    settings.dpi = static_cast<int>(Plugin::Caching::OCR::Cache::qualityToDpi(MuPDFNGSettings::ocrQuality()));
    settings.notify = MuPDFNGSettings::ocrNotify();
    settings.debounceMs = ocrDebounceMsForConfig(MuPDFNGSettings::ocrDebounceMs());

    switch (MuPDFNGSettings::ocrTriggerMode()) {
    case MuPDFNGSettings::EnumOcrTriggerMode::Five:
        settings.autoTrigger = true;
        settings.triggerThreshold = 5;
        break;
    case MuPDFNGSettings::EnumOcrTriggerMode::Twenty:
        settings.autoTrigger = true;
        settings.triggerThreshold = 20;
        break;
    case MuPDFNGSettings::EnumOcrTriggerMode::Always:
        settings.force = true;
        break;
    case MuPDFNGSettings::EnumOcrTriggerMode::Never:
    default:
        settings.autoTrigger = false;
        break;
    }
    return settings;
}

QStringList readTessDataDirectories()
{
    // Apply the same path filtering used by the worker sandbox setup.
    return normalizeTessDataDirectories(MuPDFNGSettings::tessDataDirectories());
}

QStringList installedOcrModels(const QStringList& directories)
{
    // Only language models are selectable: equ/osd are Tesseract support
    // files, not OCR languages. A model present in several directories is
    // listed once so single-model detection stays exact.
    QStringList models;
    for (const QString& directory : directories) {
        QDir dir(directory);
        const QStringList files = dir.entryList({ QStringLiteral("*.traineddata") }, QDir::Files, QDir::Name);
        for (const QString& file : files) {
            if (file == QStringLiteral("equ.traineddata") || file == QStringLiteral("osd.traineddata"))
                continue;
            if (!models.contains(file))
                models.append(file);
        }
    }
    std::sort(models.begin(), models.end());
    return models;
}

QStringList installedOcrModels()
{
    QStringList directories { QStringLiteral(TESSDATA_DIR) };
    directories.append(readTessDataDirectories());
    return installedOcrModels(directories);
}

QString autoSelectOcrModel(const QStringList& usableFiles)
{
    if (usableFiles.isEmpty())
        return QStringLiteral("-");
    if (usableFiles.size() == 1)
        return usableFiles.constFirst();
    // With several models installed, English is the default; a non-English
    // setup stays off rather than guessing a language for the user.
    if (usableFiles.contains(QStringLiteral("eng.traineddata")))
        return QStringLiteral("eng.traineddata");
    return QStringLiteral("-");
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
    return MuPDFNGSettings::useDefaultCertDB() ? defaultPath : MuPDFNGSettings::dBCertificatePath();
}

bool usesDefaultCertificateDatabase()
{
    return MuPDFNGSettings::useDefaultCertDB();
}

} // namespace Mu::Generator::Config
