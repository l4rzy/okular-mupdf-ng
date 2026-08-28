// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_SIGNATURE_HPP
#define MU_GENERATOR_PROXY_FORM_SIGNATURE_HPP

#include <okular/core/form.h>

#include <map>
#include <mutex>

#include "shared/model/types.hpp"

namespace Mu::Plugin {

class WorkerClient;

}

namespace Mu::Generator::Proxy::Form {

/// Okular signature field backed by worker-provided verification data.
///
/// Optional remote signing uses the worker backend; subscription bookkeeping is
/// kept local so Okular can observe updates without owning the backend.
class Signature final : public Okular::FormFieldSignature {
public:
    Signature(int id, Model::SignatureField data, Plugin::WorkerClient* backend = nullptr);
    ~Signature() override;

    Okular::NormalizedRect rect() const override;
    QString name() const override;
    QString uiName() const override;
    QString fullyQualifiedName() const override;
    bool isReadOnly() const override;
    bool isVisible() const override;

    Okular::FormFieldSignature::SignatureType signatureType() const override;
    Okular::SignatureInfo signatureInfo() const override;
    std::pair<Okular::SigningResult, QString> sign(const Okular::NewSignatureData& data,
                                                   const QString& newPath) const override;
    SubscriptionHandle subscribeUpdates(const std::function<void()>& callback) const override;
    bool unsubscribeUpdates(const SubscriptionHandle& handle) const override;

    int id() const override { return m_id; }

private:
    int m_id;
    // Verification and display data are a snapshot from the active document.
    Model::SignatureField m_data;
    // Non-owning worker used only when remote signing support is enabled.
    [[maybe_unused]] Plugin::WorkerClient* m_backend;
    // Protects callbacks because Okular may subscribe/unsubscribe from workers.
    mutable std::mutex m_subscriptionMutex;
    mutable std::map<SubscriptionHandle, std::function<void()>> m_subscriptions;
    mutable SubscriptionHandle m_nextSubscriptionHandle = 1;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_SIGNATURE_HPP
