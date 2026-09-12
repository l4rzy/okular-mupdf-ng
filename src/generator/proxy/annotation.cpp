// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/proxy/annotation.hpp"

#include <okular/core/document.h>
#include <okular/core/version.h>

#include <QtCore/qglobal.h>

#include <cmath>
#include <utility>

#include "generator/conversion/annotation.hpp"
#include "generator/conversion/signing.hpp"
#include "plugin/crypto/nss.hpp"
#include "plugin/util/signature_image.hpp"
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

Annotation::Annotation(Plugin::WorkerClient* backend, MutationCallback mutationCallback)
    : m_backend(backend)
    , m_mutationCallback(std::move(mutationCallback))
{
}

Annotation::~Annotation() = default;

bool Annotation::supports(Capability capability) const
{
    return m_available && m_backend && m_backend->isConnected()
        && (capability == Addition || capability == Modification || capability == Removal);
}

void Annotation::notifyAddition(Okular::Annotation* annotation, int page)
{
    // The worker owns page ranges; reject only what is locally known bad so a
    // corrupt Okular page index never becomes a blocking worker IPC.
    if (!m_available || !m_backend || !m_backend->isConnected() || !annotation || page < 0)
        return;
    if (auto* signature = dynamic_cast<Okular::SignatureAnnotation*>(annotation)) {
        signature->setPage(page);
        Plugin::WorkerClient* const backend = m_backend;
        // Bounds are read live at signing time: Okular lets the user move or
        // resize the widget after notifyAddition, and signature edits never
        // reach the worker (toModel rejects SignatureAnnotation), so a
        // captured rect would sign stale coordinates.
        const auto sign =
            [backend, page, signature](const Okular::NewSignatureData& data,
                                       const QString& fileName) -> std::pair<Okular::SigningResult, QString> {
            // Only the backend state can change between registration and
            // invocation: page was validated on addition and the lambda dies
            // with its owning signature object.
            if (!backend || !backend->isConnected())
                return std::make_pair(Okular::GenericSigningError, QStringLiteral("MuPDF worker is unavailable"));
            const Okular::NormalizedRect bounds = signature->boundingRectangle();
            if (!std::isfinite(bounds.left) || !std::isfinite(bounds.top) || !std::isfinite(bounds.right)
                || !std::isfinite(bounds.bottom) || bounds.width() <= 0 || bounds.height() <= 0)
                return std::make_pair(Okular::GenericSigningError, QStringLiteral("Signature bounds are invalid"));
            const QString commonName = Plugin::Crypto::signingCertificateCommonName(data.certNickname());
            if (commonName.isEmpty())
                return std::make_pair(Okular::KeyMissing, QStringLiteral("Signing certificate was not found"));
            const QString imagePath =
                !data.backgroundImagePath().isEmpty() ? data.backgroundImagePath() : signature->imagePath();
            auto appearance = Conversion::toModelSignatureAppearance(data);
            appearance.backgroundImage =
                Plugin::Util::SignatureImage::prepareBackgroundImage(imagePath, bounds.width(), bounds.height());
            return signingResult(backend->sign({ { },
                                                 page,
                                                 { bounds.left, bounds.top, bounds.right, bounds.bottom },
                                                 data.certNickname().toStdString(),
                                                 commonName.toStdString(),
                                                 -1,
                                                 std::move(appearance) },
                                               data.password(),
                                               fileName));
        };
#if OKULAR_VERSION >= QT_VERSION_CHECK(25, 8, 0) // Pair-returning signing API.
        signature->setSignFunction(sign);
#else
        signature->setSignFunction([sign](const Okular::NewSignatureData& data, const QString& fileName) {
            return sign(data, fileName).first;
        });
#endif
        return;
    }
    const auto model = Conversion::toModel(annotation);
    if (!model)
        return;
    const auto result = m_backend->addAnnotation(page, *model);
    if (result) {
        if (m_mutationCallback)
            m_mutationCallback();
        const QString id = QString::fromStdString(result->value);
        annotation->setNativeId(id);
        // The worker, rather than Okular's overlay, draws native PDF
        // annotations. This makes Document refresh the page raster now.
        annotation->setFlags(annotation->flags() | Okular::Annotation::ExternallyDrawn);
    }
}

void Annotation::notifyModification(const Okular::Annotation* annotation, int page, bool appearanceChanged)
{
    if (!m_available || !m_backend || !m_backend->isConnected() || !annotation || page < 0)
        return;
    const auto model = Conversion::toModel(annotation);
    if (!model)
        return;
    const QVariant id = annotation->nativeId();
    if (!id.isValid() || id.toString().isEmpty())
        return;
    if (m_backend->modifyAnnotation(page, id.toString(), *model, appearanceChanged)) {
        if (m_mutationCallback)
            m_mutationCallback();
    }
}

void Annotation::notifyRemoval(Okular::Annotation* annotation, int page)
{
    if (!m_available || !m_backend || !m_backend->isConnected() || !annotation || page < 0)
        return;
    const QVariant id = annotation->nativeId();
    if (!id.isValid() || id.toString().isEmpty())
        return;
    if (m_backend->removeAnnotation(page, id.toString())) {
        if (m_mutationCallback)
            m_mutationCallback();
        annotation->setNativeId(QVariant());
    }
}

} // namespace Mu::Generator::Proxy
