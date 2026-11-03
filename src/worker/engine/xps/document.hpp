// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_XPS_DOCUMENT_HPP
#define MU_WORKER_ENGINE_XPS_DOCUMENT_HPP

#include "engine/constants.hpp"
#include "engine/document_base.hpp"
#include "engine/mupdf_helpers.hpp"
#include "engine/page_cache.hpp"

#include <cstdio>
#include <optional>
#include <unordered_map>

namespace Mu::Worker::Engine {

/// Independent fixed-layout XPS/OpenXPS engine backed by MuPDF.
class XpsDocument final : public DocumentBase {
public:
    explicit XpsDocument(std::size_t storeSize = Constant::DefaultStoreSize);
    ~XpsDocument() override;

    [[nodiscard]] Model::DocumentType type() const noexcept override { return Model::DocumentType::Xps; }

    [[nodiscard]] bool openFd(int fd, std::string displayName, std::string* error = nullptr) override;
    [[nodiscard]] bool unlock(const std::string& password, std::string* error = nullptr) override;
    void close() noexcept override;
    void trimMemoryForIdle() noexcept override;

    [[nodiscard]] bool isOpen() const noexcept override { return m_document != nullptr; }

    [[nodiscard]] bool isLocked() const noexcept override { return false; }

    [[nodiscard]] int pageCount() const noexcept override { return m_pageCount; }

    [[nodiscard]] PageGeometry pageGeometry(int page, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<Annotation> extractAnnotations(int page, std::string* error = nullptr) const override;
    [[nodiscard]] PageDetails pageDetails(int page,
                                          std::string* error = nullptr,
                                          bool includeLinks = true,
                                          ExtractionBudgets* budgets = nullptr) const override;
    [[nodiscard]] bool renderToBuffer(const RenderRequest& request,
                                      void* dstPixels,
                                      std::size_t dstStride,
                                      std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<TextBox> textBoxes(int page,
                                                 double dpiX,
                                                 double dpiY,
                                                 std::size_t maxBoxes,
                                                 bool skipAnnots = false,
                                                 std::string* error = nullptr) const override;

    [[nodiscard]] DocumentSettings settings() const noexcept override { return m_settings; }

    void setSettings(const DocumentSettings& settings) noexcept override;
    [[nodiscard]] ResolvedLink resolveLink(const std::string& uri, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<Link>
    extractLinks(int page, std::string* error = nullptr, ExtractionBudgets* budgets = nullptr) const override;
    void discardResolvedLinkCache() noexcept override;
    [[nodiscard]] std::vector<OutlineNode> outline(std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<Font> fonts(const std::vector<int>& pages, std::string* error = nullptr) const override;
    [[nodiscard]] DocumentMetadata metadata(const std::vector<std::string>& keys,
                                            std::string* error = nullptr) const override;
    [[nodiscard]] bool savePdfFd(int fd, const std::vector<int>& pages, std::string* error = nullptr) override;

private:
    [[nodiscard]] fz_page* loadPage(int page, std::string* error) const;
    [[nodiscard]] fz_page* loadPageWithBounds(int page, fz_rect* bounds, std::string* error) const;
    void setPageCacheSuspended(bool suspended) noexcept override;
    [[nodiscard]] std::vector<Link>
    extractPageLinks(fz_page* page, const fz_rect& bounds, std::string* error, ByteBudget& budget) const;
    [[nodiscard]] std::vector<OutlineNode> copyOutline(
        const fz_outline* source, std::size_t depth, std::size_t& count, ByteBudget& budget, std::string* error) const;

    fz_context* m_context = nullptr;
    fz_document* m_document = nullptr;
    fz_stream* m_stream = nullptr;
    FILE* m_input = nullptr;
    int m_pageCount = 0;
    DocumentSettings m_settings;
    PageCache m_pageCache;
    mutable std::optional<std::vector<Font>> m_fonts;
    mutable std::optional<std::string> m_hash;
    mutable std::unordered_map<std::string, ResolvedLink> m_resolvedLinks;
    mutable std::size_t m_resolvedLinkKeyBytes = 0;
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_XPS_DOCUMENT_HPP
