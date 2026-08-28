// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_ENGINE_SIGNER_HPP
#define MUPDF_WORKER_ENGINE_SIGNER_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/pdf.h>
}

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

/// Result container returned by the digital signature CMS signing callback.
struct CmsResult {
    ::Mu::Model::SigningResult result = ::Mu::Model::SigningResult::GenericError;
    std::string details;
    std::vector<std::uint8_t> cmsSignature;
};

/// Callback signature invoked during digital signing to produce a PKCS#7 / CMS signature.
/// Streams SHA-256 document digest across IPC to the unisolated parent process NSS crypto engine.
using CmsCallback =
    std::function<CmsResult(const std::array<std::uint8_t, 32>& sha256Digest, const std::string& certificateNickname)>;

/**
 * Creates a custom MuPDF `pdf_pkcs7_signer` wrapper.
 *
 * Security Model:
 * 1. MuPDF streams byte ranges (/ByteRange array) through MuPDF SHA-256 hashing.
 * 2. Only the computed 32-byte digest is sent across IPC via `CmsCallback`.
 * 3. The isolated worker process never has direct access to user private keys or PKCS#11 tokens.
 *
 * @param nickname Certificate nickname identifying the signing identity.
 * @param subjectCommonName Common Name (CN) displayed on the visual signature line.
 * @param callback Callback invoked when the digest is ready for PKCS#7 / CMS signing.
 * @return Heap-allocated `pdf_pkcs7_signer` pointer owned and reference-counted by MuPDF.
 */
pdf_pkcs7_signer* createSigner(std::string nickname, std::string subjectCommonName, CmsCallback callback);

/// Retrieves the execution result from a custom MuPDF signer handle.
CmsResult signerResult(const pdf_pkcs7_signer* signer);

} // namespace Mu::Worker::Engine

#endif
