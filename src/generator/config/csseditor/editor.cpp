// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "editor.hpp"

#include <QFontDatabase>
#include <QSignalBlocker>

#include "highlighter.hpp"
#include "shared/model/types.hpp"

namespace Mu::Generator {

namespace {

QString truncateCharacters(const QString& value)
{
    // Count Unicode code points rather than UTF-16 units so the shared limit
    // cannot split a supplementary character in the middle of its surrogate.
    const QList<uint> codePoints = value.toUcs4();
    if (codePoints.size() <= static_cast<qsizetype>(Model::MaxEpubCustomCssCharacters))
        return value;
    return QString::fromUcs4(reinterpret_cast<const char32_t*>(codePoints.constData()),
                             static_cast<qsizetype>(Model::MaxEpubCustomCssCharacters));
}

} // namespace

CssEditor::CssEditor(QWidget* parent)
    : QPlainTextEdit(parent)
{
    // Step 1: Use a fixed-width font and attach highlighting to this document.
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_highlighter = new CssHighlighter(document());
    // Step 2: Keep the encoded property synchronized with user edits while
    // enforcing the same size bound used by the worker model.
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        enforceCharacterLimit();
        Q_EMIT encodedTextChanged();
    });
    setPlaceholderText(QStringLiteral("Enter custom EPUB CSS..."));
}

QString CssEditor::encodedText() const
{
    // Settings store ASCII base64, while the editor works with UTF-8 text.
    return QString::fromLatin1(toPlainText().toUtf8().toBase64());
}

void CssEditor::setEncodedText(const QString& value)
{
    // Loading settings must not trigger a second settings write through
    // encodedTextChanged.
    const QByteArray encoded = value.toLatin1();
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    const QString css = truncateCharacters(QString::fromUtf8(decoded));
    const QSignalBlocker blocker(this);
    setPlainText(css);
}

void CssEditor::enforceCharacterLimit()
{
    const QString current = toPlainText();
    const QString limited = truncateCharacters(current);
    if (current == limited)
        return;

    // Preserve the caret as far as possible after truncating the document.
    const QSignalBlocker blocker(this);
    QTextCursor cursor = textCursor();
    setPlainText(limited);
    cursor.setPosition(qMin(cursor.position(), static_cast<int>(limited.size())));
    setTextCursor(cursor);
}

} // namespace Mu::Generator
