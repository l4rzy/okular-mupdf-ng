// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_DOCUMENT_FINGERPRINT_HPP
#define MU_WORKER_ENGINE_DOCUMENT_FINGERPRINT_HPP

#include <string>
#include <string_view>

#define XXH_INLINE_ALL
#include <xxhash/xxhash.h>
#undef XXH_INLINE_ALL

namespace Mu::Worker::Engine {

// Non-cryptographic identity for OCR caching and document recovery.
class DocumentFingerprint {
public:
    DocumentFingerprint() { XXH3_128bits_reset(&m_hash); }

    void addData(std::string_view data) { XXH3_128bits_update(&m_hash, data.data(), data.size()); }

    void addMetadataField(std::string_view name, std::string_view value)
    {
        addData(name);
        addData(":");
        addData(value);
        addData(std::string_view("\0", 1));
    }

    [[nodiscard]] std::string hexDigest() const
    {
        XXH128_canonical_t digest;
        XXH128_canonicalFromHash(&digest, XXH3_128bits_digest(&m_hash));
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(sizeof(digest.digest) * 2);
        for (const auto byte : digest.digest) {
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
        return result;
    }

private:
    XXH3_state_t m_hash;
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_DOCUMENT_FINGERPRINT_HPP
