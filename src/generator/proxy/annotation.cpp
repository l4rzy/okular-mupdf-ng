// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/annotation.hpp"

#include <okular/core/document.h>

#include "generator/conversion/annotation.hpp"
#include "generator/signature_image.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/worker_client.hpp"

namespace Mu::Generator::Proxy {
namespace {

std::pair<Okular::SigningResult, QString> signingResult(const Model::SignResponse& result)
{
    const QString details = QString::fromStdString(result.details);
    switch (result.result) {
    case Model::SigningResult::Success:
        return { Okular::SigningSuccess, { } };
    case Model::SigningResult::FieldAlreadySigned:
        return { Okular::FieldAlreadySigned, details };
    case Model::SigningResult::KeyMissing:
        return { Okular::KeyMissing, details };
    case Model::SigningResult::WriteFailed:
        return { Okular::SignatureWriteFailed, details };
    case Model::SigningResult::UserCancelled:
        return { Okular::UserCancelled, details };
    case Model::SigningResult::BadPassphrase:
        return { Okular::BadPassphrase, details };
    default:
        return { Okular::GenericSigningError, details };
    }
}

} // namespace

Annotation::Annotation(Plugin::WorkerClient* backend)
    : m_backend(backend)
{
}

Annotation::~Annotation() = default;

bool Annotation::supports(Capability capability) const
{
    return m_backend && m_backend->isConnected()
        && (capability == Addition || capability == Modification || capability == Removal);
}

void Annotation::notifyAddition(Okular::Annotation* annotation, int page)
{
    if (!m_backend || !m_backend->isConnected() || !annotation)
        return;
    if (auto* signature = dynamic_cast<Okular::SignatureAnnotation*>(annotation)) {
        signature->setPage(page);
        const Okular::NormalizedRect bounds = signature->boundingRectangle();
        Plugin::WorkerClient* const backend = m_backend;
        signature->setSignFunction(
            [backend, page, bounds, signature](const Okular::NewSignatureData& data, const QString& fileName) {
                if (!backend || !backend->isConnected())
                    return std::make_pair(Okular::GenericSigningError, QStringLiteral("MuPDF worker is unavailable"));
                const QString commonName = Plugin::Crypto::signingCertificateCommonName(data.certNickname());
                if (commonName.isEmpty())
                    return std::make_pair(Okular::KeyMissing, QStringLiteral("Signing certificate was not found"));
                const QString imagePath =
                    !data.backgroundImagePath().isEmpty() ? data.backgroundImagePath() : signature->imagePath();
                const auto bgImage = SignatureImage::prepareBackgroundImage(imagePath, bounds);
                return signingResult(backend->sign({ { },
                                                     page,
                                                     { bounds.left, bounds.top, bounds.right, bounds.bottom },
                                                     data.certNickname().toStdString(),
                                                     commonName.toStdString(),
                                                     data.reason().toStdString(),
                                                     data.location().toStdString(),
                                                     -1,
                                                     bgImage },
                                                   data.password(),
                                                   fileName));
            });
        return;
    }
    const auto model = Conversion::toModel(annotation);
    if (!model)
        return;
    const auto result = m_backend->addAnnotation(page, *model);
    if (result) {
        const QString id = QString::fromStdString(result->value);
        annotation->setNativeId(id);
        // The worker, rather than Okular's overlay, draws native PDF
        // annotations. This makes Document refresh the page raster now.
        annotation->setFlags(annotation->flags() | Okular::Annotation::ExternallyDrawn);
    }
}

void Annotation::notifyModification(const Okular::Annotation* annotation, int page, bool appearanceChanged)
{
    if (!m_backend || !m_backend->isConnected() || !annotation)
        return;
    const auto model = Conversion::toModel(annotation);
    if (!model)
        return;
    const QVariant id = annotation->nativeId();
    if (!id.isValid() || id.toString().isEmpty())
        return;
    m_backend->modifyAnnotation(page, id.toString(), *model, appearanceChanged);
}

void Annotation::notifyRemoval(Okular::Annotation* annotation, int page)
{
    if (!m_backend || !m_backend->isConnected() || !annotation)
        return;
    const QVariant id = annotation->nativeId();
    if (!id.isValid() || id.toString().isEmpty())
        return;
    if (m_backend->removeAnnotation(page, id.toString()))
        annotation->setNativeId(QVariant());
}

} // namespace Mu::Generator::Proxy
