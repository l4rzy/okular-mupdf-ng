// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_KINDLE_DOCUMENT_HPP
#define MU_WORKER_ENGINE_KINDLE_DOCUMENT_HPP

#include "engine/epub/document.hpp"
#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

class KindleDocument final : public EpubDocument {
public:
    [[nodiscard]] Model::DocumentType type() const noexcept override { return Model::DocumentType::Kindle; }

    using EpubDocument::EpubDocument;

    [[nodiscard]] bool openFd(int fd, std::string displayName, std::string* error = nullptr) override;
    void close() noexcept override;
    [[nodiscard]] DocumentMetadata metadata(const std::vector<std::string>& keys,
                                            std::string* error = nullptr) const override;

protected:
    [[nodiscard]] bool writeExport(Sys::FileDescriptor output,
                                   const Model::ExportOptions& options,
                                   std::string* error,
                                   const ExportProgressCallback& progress) override;

private:
    Sys::FileDescriptor m_epub;
};

} // namespace Mu::Worker::Engine

#endif
