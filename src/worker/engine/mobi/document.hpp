// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_MOBI_DOCUMENT_HPP
#define MU_WORKER_ENGINE_MOBI_DOCUMENT_HPP

#include "engine/epub/document.hpp"

namespace Mu::Worker::Engine {

// Legacy MOBI uses MuPDF's HTML reflow engine and the existing EPUB settings.
class MobiDocument final : public EpubDocument {
public:
    using EpubDocument::EpubDocument;

    [[nodiscard]] bool openFd(int fd, std::string displayName, std::string* error = nullptr) override;
    [[nodiscard]] DocumentMetadata metadata(const std::vector<std::string>& keys,
                                            std::string* error = nullptr) const override;
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_MOBI_DOCUMENT_HPP
