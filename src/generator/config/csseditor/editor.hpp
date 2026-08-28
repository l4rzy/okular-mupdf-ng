// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_CSS_EDITOR_HPP
#define MUPDF_GENERATOR_CSS_EDITOR_HPP

#include <QPlainTextEdit>
#include <QString>

namespace Mu::Generator {

class CssHighlighter;

/// Small Qt editor for the persisted, base64-encoded EPUB custom CSS value.
///
/// The widget exposes plain CSS to users while keeping the settings-facing
/// representation encoded and bounded by the shared model limit.
class CssEditor final : public QPlainTextEdit {
    Q_OBJECT
    Q_PROPERTY(QString encodedText READ encodedText WRITE setEncodedText NOTIFY encodedTextChanged)

public:
    explicit CssEditor(QWidget* parent = nullptr);

    // Encodes the current UTF-8 CSS for storage in the settings object.
    [[nodiscard]] QString encodedText() const;
    // Decodes persisted text, applies the character limit, and updates the UI.
    void setEncodedText(const QString& value);

Q_SIGNALS:
    void encodedTextChanged();

private:
    // Enforces the model limit without recursively emitting textChanged.
    void enforceCharacterLimit();

    // QSyntaxHighlighter reparents itself to the QTextDocument.
    CssHighlighter* m_highlighter = nullptr;
};

} // namespace Mu::Generator

#endif
