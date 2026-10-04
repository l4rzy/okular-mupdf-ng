// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_EXTRACTION_BUDGET_HPP
#define MU_WORKER_ENGINE_EXTRACTION_BUDGET_HPP

#include <cstddef>
#include <cstring>

#include "shared/protocol/limits.hpp"

namespace Mu::Worker::Engine {

// Shared across pages. Charge before allocating copied text or container storage.
struct ByteBudget {
    std::size_t remainingBytes = ::Mu::Limit::MaxAggregateMetadataBytes;

    bool charge(std::size_t bytes) noexcept
    {
        if (bytes > remainingBytes)
            return false;
        remainingBytes -= bytes;
        return true;
    }

    bool chargeText(const char* text, std::size_t maxBytes = ::Mu::Limit::MaxString) noexcept
    {
        if (!text)
            return true;
        const auto bytes = strnlen(text, maxBytes + 1);
        return bytes <= maxBytes && charge(bytes);
    }
};

// Keep independent limits while sharing their lifetime across document extraction.
struct ExtractionBudgets {
    ByteBudget forms { ::Mu::Limit::MaxAggregateFormTextBytes };
    ByteBudget metadata { ::Mu::Limit::MaxAggregateMetadataBytes };
    ByteBudget links { ::Mu::Limit::MaxAggregateMetadataBytes };
};

inline constexpr const char* ExtractionBudgetError = "resource limit: document metadata size exceeded";

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_EXTRACTION_BUDGET_HPP
