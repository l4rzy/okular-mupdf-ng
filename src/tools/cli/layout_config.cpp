// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tools/cli/layout_config.hpp"

#include <QHash>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

#include "shared/model/validation.hpp"

namespace Mu::Tools::Cli {
namespace {

struct Config {
    QHash<QString, QString> values;

    void readFile(const QString& path)
    {
        QSettings settings(path, QSettings::IniFormat);
        settings.setFallbacksEnabled(false);
        for (const auto& key : settings.allKeys()) {
            // QSettings exposes entries in [General] as root keys.
            const auto name = key.contains(QLatin1Char('/')) ? key : QStringLiteral("General/") + key;
            values.insert(name, settings.value(key).toString());
        }
    }

    int integer(const QString& key, int fallback) const
    {
        bool ok = false;
        const int result = values.value(key).toInt(&ok);
        return ok ? result : fallback;
    }

    int choice(const QString& key, const QStringList& names, int fallback) const
    {
        const auto value = values.constFind(key);
        if (value == values.cend())
            return fallback;
        for (qsizetype i = 0; i < names.size(); ++i) {
            if (value->compare(names[i], Qt::CaseInsensitive) == 0)
                return static_cast<int>(i);
        }
        return integer(key, fallback);
    }

    bool boolean(const QString& key, bool fallback) const
    {
        const auto value = values.constFind(key);
        if (value == values.cend())
            return fallback;
        // KConfig considers every value except these four spellings true.
        const auto lower = value->toLower();
        return lower != QStringLiteral("false") && lower != QStringLiteral("no") && lower != QStringLiteral("off")
            && lower != QStringLiteral("0");
    }
};

Model::DocumentSettings settingsFor(const Config& config)
{
    using namespace Model;
    DocumentSettings settings;
    const QStringList antialiasing { QStringLiteral("Disabled"),
                                     QStringLiteral("Minimum"),
                                     QStringLiteral("Low"),
                                     QStringLiteral("Medium"),
                                     QStringLiteral("High") };
    const auto aa = [&](const QString& key) {
        const int index = config.choice(key, antialiasing, 3);
        return index >= 0 && index < 5 ? index * 2 : 8;
    };
    settings.graphicsAntialiasing = aa(QStringLiteral("Rendering/GraphicsAntialiasingBits"));
    settings.textAntialiasing = aa(QStringLiteral("Rendering/TextAntialiasingBits"));
    settings.imageQuality =
        std::clamp(config.choice(QStringLiteral("Rendering/ImageRenderingQuality"),
                                 { QStringLiteral("Speed"), QStringLiteral("Balanced"), QStringLiteral("Quality") },
                                 1),
                   0,
                   Limit::MaxDocumentImageQuality);
    settings.interpolateImages = config.boolean(QStringLiteral("Rendering/ImageInterpolation"), true);
    settings.overprintSimulation = config.boolean(QStringLiteral("Rendering/OverprintSimulation"), false);
    const int memory = config.choice(QStringLiteral("General/MemoryLimit"),
                                     { QStringLiteral("Size32MiB"),
                                       QStringLiteral("Size64MiB"),
                                       QStringLiteral("Size128MiB"),
                                       QStringLiteral("Size256MiB") },
                                     1);
    settings.memoryCacheBytes = (memory >= 0 && memory < 4 ? (32LL << memory) : 64LL) * 1024 * 1024;
    const int trim = config.choice(QStringLiteral("General/IdleTrimLevel"),
                                   { QStringLiteral("Off"),
                                     QStringLiteral("Conservative"),
                                     QStringLiteral("Balanced"),
                                     QStringLiteral("Aggressive") },
                                   2);
    settings.idleTrimAggressiveness = trim >= 0 && trim <= 3 ? trim : IdleTrimLevel::Balanced;
    // The schema's font bounds are narrower than the protocol's bounds.
    settings.epub.fontSize = std::clamp(config.integer(QStringLiteral("EPUB/EpubFontSize"), 11), 10, 20);
    settings.epub.fontFamily =
        static_cast<EpubFontFamily>(std::clamp(config.choice(QStringLiteral("EPUB/EpubFontFamily"),
                                                             { QStringLiteral("Default"),
                                                               QStringLiteral("Serif"),
                                                               QStringLiteral("SansSerif"),
                                                               QStringLiteral("Monospace") },
                                                             0),
                                               0,
                                               3));
    constexpr EpubPageSize pageSizes[] {
        EpubPageSize::A5, EpubPageSize::SixByNine, EpubPageSize::B5, EpubPageSize::Letter
    };
    const int pageSize = config.choice(
        QStringLiteral("EPUB/EpubPageSize"),
        { QStringLiteral("A5"), QStringLiteral("SixByNine"), QStringLiteral("B5"), QStringLiteral("Letter") },
        0);
    settings.epub.pageSize = pageSizes[pageSize >= 0 && pageSize < 4 ? pageSize : 0];
    const auto css = config.values.value(QStringLiteral("EPUB/EpubCustomCss")).toStdString();
    if (isValidEpubCustomCssBase64(css))
        settings.epub.customCssBase64 = css;
#ifdef MU_WORKER_ENABLE_FORM_JAVASCRIPT
    settings.formJavaScriptEnabled = config.boolean(QStringLiteral("General/PdfFormJavaScriptEnabled"), false);
#endif
    return settings;
}

} // namespace

Model::DocumentSettings readLayoutConfig(const QStringList& files)
{
    Config config;
    for (const auto& file : files)
        config.readFile(file);
    return settingsFor(config);
}

Model::DocumentSettings readLayoutConfig()
{
    Config config;
    const auto directories = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation);
    const auto user = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const auto readSystem = [&](const QString& name) {
        for (auto it = directories.crbegin(); it != directories.crend(); ++it) {
            if (*it != user)
                config.readFile(*it + QLatin1Char('/') + name);
        }
    };
    config.readFile(QStringLiteral("/etc/kderc"));
    readSystem(QStringLiteral("system.kdeglobals"));
    readSystem(QStringLiteral("kdeglobals"));
    readSystem(QStringLiteral("okular-mupdf-ngrc"));
    config.readFile(user + QStringLiteral("/system.kdeglobals"));
    config.readFile(user + QStringLiteral("/kdeglobals"));
    config.readFile(user + QStringLiteral("/okular-mupdf-ngrc"));
    return settingsFor(config);
}

} // namespace Mu::Tools::Cli
