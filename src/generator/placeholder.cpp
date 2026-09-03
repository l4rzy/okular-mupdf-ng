// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/placeholder.hpp"

#include <KLocalizedString>
#include <QMimeDatabase>
#include <QMimeType>
#include <QUrl>

#include <QFont>
#include <QPainter>
#include <QtGlobal>

#include "shared/logging.hpp"

namespace Mu::Generator {

namespace {

// Error-card palette; messages arrive pre-localized from the caller.
struct Palette {
    QColor cardBg;
    QColor cardBorder;
    QColor textColor;
};

constexpr Palette kPalette { QColor(255, 235, 235), QColor(180, 45, 45), QColor(120, 30, 30) };

QImage drawErrorCard(int width, int height, const QString& message)
{
    if (width <= 0 || height <= 0)
        return { };

    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return { };

    image.fill(QColor(245, 245, 245));

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    const int minDimension = qMin(width, height);
    const int borderWidth = qMax(1, minDimension / 160);
    const int margin = qMax(2, minDimension / 20);

    painter.setPen(QPen(kPalette.cardBorder, borderWidth));
    painter.setBrush(kPalette.cardBg);
    painter.drawRoundedRect(image.rect().adjusted(margin, margin, -margin, -margin), margin, margin);

    // Skip drawing text on tiny thumbnails to avoid illegible artifacts
    if (message.isEmpty() || width < 120 || height < 80)
        return image;

    QFont font = painter.font();
    font.setPixelSize(qBound(10, minDimension / 18, 28));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(kPalette.textColor);

    const QRect textRect = image.rect().adjusted(margin * 2, margin * 2, -margin * 2, -margin * 2);
    painter.drawText(textRect, Qt::AlignCenter | Qt::TextWordWrap, message);
    return image;
}

} // namespace

void Placeholder::activate(QString message, Reason reason)
{
    // Terminal state is sticky: never re-notify or downgrade WorkerUnavailable.
    if (isActive() && m_reason == Reason::WorkerUnavailable)
        return;

    m_reason = reason;
    // Atomic shared_ptr publication: readers either see nothing (inactive) or
    // a fully constructed string.
    m_message.store(std::make_shared<const QString>(std::move(message)));
}

bool Placeholder::deactivate()
{
    // The published message stays until the queued reopen completes and
    // reset() runs, keeping renders blocked in the meantime. Repeated
    // deactivations must not request a duplicate reopen.
    if (!isActive() || m_reason != Reason::SandboxGate || m_reloadQueued)
        return false;
    m_reloadQueued = true;
    return true;
}

void Placeholder::reset()
{
    // Terminal failure persists for the owning-thread lifetime.
    if (m_reason == Reason::WorkerUnavailable)
        return;
    m_message.store(nullptr);
    m_reloadQueued = false;
}

QString Placeholder::message() const
{
    const auto snapshot = m_message.load();
    return snapshot ? *snapshot : QString();
}

QImage Placeholder::image(int width, int height) const
{
    return drawErrorCard(width, height, message());
}

namespace SandboxGate {

QString guidanceMessage(const Model::SandboxStatus& status)
{
    const QString reason = QString::fromStdString(status.reason);
    return i18n("⚠️ The worker sandbox is not fully hardened and sandbox enforcement is Strict.\n"
                "To open this document, switch Sandbox Enforcement to Relaxed. This will reduce security "
                "protections.\nError: %1",
                reason.isEmpty() ? QString() : QStringLiteral("[%1]").arg(reason));
}

Okular::Page* withheldPage(double dpiWidth, double dpiHeight)
{
    constexpr double kPlaceholderWidthPt = 595.0;
    constexpr double kPlaceholderHeightPt = 842.0;
    return new Okular::Page(
        0, kPlaceholderWidthPt * dpiWidth / 72.0, kPlaceholderHeightPt * dpiHeight / 72.0, Okular::Rotation0);
}

ReopenResult reopenLocalDocument(Okular::Document* doc, const QString& password)
{
    // Capture everything before closeDocument(): it nulls the generator's
    // document back-pointer (Document::closeDocument), and doCloseDocument()
    // clears the password. The Document object itself is Part-owned and
    // survives the close, so the passed pointer stays valid for openDocument.
    if (!doc || !doc->isOpened())
        return ReopenResult::Failed;
    const QUrl url = doc->currentDocument();
    if (!url.isLocalFile()) {
        MU_LOG(warning, "Mu::Generator::Main", "cannot auto-reopen a document without a local file");
        return ReopenResult::NotLocal;
    }
    const QString docFile = url.toLocalFile();
    const QMimeType mime = QMimeDatabase().mimeTypeForFile(docFile);
    MU_LOG(debug, "Mu::Generator::Main", "sandbox gate relaxed; reopening withheld document");
    doc->closeDocument();
    // On failure the generator signals are disconnected, so this is log-only.
    if (doc->openDocument(docFile, url, mime, password) != Okular::Document::OpenSuccess) {
        MU_LOG(warning, "Mu::Generator::Main", "document reopen failed after enforcement change");
        return ReopenResult::Failed;
    }
    return ReopenResult::Reopened;
}

} // namespace SandboxGate

} // namespace Mu::Generator
