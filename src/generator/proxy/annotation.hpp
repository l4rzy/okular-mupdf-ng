// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_ANNOTATION_HPP
#define MU_GENERATOR_PROXY_ANNOTATION_HPP

#include <okular/core/annotations.h>

namespace Mu::Plugin {

class WorkerClient;

}

namespace Mu::Generator::Proxy {

class Annotation final : public Okular::AnnotationProxy {
public:
    explicit Annotation(Plugin::WorkerClient* backend = nullptr);
    ~Annotation() override;

    bool supports(Capability capability) const override;
    void notifyAddition(Okular::Annotation* annotation, int page) override;
    void notifyModification(const Okular::Annotation* annotation, int page, bool appearanceChanged) override;
    void notifyRemoval(Okular::Annotation* annotation, int page) override;

private:
    Plugin::WorkerClient* m_backend = nullptr;
};

} // namespace Mu::Generator::Proxy

#endif // MU_GENERATOR_PROXY_ANNOTATION_HPP
