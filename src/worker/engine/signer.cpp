// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/signer.hpp"
#include "engine/constants.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

extern "C" {
#include <mupdf/pdf.h>
}

namespace Mu::Worker::Engine {
namespace {

struct Signer {
    pdf_pkcs7_signer base;
    std::atomic<int> refs { 1 };
    std::string nickname, subjectCommonName;
    CmsCallback callback;
    CmsResult result;
};

} // namespace

// =============================================================================
// MuPDF PKCS#7 Signer Adapter Implementation
// =============================================================================

// Builds a MuPDF pdf_pkcs7_signer backed by the CmsCallback. The vtable is
// wired from capturing lambdas into a heap Signer whose base pointer is
// returned; an atomic refcount mirrors MuPDF's keep/drop contract. MuPDF
// streams the exact signed byte ranges; only their SHA-256 digest crosses IPC.
pdf_pkcs7_signer* createSigner(std::string nickname, std::string subjectCommonName, CmsCallback callback)
{
    // Reference counting callbacks (Fitz keep / drop protocol)
    auto keep = [](fz_context*, pdf_pkcs7_signer* value) -> pdf_pkcs7_signer* {
        if (value)
            ++reinterpret_cast<Signer*>(value)->refs;
        return value;
    };
    auto drop = [](fz_context*, pdf_pkcs7_signer* value) {
        if (value && --reinterpret_cast<Signer*>(value)->refs == 0)
            delete reinterpret_cast<Signer*>(value);
    };

    // Signing name extractor for the signature appearance dictionary
    auto name = [](fz_context* context, pdf_pkcs7_signer* value) -> pdf_pkcs7_distinguished_name* {
        auto* signer = reinterpret_cast<Signer*>(value);
        if (signer->subjectCommonName.empty())
            return nullptr;
        pdf_pkcs7_distinguished_name* result = nullptr;
        bool complete = false;
        fz_var(result);
        fz_var(complete);
        fz_try(context)
        {
            result =
                static_cast<pdf_pkcs7_distinguished_name*>(fz_calloc(context, 1, sizeof(pdf_pkcs7_distinguished_name)));
            result->cn = fz_strdup(context, signer->subjectCommonName.c_str());
            complete = true;
        }
        fz_always(context)
        {
            if (!complete && result) {
                fz_free(context, result->cn);
                fz_free(context, result);
            }
        }
        fz_catch(context)
        {
            fz_rethrow(context);
        }
        return result;
    };

    // Maximum expected PKCS#7 / CMS blob size (64 KiB buffer)
    auto maxSize = [](fz_context*, pdf_pkcs7_signer*) -> size_t {
        return Constant::MaxPkcs7SignatureBufferBytes;
    };

    // Digest creation callback: streams signed byte ranges through MuPDF SHA-256
    auto create = [](fz_context* context,
                     pdf_pkcs7_signer* value,
                     fz_stream* input,
                     unsigned char* output,
                     size_t outputSize) -> int {
        auto* signer = reinterpret_cast<Signer*>(value);
        std::uint8_t buffer[Constant::DigestStreamingChunkBytes];
        std::array<std::uint8_t, 32> digest { };
        fz_sha256 hash { };
        fz_sha256_init(&hash);

        // Step 1: Stream bytes from input fz_stream through MuPDF SHA-256
        fz_try(context)
        {
            size_t count = 0;
            while ((count = fz_read(context, input, buffer, sizeof(buffer))) > 0)
                fz_sha256_update(&hash, buffer, count);
        }
        fz_catch(context)
        {
            return 0;
        }
        fz_sha256_final(&hash, digest.data());

        // Step 2: Invoke IPC callback to obtain CMS signature bytes from parent
        // NSS engine. This function is called through MuPDF's C callback ABI;
        // no C++ exception may escape into MuPDF, and callback failure becomes
        // a zero-length CMS result for MuPDF to reject.
        try {
            signer->result = signer->callback(digest, signer->nickname);
        } catch (...) {
            signer->result = { };
            return 0;
        }
        if (signer->result.result != ::Mu::Model::SigningResult::Success || signer->result.cmsSignature.empty()
            || signer->result.cmsSignature.size() > outputSize)
            return 0;

        // Step 3: Copy CMS signature into output buffer for PDF embedding
        std::memcpy(output, signer->result.cmsSignature.data(), signer->result.cmsSignature.size());
        return static_cast<int>(signer->result.cmsSignature.size());
    };

    auto* signer = new Signer;
    signer->nickname = std::move(nickname);
    signer->subjectCommonName = std::move(subjectCommonName);
    signer->callback = std::move(callback);
    signer->base.keep = keep;
    signer->base.drop = drop;
    signer->base.get_signing_name = name;
    signer->base.max_digest_size = maxSize;
    signer->base.create_digest = create;
    return &signer->base;
}

CmsResult signerResult(const pdf_pkcs7_signer* value)
{
    if (!value)
        return { };
    return reinterpret_cast<const Signer*>(value)->result;
}

} // namespace Mu::Worker::Engine
