// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "highlighter.hpp"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QRegularExpression>
#include <QTextCharFormat>

namespace Mu::Generator {

namespace {

QColor syntaxColor(const QPalette& palette, const QColor& light, const QColor& dark)
{
    // Use the editor base color rather than the global widget mode so custom
    // application palettes remain readable in either appearance.
    return palette.color(QPalette::Base).lightness() < 128 ? dark : light;
}

} // namespace

CssHighlighter::CssHighlighter(QTextDocument* document)
    : QSyntaxHighlighter(document)
{
    // The QTextDocument owns this highlighter through QSyntaxHighlighter's
    // QObject parent relationship.
    initializeFormats();
}

void CssHighlighter::initializeFormats()
{
    // Step 1: Select palette-aware colors once; block highlighting only applies
    // these precomputed formats.
    const QPalette palette = QApplication::palette();

    m_commentFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#6a737d")), QColor(QStringLiteral("#8b949e"))));
    m_commentFormat.setFontItalic(true);

    m_stringFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#a31515")), QColor(QStringLiteral("#ce9178"))));
    m_selectorFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#800000")), QColor(QStringLiteral("#d7ba7d"))));
    m_propertyFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#795e26")), QColor(QStringLiteral("#9cdcfe"))));
    m_atRuleFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#af00db")), QColor(QStringLiteral("#c586c0"))));
    m_valueFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#001080")), QColor(QStringLiteral("#b5cea8"))));
    m_colorFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#267f99")), QColor(QStringLiteral("#4ec9b0"))));
    m_keywordFormat.setForeground(
        syntaxColor(palette, QColor(QStringLiteral("#0000ff")), QColor(QStringLiteral("#569cd6"))));
    m_keywordFormat.setFontWeight(QFont::Bold);
}

void CssHighlighter::highlightBlock(const QString& text)
{
    // Step 2: Track comments first because an unterminated comment continues
    // into the next QTextDocument block.
    constexpr int commentState = 1;
    qsizetype commentStart = previousBlockState() == commentState ? 0 : text.indexOf(QStringLiteral("/*"));

    while (commentStart >= 0) {
        const qsizetype commentEnd = text.indexOf(QStringLiteral("*/"), commentStart + 2);
        if (commentEnd < 0) {
            setFormat(static_cast<int>(commentStart), static_cast<int>(text.size() - commentStart), m_commentFormat);
            setCurrentBlockState(commentState);
            return;
        }

        setFormat(static_cast<int>(commentStart), static_cast<int>(commentEnd + 2 - commentStart), m_commentFormat);
        commentStart = text.indexOf(QStringLiteral("/*"), commentEnd + 2);
    }

    setCurrentBlockState(0);

    // Step 3: Apply lexical CSS formats. Later matches intentionally override
    // broad selector/value matches when constructs overlap.
    const auto setFormatForMatches = [this, &text](const QRegularExpression& expression,
                                                   const QTextCharFormat& format) {
        auto match = expression.match(text);
        while (match.hasMatch()) {
            setFormat(static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), format);
            match = expression.match(text, match.capturedEnd());
        }
    };

    // Apply broad constructs first; the more specific formats below take
    // precedence for properties, values, strings, and comments.
    setFormatForMatches(QRegularExpression(QStringLiteral("(?:^|})\\s*[^{}]+(?=\\{)")), m_selectorFormat);
    setFormatForMatches(QRegularExpression(QStringLiteral("\\b[-_a-zA-Z][-_a-zA-Z0-9]*\\s*(?=:)")), m_propertyFormat);
    setFormatForMatches(QRegularExpression(QStringLiteral("@[a-zA-Z-]+")), m_atRuleFormat);
    setFormatForMatches(QRegularExpression(QStringLiteral("#[0-9a-fA-F]{3,8}\\b")), m_colorFormat);
    setFormatForMatches(
        QRegularExpression(QStringLiteral("(?<![a-zA-Z0-9_.-])-?[0-9]+(?:\\.[0-9]+)?(?:%|[a-zA-Z]+)?\\b")),
        m_valueFormat);
    setFormatForMatches(
        QRegularExpression(QStringLiteral("\\!(?:important|default)\\b|\\b(?:inherit|initial|unset|none|auto)\\b")),
        m_keywordFormat);

    setFormatForMatches(QRegularExpression(QStringLiteral("(?:\"[^\"]*\"|'[^']*')")), m_stringFormat);

    // Step 4: Reapply comments last so comment contents cannot receive another
    // style, including when a comment shares a line with CSS syntax.
    qsizetype start = text.indexOf(QStringLiteral("/*"));
    while (start >= 0) {
        const qsizetype end = text.indexOf(QStringLiteral("*/"), start + 2);
        const qsizetype length = end < 0 ? text.size() - start : end + 2 - start;
        setFormat(static_cast<int>(start), static_cast<int>(length), m_commentFormat);
        if (end < 0)
            break;
        start = text.indexOf(QStringLiteral("/*"), end + 2);
    }
}

} // namespace Mu::Generator
