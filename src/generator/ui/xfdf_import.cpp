// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/mupdfng.hpp"

#include "generator/conversion/xfdf.hpp"

#include <KLocalizedString>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QPointer>
#include <QScopeGuard>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace Mu::Generator {

bool Main::canImportXfdf() const
{
    return document() && m_document.type == Model::DocumentType::Pdf && !m_okularPages.isEmpty() && workerReady()
        && m_annotationProxy.supports(Okular::AnnotationProxy::Addition) && document()->isAllowed(Okular::AllowNotes);
}

void Main::importXfdf()
{
    if (!canImportXfdf())
        return;
    const QPointer<Main> guard(this);
    const QPointer<Okular::Document> target(const_cast<Okular::Document*>(document()));
    const auto isCurrentDocument = [guard, target] {
        return guard && target && guard->document() == target;
    };
    const QString path = QFileDialog::getOpenFileName(QApplication::activeWindow(),
                                                      i18n("Import Annotations from XFDF"),
                                                      QString(),
                                                      i18n("XFDF annotation files (*.xfdf *.xml)"));
    // The dialog runs an event loop: closing the tab can destroy this generator.
    if (!isCurrentDocument() || path.isEmpty() || !canImportXfdf())
        return;

    constexpr int NoticeMs = 5000;
    // Nonblocking open plus descriptor validation avoids FIFO hangs and path
    // replacement races. QFile borrows the descriptor; the guard closes it.
    const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (descriptor < 0) {
        Q_EMIT error(
            i18n("Could not open the XFDF file: %1", QString::fromLocal8Bit(std::strerror(errno)).toHtmlEscaped()),
            NoticeMs);
        return;
    }
    const auto closeDescriptor = qScopeGuard([descriptor] { ::close(descriptor); });
    struct stat status { };
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        Q_EMIT error(i18n("Select a regular XFDF or XML file."), NoticeMs);
        return;
    }
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly)) {
        Q_EMIT error(i18n("Could not open the XFDF file: %1", file.errorString().toHtmlEscaped()), NoticeMs);
        return;
    }
    const auto xml = file.read(Plugin::Xfdf::MaxXfdfBytes + 1);
    if (xml.size() > Plugin::Xfdf::MaxXfdfBytes) {
        Q_EMIT error(i18n("The XFDF file exceeds the 16 MiB size limit."), NoticeMs);
        return;
    }
    if (file.error() != QFileDevice::NoError) {
        Q_EMIT error(i18n("Could not read the XFDF file: %1", file.errorString().toHtmlEscaped()), NoticeMs);
        return;
    }

    const auto info = m_worker.getDocumentInfo({ QStringLiteral("hasRotatedPages") });
    if (!isCurrentDocument())
        return;
    const auto rotated = info.values.find("hasRotatedPages");
    if (rotated == info.values.end() || rotated->second != "false") {
        Q_EMIT error(rotated != info.values.end() && rotated->second == "true"
                         ? i18n("XFDF import does not support rotated PDF pages.")
                         : i18n("Could not check PDF page rotation for XFDF import."),
                     NoticeMs);
        return;
    }

    QString parseError;
    const auto parsed = Conversion::annotationsFromXfdf(xml, m_okularPages, dpi(), &parseError);
    if (!parseError.isEmpty()) {
        Q_EMIT error(i18n("Could not import XFDF: %1", parseError.toHtmlEscaped()), NoticeMs);
        return;
    }

    int imported = 0;
    int skipped = parsed.skipped;
    bool failed = false;
    for (int page = 0; page < parsed.pages.size(); ++page) {
        for (const auto& model : parsed.pages.at(page).annotations) {
            if (!isCurrentDocument())
                return;
            if (!canImportXfdf()) {
                failed = true;
                break;
            }
            // Read the live view rotation: earlier additions can notify slots
            // that rotate the document before the next annotation is added.
            auto annotation = Conversion::annotationFromXfdf(model, m_okularPages.at(page)->rotation());
            if (!annotation) {
                ++skipped;
                continue;
            }
            auto* added = annotation.release();
            target->addPageAnnotation(page, added);
            // Addition emits public Qt signals. Never touch old page objects
            // if a receiver closed the document or destroyed the generator.
            if (!isCurrentDocument())
                return;
            if (added->nativeId().toString().isEmpty()) {
                // Keep a failed native addition out of Okular's local overlay.
                target->undo();
                if (!isCurrentDocument())
                    return;
                failed = true;
                break;
            }
            ++imported;
        }
        if (failed)
            break;
    }
    if (failed) {
        Q_EMIT error(
            i18n("XFDF import stopped after %1 annotations. The remaining annotations were not added.", imported),
            NoticeMs);
        return;
    }
    if (imported == 0) {
        Q_EMIT warning(i18n("No supported annotations were found in the XFDF file (%1 skipped).", skipped), NoticeMs);
        return;
    }
    Q_EMIT notice(i18np("Imported %1 annotation from XFDF (%2 skipped).",
                        "Imported %1 annotations from XFDF (%2 skipped).",
                        imported,
                        skipped),
                  NoticeMs);
}

} // namespace Mu::Generator
