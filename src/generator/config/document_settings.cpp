// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/config/document_settings.hpp"
#include "shared/model/validation.hpp"

#include <KConfig>
#include <KConfigGroup>
#include <QCryptographicHash>
#include <QFileInfo>

namespace Mu::Generator::Config {
namespace {

QString digest(QByteArrayView data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

constexpr auto SettingsFile = "okular-mupdf-ng/documentsrc";

} // namespace

QString documentSettingsKey(const QString& path)
{
    const QFileInfo file(path);
    if (!file.isFile())
        return { };
    return digest(file.canonicalFilePath().toUtf8());
}

QString documentSettingsKey(const QByteArray& data)
{
    return digest(data);
}

DocumentSettings readDocumentSettings(const QString& key)
{
    DocumentSettings settings;
    if (key.isEmpty())
        return settings;
    const KConfig config(QString::fromLatin1(SettingsFile), KConfig::SimpleConfig);
    const KConfigGroup group(&config, key);
    const int theme = group.readEntry("Theme", -1);
    if (theme >= 0 && theme <= static_cast<int>(Model::DocumentTheme::Dark))
        settings.theme = static_cast<Model::DocumentTheme>(theme);
    if (group.hasKey("FontSize")) {
        EpubSettings layout;
        layout.fontSize = group.readEntry("FontSize", layout.fontSize);
        layout.fontFamily = group.readEntry("FontFamily", layout.fontFamily);
        layout.pageSize = group.readEntry("PageSize", layout.pageSize);
        layout.customCssBase64 = group.readEntry("CustomCss", QString());
        if (layout.fontSize >= Limit::MinEpubFontSize && layout.fontSize <= Limit::MaxEpubFontSize
            && layout.fontFamily >= 0 && layout.fontFamily <= static_cast<int>(Model::EpubFontFamily::Monospace)
            && layout.pageSize >= 0 && layout.pageSize <= static_cast<int>(Model::EpubPageSize::Letter)
            && Model::isValidEpubCustomCssBase64(layout.customCssBase64.toStdString()))
            settings.layout = layout;
    }
    if (group.hasKey("OcrLanguage")) {
        OcrSettings ocr;
        ocr.language = group.readEntry("OcrLanguage", QStringLiteral("-"));
        ocr.dpi = group.readEntry("OcrDpi", ocr.dpi);
        ocr.notify = group.readEntry("OcrNotify", ocr.notify);
        const int mode = group.readEntry("OcrMode", -1);
        if (!ocr.language.isEmpty() && !ocr.language.contains(QChar::Null) && !ocr.language.contains(QLatin1Char('/'))
            && !ocr.language.contains(QLatin1Char('\\')) && (ocr.dpi == 150 || ocr.dpi == 225 || ocr.dpi == 300)
            && mode >= 0 && mode <= 3) {
            setOcrTriggerMode(ocr, mode);
            settings.ocr = ocr;
        }
    }
    return settings;
}

bool writeDocumentSettings(const QString& key, const DocumentSettings& settings)
{
    if (key.isEmpty())
        return false;
    KConfig config(QString::fromLatin1(SettingsFile), KConfig::SimpleConfig);
    KConfigGroup group(&config, key);
    // Delete inherited entries rather than materializing global defaults.
    group.deleteGroup();
    if (settings.theme)
        group.writeEntry("Theme", static_cast<int>(*settings.theme));
    if (settings.layout) {
        group.writeEntry("FontSize", settings.layout->fontSize);
        group.writeEntry("FontFamily", settings.layout->fontFamily);
        group.writeEntry("PageSize", settings.layout->pageSize);
        group.writeEntry("CustomCss", settings.layout->customCssBase64);
    }
    if (settings.ocr) {
        group.writeEntry("OcrLanguage", settings.ocr->language);
        group.writeEntry("OcrDpi", settings.ocr->dpi);
        group.writeEntry("OcrMode", ocrTriggerModeFor(*settings.ocr));
        group.writeEntry("OcrNotify", settings.ocr->notify);
    }
    return config.sync();
}

} // namespace Mu::Generator::Config
