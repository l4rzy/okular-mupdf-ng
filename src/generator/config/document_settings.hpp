// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_DOCUMENT_SETTINGS_HPP
#define MU_GENERATOR_CONFIG_DOCUMENT_SETTINGS_HPP

#include "generator/config/settings.hpp"
#include <optional>

namespace Mu::Generator::Config {

struct DocumentSettings {
    std::optional<Model::DocumentTheme> theme;
    std::optional<EpubSettings> layout;
    std::optional<OcrSettings> ocr;
    bool operator==(const DocumentSettings&) const = default;
};

// Local identity follows the canonical path, including across file replacements.
QString documentSettingsKey(const QString& path);
QString documentSettingsKey(const QByteArray& data);
DocumentSettings readDocumentSettings(const QString& key);
bool writeDocumentSettings(const QString& key, const DocumentSettings& settings);

} // namespace Mu::Generator::Config
#endif
