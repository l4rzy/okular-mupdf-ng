// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_ANNOTATION_HPP
#define MU_GENERATOR_PROXY_ANNOTATION_HPP

#include <okular/core/annotations.h>

#include <functional>
#include <optional>

#include <QImage>
#include <QMutex>

namespace Mu::Plugin {

class WorkerClient;

}

namespace Mu::Generator::Proxy {

class Annotation final : public Okular::AnnotationProxy {
public:
    // Called when Okular has changed a native annotation and the worker state
    // may no longer match the retained source document.
    using MutationCallback = std::function<void()>;

    explicit Annotation(Plugin::WorkerClient* backend = nullptr, MutationCallback mutationCallback = { });
    ~Annotation() override;

    // Disabled while the worker has no document or its native handles are stale.
    void setAvailable(bool available) noexcept;

    bool supports(Capability capability) const override;
    void notifyAddition(Okular::Annotation* annotation, int page) override;
    void notifyModification(const Okular::Annotation* annotation, int page, bool appearanceChanged) override;
    void notifyRemoval(Okular::Annotation* annotation, int page) override;

    // Composites the pending signature preview onto a freshly rendered page
    // image covering @p pageRegion (a normalized page area: full page or tile).
    // Returns true when @p image was modified. May run on render threads.
    bool paintPendingSignature(QImage& image, int page, const Okular::NormalizedRect& pageRegion) const;

private:
    // Display-only state for an unsigned signature the user is positioning. It
    // never reaches the worker: the worker is only contacted on Finish Signing.
    struct PendingSignature {
        Okular::NormalizedRect bounds;
        bool hidden = false;
    };

    void beginPendingSignature(const Okular::SignatureAnnotation* signature, int page);
    void updatePendingSignature(const Okular::SignatureAnnotation* signature);
    void clearPendingSignature() noexcept;

    Plugin::WorkerClient* m_backend = nullptr;
    MutationCallback m_mutationCallback;
    bool m_available = false;

    mutable QMutex m_pendingMutex;
    std::optional<PendingSignature> m_pending;
    int m_pendingPage = -1;
};

} // namespace Mu::Generator::Proxy

#endif // MU_GENERATOR_PROXY_ANNOTATION_HPP
