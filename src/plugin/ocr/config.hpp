// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_OCR_CONFIG_HPP
#define MU_PLUGIN_OCR_CONFIG_HPP

#include <QString>

namespace Mu::Plugin::OCR {

struct Config {
    QString documentHash;
    QString language;
    int pageCount = 0;
    int dpi = 225;
    double dpiX = 72;
    double dpiY = 72;
    bool force = false;
    bool autoTrigger = true;
    unsigned triggerThreshold = 20;

    bool operator==(const Config& other) const
    {
        // dpiX/dpiY reflect the current view zoom and are OCR-irrelevant;
        // excluding them avoids spurious settle() runs while zooming.
        return documentHash == other.documentHash && language == other.language && pageCount == other.pageCount
            && dpi == other.dpi && force == other.force && autoTrigger == other.autoTrigger
            && triggerThreshold == other.triggerThreshold;
    }
};

} // namespace Mu::Plugin::OCR
#endif // MU_PLUGIN_OCR_CONFIG_HPP
