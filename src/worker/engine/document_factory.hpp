// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <memory>

#include "engine/document_base.hpp"

namespace Mu::Worker::Engine {

/// Creates the engine for a supported document type; unknown types return null.
[[nodiscard]] std::unique_ptr<DocumentBase> createDocument(Model::DocumentType type, std::size_t storeSize);

} // namespace Mu::Worker::Engine
