// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_CONFIG_CSSEDITOR_HIGHLIGHTER_HPP
#define MU_GENERATOR_CONFIG_CSSEDITOR_HIGHLIGHTER_HPP

#include <QSyntaxHighlighter>

namespace Mu::Generator {

/// Applies lightweight CSS syntax colors using the current application theme.
///
/// Highlighting is intentionally lexical rather than a full CSS parser: the
/// editor remains responsive while still making common constructs readable.
class CssHighlighter final : public QSyntaxHighlighter {
    Q_OBJECT

public:
    explicit CssHighlighter(QTextDocument* document);

protected:
    // Formats one block while preserving multi-line comment state.
    void highlightBlock(const QString& text) override;

private:
    // Initializes light/dark palette-aware formats before highlighting starts.
    void initializeFormats();

    QTextCharFormat m_commentFormat;
    QTextCharFormat m_stringFormat;
    QTextCharFormat m_selectorFormat;
    QTextCharFormat m_propertyFormat;
    QTextCharFormat m_atRuleFormat;
    QTextCharFormat m_valueFormat;
    QTextCharFormat m_colorFormat;
    QTextCharFormat m_keywordFormat;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_CONFIG_CSSEDITOR_HIGHLIGHTER_HPP
