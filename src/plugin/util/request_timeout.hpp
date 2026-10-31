// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_REQUEST_TIMEOUT_HPP
#define MU_PLUGIN_UTIL_REQUEST_TIMEOUT_HPP

#include <type_traits>
#include <variant>

#include "shared/model/types.hpp"
#include "shared/transport/common.hpp"

namespace Mu::Plugin::Util {

/// Returns the response deadline in milliseconds for a worker request.
inline int requestTimeoutFor(const Model::RequestPayload& payload)
{
    namespace Timeout = ::Mu::IPC::Timeout;
    return std::visit(
        [](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Model::RenderRequest>)
                return Timeout::RenderMs;
            if constexpr (std::is_same_v<T, Model::OcrPageRequest> || std::is_same_v<T, Model::OcrResultRequest>)
                return Timeout::OcrMs;
            if constexpr (std::is_same_v<T, Model::OpenRequest>)
                return value.documentType == Model::DocumentType::Kindle ? Timeout::ExportMs : Timeout::GenericOpMs;
            if constexpr (std::is_same_v<T, Model::SaveRequest>)
                return value.exportEpub || value.exportAzw3 ? Timeout::ExportMs : Timeout::GenericOpMs;
            if constexpr (std::is_same_v<T, Model::SavePdfRequest>)
                return Timeout::PdfConversionMs;
            if constexpr (std::is_same_v<T, Model::ExportPdfAsyncRequest>)
                // Only the job submission round-trip is bounded here; the
                // export itself completes out-of-band via a notification.
                return Timeout::GenericOpMs;
            if constexpr (std::is_same_v<T, Model::SynopsisRequest>)
                return value.generateFallback ? 30'000 : Timeout::GenericOpMs;
            if constexpr (std::is_same_v<T, Model::SignRequest>)
                return Timeout::SignMs;
            return Timeout::GenericOpMs;
        },
        payload);
}

} // namespace Mu::Plugin::Util

#endif
