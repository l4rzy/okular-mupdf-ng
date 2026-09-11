// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_SETTINGS_HPP
#define MU_GENERATOR_CONFIG_SETTINGS_HPP

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>

#include "plugin/ocr/config.hpp"
#include "shared/model/types.hpp"
#include "shared/protocol/limits.hpp"

namespace Mu::Generator::Config {

/// Rendering values after conversion from the generated KDE settings enums.
struct RenderingSettings {
    int graphicsAntialiasing = 6;
    int textAntialiasing = 6;
    int imageQuality = 0;
    bool interpolateImages = true;
    std::int64_t memoryCacheBytes = 64LL * 1024 * 1024;
    std::int32_t idleTrimAggressiveness = Model::IdleTrimLevel::Balanced;

    bool operator==(const RenderingSettings& other) const = default;
};

/// EPUB layout values passed to the worker; custom CSS remains base64 encoded.
struct EpubSettings {
    int fontSize = 11;
    int fontFamily = 0;
    // Page size in Model::EpubPageSize order, not the KCfg index (see
    // epubPageSizeForConfig); the default matches the KCfg default of A5.
    int pageSize = static_cast<int>(Model::EpubPageSize::A5);
    QString customCssBase64;

    bool operator==(const EpubSettings& other) const = default;
};

/// OCR policy read from the generator settings page.
struct OcrSettings {
    QString language = QStringLiteral("eng");
    int dpi = 225;
    bool force = false;
    bool autoTrigger = true;
    unsigned triggerThreshold = 20;
    bool notify = false;
    /// Scroll-settle delay in milliseconds before OCR fires (hidden setting).
    int debounceMs = 250;
};

/// Stable inputs that identify an OCR cache namespace for one document.
struct OcrTarget {
    QString documentHash;
    QString language;
    int dpi = 225;
};

/// Policy for running the worker when its sandbox is not fully hardened.
enum class SandboxEnforcement {
    Relaxed,
    Strict,
};

inline Model::DocumentSettings
documentSettingsFor(const RenderingSettings& rendering, const EpubSettings& epub, std::uint32_t paperColorRgb)
{
    // Clamp UI values to the shared worker limits at the IPC boundary; the
    // generated settings can outlive the enum range accepted by the worker, and
    // the validator rejects anything outside these bounds.
    Model::DocumentSettings settings;
    settings.graphicsAntialiasing = std::clamp(rendering.graphicsAntialiasing, 0, Limit::MaxDocumentAntialiasing);
    settings.textAntialiasing = std::clamp(rendering.textAntialiasing, 0, Limit::MaxDocumentAntialiasing);
    settings.imageQuality = std::clamp(rendering.imageQuality, 0, Limit::MaxDocumentImageQuality);
    settings.interpolateImages = rendering.interpolateImages;
    settings.memoryCacheBytes =
        std::clamp(rendering.memoryCacheBytes, Limit::MinDocumentMemoryCacheBytes, Limit::MaxDocumentMemoryCacheBytes);
    // Clamp the trim level at the IPC boundary like the EPUB enums; the
    // worker normalizes again, but stable values keep change detection exact.
    settings.idleTrimAggressiveness = (rendering.idleTrimAggressiveness >= Model::IdleTrimLevel::Off
                                       && rendering.idleTrimAggressiveness <= Model::IdleTrimLevel::Aggressive)
        ? rendering.idleTrimAggressiveness
        : Model::IdleTrimLevel::Balanced;
    settings.paperColorRgb = paperColorRgb;
    settings.epub.fontSize = std::clamp(epub.fontSize, Limit::MinEpubFontSize, Limit::MaxEpubFontSize);
    settings.epub.pageSize =
        static_cast<Model::EpubPageSize>(std::clamp(epub.pageSize, 0, static_cast<int>(Model::EpubPageSize::Letter)));
    settings.epub.fontFamily = static_cast<Model::EpubFontFamily>(
        std::clamp(epub.fontFamily, 0, static_cast<int>(Model::EpubFontFamily::Monospace)));
    settings.epub.customCssBase64 = epub.customCssBase64.toStdString();
    return settings;
}

inline bool renderingOutputChanged(const RenderingSettings& previous, const RenderingSettings& current) noexcept
{
    // Cache-size changes affect resource usage, not the pixels Okular must
    // invalidate immediately.
    return previous.graphicsAntialiasing != current.graphicsAntialiasing
        || previous.textAntialiasing != current.textAntialiasing || previous.imageQuality != current.imageQuality
        || previous.interpolateImages != current.interpolateImages;
}

/// Session-scope worker configuration: rendering values track the settings
/// dialog; EPUB values stay fixed at process start (worker lifetime).
struct WorkerSettings {
    RenderingSettings rendering;
    EpubSettings startupEpub;

    /// Builds the worker-facing payload (clamped at the IPC boundary).
    /// The paper color is Okular session state, not a config value, so it is
    /// supplied by the caller.
    [[nodiscard]] Model::DocumentSettings documentSettings(std::uint32_t paperColorRgb) const
    {
        return documentSettingsFor(rendering, startupEpub, paperColorRgb);
    }

    bool operator==(const WorkerSettings&) const = default;
};

inline OcrTarget ocrTargetFor(const QString& documentHash, const OcrSettings& settings)
{
    // The document hash prevents OCR results from being reused for another
    // source even when language and DPI match.
    return { documentHash, settings.language, settings.dpi };
}

inline Plugin::OCR::Config
ocrConfigFor(const OcrTarget& target, int pageCount, double dpiX, double dpiY, const OcrSettings& settings)
{
    // Combine cache identity with page/display geometry for OCR scheduling.
    return {
        target.documentHash,  target.language,           pageCount,          target.dpi, dpiX, dpiY, settings.force,
        settings.autoTrigger, settings.triggerThreshold, settings.debounceMs
    };
}

void reloadSettings();
EpubSettings readEpubSettings();
WorkerSettings readWorkerSettings();
OcrSettings readOcrSettings();
/// Lists usable Tesseract language models (*.traineddata in the given
/// directories, excluding the non-language equ/osd data files) as a deduplicated,
/// name-sorted union. Missing directories contribute nothing. Matching is
/// case-sensitive; a basename present in several directories is listed once,
/// shadowing the later locations for single-model detection. Symlinks are
/// followed per QDir::entryList semantics.
QStringList installedOcrModels(const QStringList& directories);
/// The same union for the effective configuration: the built-in tessdata
/// directory plus the configured extra TessDataDirectories.
QStringList installedOcrModels();
/// Picks the effective OCR model from a usable list: empty stays off ("-"), a
/// single model wins whatever its language, several prefer "eng.traineddata"
/// and stay off when no English model exists.
QString autoSelectOcrModel(const QStringList& usableFiles);
SandboxEnforcement readSandboxEnforcement();
bool readDegradedSandboxNotificationEnabled();
QStringList readTessDataDirectories();
QStringList normalizeTessDataDirectories(const QStringList& directories);
QString readCertificateDatabasePath(const QString& defaultPath);
// Whether the default certificate database selection is in effect. Unlike
// readCertificateDatabasePath(), the defaulted selection hands NSS the empty
// path so the missing database directory is created on first use.
bool usesDefaultCertificateDatabase();
// Print scale mode as a clamped integer (0..2, matching PrintScaleMode order).
// Kept as an integer so this layer stays free of Okular widget types; the
// caller static_casts to PrintScaleMode.
std::uint32_t readPrintScaleMode();
void writePrintScaleMode(std::uint32_t mode);

} // namespace Mu::Generator::Config

#endif // MU_GENERATOR_CONFIG_SETTINGS_HPP
