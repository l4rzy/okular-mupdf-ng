// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_ANNOTATION_HPP
#define MU_GENERATOR_PROXY_ANNOTATION_HPP

#include <okular/core/annotations.h>

#include <functional>

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
    void setAvailable(bool available) noexcept { m_available = available; }

    bool supports(Capability capability) const override;
    void notifyAddition(Okular::Annotation* annotation, int page) override;
    void notifyModification(const Okular::Annotation* annotation, int page, bool appearanceChanged) override;
    void notifyRemoval(Okular::Annotation* annotation, int page) override;

private:
    Plugin::WorkerClient* m_backend = nullptr;
    MutationCallback m_mutationCallback;
    bool m_available = false;
};

} // namespace Mu::Generator::Proxy

#endif // MU_GENERATOR_PROXY_ANNOTATION_HPP
