// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/document_fingerprint.hpp"
#include "engine/xps/document.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <string_view>
#include <unistd.h>

namespace Mu::Worker::Engine {

DocumentMetadata XpsDocument::metadata(const std::vector<std::string>& keys, std::string* error) const
{
    if (!m_document) {
        fail(error, "document is unavailable");
        return { };
    }
    DocumentMetadata result;
    result.pageCount = m_pageCount;
    result.mimeType = Model::documentTypeToMime(type());
    const auto wanted = [&keys](const char* name) {
        return keys.empty() || std::find(keys.begin(), keys.end(), name) != keys.end();
    };
    // MuPDF's XPS handler exposes only the format name, not Info metadata.
    if (wanted("format"))
        result.values.emplace("format", "XPS");
    if (wanted("repaired"))
        result.values.emplace("repaired", "false");
    if (wanted("hash")) {
        if (!m_hash) {
            // Metadata hashes collide for XPS files with the same page count.
            // Hash the bytes without disturbing MuPDF's shared stream offset.
            DocumentFingerprint hash;
            std::array<char, Constant::DigestStreamingChunkBytes> buffer;
            off_t offset = 0;
            for (;;) {
                const auto bytes = ::pread(::fileno(m_input), buffer.data(), buffer.size(), offset);
                if (bytes < 0) {
                    if (errno == EINTR)
                        continue;
                    fail(error, "could not hash XPS document");
                    return { };
                }
                if (!bytes)
                    break;
                hash.addData(std::string_view(buffer.data(), static_cast<std::size_t>(bytes)));
                offset += bytes;
            }
            m_hash = hash.hexDigest();
        }
        result.values.emplace("hash", *m_hash);
    }
    return result;
}

} // namespace Mu::Worker::Engine
