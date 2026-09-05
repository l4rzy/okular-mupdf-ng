// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PLACEHOLDER_HPP
#define MU_GENERATOR_PLACEHOLDER_HPP

#include <QImage>
#include <QString>

#include <okular/core/document.h>
#include <okular/core/page.h>

#include <atomic>
#include <memory>

#include "generator/config/settings.hpp"

namespace Mu::Generator {

/**
 * Owns the "document display interrupted" state for the generator.
 *
 * While active, worker-backed generator services short-circuit: renders
 * return image(), and text, metadata, synopsis, fonts, embedded files, save,
 * print, and signing return empty or refuse. Two reasons exist:
 *
 * - SandboxGate: the document is withheld from a worker whose sandbox is not
 *   fully hardened. Restorable: deactivate() reports once that a reopen
 *   should be queued, and the published message is cleared by reset() only
 *   after the successful reopen, so no render can reach a closed worker
 *   document.
 * - WorkerUnavailable: an unrecoverable worker failure. Terminal: it
 *   survives reset() and can never be downgraded.
 *
 * This is a pure state class: side effects (closing the worker document,
 * refreshing pages, queueing the reopen) belong to the caller.
 *
 * Thread contract: activate()/deactivate()/reset() run on the owning
 * (generator) thread; isActive()/message()/image() may be called from render
 * threads. Activity and message share a single atomic publication point: a
 * null shared_ptr means inactive, and a non-null one is a fully constructed
 * string, so readers never observe a partial state.
 */
class Placeholder final {
public:
    /// What interrupted the document display.
    enum class Reason {
        SandboxGate,
        WorkerUnavailable,
    };

    /// Sets the message, then publishes it. Terminal WorkerUnavailable
    /// state is sticky: repeated or downgrading activations are ignored.
    void activate(QString message, Reason reason);
    /// Requests restore; returns true exactly once per gate cycle (the marker
    /// is cleared by reset()). No-op when inactive, terminal, or already
    /// queued. The published message stays until reset().
    bool deactivate();
    /// Ends a gate cycle; terminal WorkerUnavailable state persists.
    void reset();

    [[nodiscard]] bool isActive() const noexcept { return m_message.load() != nullptr; }

    [[nodiscard]] Reason reason() const noexcept { return m_reason; }

    [[nodiscard]] QString message() const;
    /// Renders the guidance card at the requested pixel dimensions; an empty
    /// message renders the card without text.
    [[nodiscard]] QImage image(int width, int height) const;

private:
    // Single publication point; owning thread writes, render threads read.
    std::atomic<std::shared_ptr<const QString>> m_message;
    // Owning thread only; render threads never read these.
    Reason m_reason = Reason::SandboxGate;
    bool m_reloadQueued = false;
};

/// Okular adapters for the sandbox gate: the guidance card message and the
/// display page / close-open cycle for a withheld document. The gating policy
/// itself lives in Main::sandboxGated().
namespace SandboxGate {

/// Outcome of the queued close/open cycle for a withheld document.
enum class ReopenResult {
    Reopened,
    NotLocal,
    Failed,
};

/// Localized guidance shown on the placeholder card and in refusals.
[[nodiscard]] QString guidanceMessage(const Model::SandboxStatus& status);

/// Single A4 display page standing in for the withheld document. Ownership
/// transfers to the caller's Okular page vector.
[[nodiscard]] Okular::Page* withheldPage(double dpiWidth, double dpiHeight);

/// Runs the Okular close/open cycle for the retained local file. UI-free:
/// reports the outcome; the caller maps it to signals.
[[nodiscard]] ReopenResult reopenLocalDocument(Okular::Document* doc, const QString& password);

} // namespace SandboxGate

} // namespace Mu::Generator

#endif // MU_GENERATOR_PLACEHOLDER_HPP
