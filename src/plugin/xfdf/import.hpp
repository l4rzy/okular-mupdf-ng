// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_XFDF_IMPORT_HPP
#define MU_PLUGIN_XFDF_IMPORT_HPP

#include <cstddef>

#include <QByteArray>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

#include "plugin/xfdf/types.hpp"
#include "shared/protocol/limits.hpp"

namespace Mu::Plugin::Xfdf {

/// Upper bound on a single XFDF document, mirroring the control-channel string
/// limit. Callers should reject larger inputs before reading them into memory.
inline constexpr qsizetype MaxXfdfBytes = 16 * 1024 * 1024;

/// Outcome of parsing one XFDF document.
///
/// `pages` always has the same length as the page-size vector passed to
/// `xfdfToAnnotations`; annotations are placed at their `page` attribute index,
/// so `pages.at(i)` is the annotation list for page `i`.
struct XfdfParseResult {
    QVector<Page> pages;
    /// Number of annotation entries that produced a usable model annotation.
    int applied = 0;
    /// Number of entries rejected as malformed, unknown, or out of range.
    int skipped = 0;
    /// Bounded, human-readable diagnostics for skipped entries.
    QStringList warnings;
};

/// Remaining annotation capacity in the target PDF after existing annotations
/// are counted. Empty per-page limits use the shared default.
struct XfdfParseLimits {
    std::size_t annotationsRemaining = Limit::MaxAnnotationsPerDocument;
    QVector<std::size_t> annotationsPerPageRemaining;
};

/// Parses an XFDF document into model annotations.
///
/// `pageSizes` holds the target page dimensions in PDF points, indexed by the
/// XFDF `page` attribute, and is used to convert the document's user-space
/// coordinates into normalized [0, 1] top-left coordinates. Entries that are
/// malformed, reference an out-of-range page, or name an unsupported element
/// are skipped and counted rather than failing the parse; only an unreadable
/// XML document, a DOCTYPE, or exhausted target annotation capacity is fatal
/// and reports `error`.
[[nodiscard]] XfdfParseResult xfdfToAnnotations(const QByteArray& xml,
                                                const QVector<QSizeF>& pageSizes,
                                                QString* error,
                                                const XfdfParseLimits& limits = { });

} // namespace Mu::Plugin::Xfdf

#endif // MU_PLUGIN_XFDF_IMPORT_HPP
