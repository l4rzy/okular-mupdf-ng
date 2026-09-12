// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_SIGNATURE_HPP
#define MU_GENERATOR_PROXY_FORM_SIGNATURE_HPP

#include <okular/core/form.h>
#include <okular/core/version.h>

#include <QtCore/qglobal.h>

#include <utility>

#include "shared/model/types.hpp"

namespace Mu::Plugin {

class WorkerClient;

}

namespace Mu::Generator::Proxy::Form {

/// Okular signature field backed by worker-provided verification data.
///
/// Verification data is a per-document-load snapshot: proxies are rebuilt on
/// reload, so there are no live updates to subscribe to. Subscription requests
/// are therefore honestly refused (0 / false) instead of handing out handles
/// that would never fire.
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
#if OKULAR_VERSION >= QT_VERSION_CHECK(25, 8, 0) // Pair-returning signing API.
    std::pair<Okular::SigningResult, QString> sign(const Okular::NewSignatureData& data,
                                                   const QString& newPath) const override;
#else
    bool sign(const Okular::NewSignatureData& data, const QString& newPath) const override;
#endif
    SubscriptionHandle subscribeUpdates(const std::function<void()>& callback) const override;
    bool unsubscribeUpdates(const SubscriptionHandle& handle) const override;

    int id() const override { return m_id; }

private:
    std::pair<Okular::SigningResult, QString> signResult(const Okular::NewSignatureData& data,
                                                         const QString& newPath) const;

    int m_id;
    // Verification and display data are a snapshot from the active document.
    Model::SignatureField m_data;
    // Non-owning worker used only when remote signing support is enabled.
    [[maybe_unused]] Plugin::WorkerClient* m_backend;
};

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_SIGNATURE_HPP
