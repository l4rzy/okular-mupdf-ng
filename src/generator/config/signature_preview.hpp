// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_SIGNATURE_PREVIEW_HPP
#define MU_GENERATOR_CONFIG_SIGNATURE_PREVIEW_HPP

#include <QDateTime>
#include <QFont>
#include <QImage>
#include <QString>
#include <QStringList>

namespace Mu::Generator::Config {

/// Mock preview content for the signature settings card. Mirrors the worker
/// appearance rules using fixed sample identity: the complete profile shows
/// the signer name as left graphic text plus labeled info lines, while the
/// simple profile shows name, reason, and time full-width. Sample strings and
/// label prefixes stay English to match MuPDF's real output.
struct SignaturePreview {
    QString leftText;
    QStringList rightLines;
};

/// Builds the preview content for the given profile and clock. Pure decision
/// logic; the caller chooses local time or UTC beforehand via @p useUtc.
[[nodiscard]] SignaturePreview buildSignaturePreview(bool simple, bool useUtc, const QDateTime& now);

/// Renders preview content as a document snippet: black text on white with a
/// thin border. An empty left text renders full-width info lines; otherwise a
/// two-pane layout with the left text vertically centered beside the info
/// lines. Colors are hardcoded deliberately: the preview depicts a PDF
/// artifact, which is black-on-white regardless of the application theme.
[[nodiscard]] QImage renderSignaturePreview(const SignaturePreview& preview, const QFont& font, qreal devicePixelRatio);

} // namespace Mu::Generator::Config

#endif // MU_GENERATOR_CONFIG_SIGNATURE_PREVIEW_HPP
