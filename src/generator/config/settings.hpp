// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_CONFIG_SETTINGS_HPP
#define MUPDF_GENERATOR_CONFIG_SETTINGS_HPP

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "plugin/ocr/config.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator::Config {

/// Rendering values after conversion from the generated KDE settings enums.
struct RenderingSettings {
    int graphicsAntialiasing = 6;
    int textAntialiasing = 6;
    int imageQuality = 0;
    bool interpolateImages = true;
    std::int64_t memoryCacheBytes = 64LL * 1024 * 1024;

    bool operator==(const RenderingSettings& other) const = default;
};

/// EPUB layout values passed to the worker; custom CSS remains base64 encoded.
struct EpubSettings {
    int fontSize = 11;
    int fontFamily = 0;
    int pageSize = 1;
    QString customCssBase64;

    bool operator==(const EpubSettings& other) const = default;
};

/// Sentinel shown in the OCR language dropdown when no traineddata models exist.
inline constexpr std::string_view NoOcrModel = "-";

/// OCR policy read from the generator settings page.
struct OcrSettings {
    QString language = QStringLiteral("eng");
    int dpi = 225;
    bool force = false;
    bool autoTrigger = true;
    unsigned triggerThreshold = 20;
    bool asynchronous = true;
    bool notify = false;
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

inline Model::DocumentType documentTypeForMime(const QString& mime)
{
    // Keep MIME-to-model mapping in shared code so file and data detection
    // produce identical document types.
    return Model::documentTypeFromMime(mime.toStdString());
}

inline Model::DocumentSettings
documentSettingsFor(const RenderingSettings& rendering, const EpubSettings& epub, std::uint32_t paperColorRgb)
{
    // Clamp UI values at the IPC boundary; generated settings can outlive the
    // enum range accepted by the worker.
    Model::DocumentSettings settings;
    settings.graphicsAntialiasing = rendering.graphicsAntialiasing;
    settings.textAntialiasing = rendering.textAntialiasing;
    settings.imageQuality = rendering.imageQuality;
    settings.interpolateImages = rendering.interpolateImages;
    settings.memoryCacheBytes = rendering.memoryCacheBytes;
    settings.paperColorRgb = paperColorRgb;
    settings.epub.fontSize = std::clamp(epub.fontSize, 10, 20);
    settings.epub.pageSize = static_cast<Model::EpubPageSize>(std::clamp(epub.pageSize, 0, 3));
    settings.epub.fontFamily = static_cast<Model::EpubFontFamily>(std::clamp(epub.fontFamily, 0, 3));
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
    return { target.documentHash,  target.language,          pageCount, target.dpi, dpiX, dpiY, settings.force,
             settings.autoTrigger, settings.triggerThreshold };
}

void reloadSettings();
EpubSettings readEpubSettings();
WorkerSettings readWorkerSettings();
OcrSettings readOcrSettings();
SandboxEnforcement readSandboxEnforcement();
QStringList readTessDataDirectories();
QStringList normalizeTessDataDirectories(const QStringList& directories);
QString readCertificateDatabasePath(const QString& defaultPath);
Model::DocumentType documentTypeForFile(const QString& fileName);
Model::DocumentType documentTypeForData(const QByteArray& data);

} // namespace Mu::Generator::Config

#endif
