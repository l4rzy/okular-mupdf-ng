// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_ENGINE_EPUB_DOCUMENT_HPP
#define MUPDF_WORKER_ENGINE_EPUB_DOCUMENT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/fitz/archive.h>
}

#include "engine/constants.hpp"
#include "engine/document_base.hpp"
#include "engine/mupdf_helpers.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

/**
 * EPUB reflowable document engine implementation backed by libmupdf.
 *
 * Reflow & Styling Pipeline:
 * 1. Base64 custom CSS decoding with UTF-8 verification.
 * 2. Automatic margin calculation and image reflow safeguards (height: auto, max-width: 100%).
 * 3. User CSS and font-family injection via `fz_style_document`.
 * 4. Paginated document layout via `fz_layout_document` at target page dimensions (A5, B5, Letter, 6x9).
 */
class EpubDocument final : public DocumentBase {
public:
    explicit EpubDocument(std::size_t storeSize = Constant::DefaultStoreSize);
    ~EpubDocument() override;

    EpubDocument(const EpubDocument&) = delete;
    EpubDocument& operator=(const EpubDocument&) = delete;
    EpubDocument(EpubDocument&&) noexcept = delete;
    EpubDocument& operator=(EpubDocument&&) noexcept = delete;

    // -------------------------------------------------------------------------
    // Lifecycle & Document State
    // -------------------------------------------------------------------------

    [[nodiscard]] bool openFd(int fd, std::string displayName, std::string* error = nullptr) override;
    [[nodiscard]] bool openFdWithAccelerator(int fd,
                                             std::string displayName,
                                             const std::vector<std::uint8_t>& accelerator,
                                             std::string* error = nullptr);
    [[nodiscard]] std::vector<std::uint8_t> exportAccelerator(std::string* error = nullptr) const;
    [[nodiscard]] bool unlock(const std::string& password, std::string* error = nullptr) override;
    void close() noexcept override;
    [[nodiscard]] bool isOpen() const noexcept override;
    [[nodiscard]] bool isLocked() const noexcept override;
    [[nodiscard]] int pageCount() const noexcept override;

    [[nodiscard]] fz_context* context() const noexcept { return m_context; }

    [[nodiscard]] fz_document* document() const noexcept { return m_document; }

    // -------------------------------------------------------------------------
    // Page Geometry, Details & Rendering
    // -------------------------------------------------------------------------

    [[nodiscard]] PageGeometry pageGeometry(int page, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<Annotation> extractAnnotations(int page, std::string* error = nullptr) const override;
    [[nodiscard]] PageDetails
    pageDetails(int page, std::string* error = nullptr, bool includeLinks = true) const override;
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
    [[nodiscard]] DocumentSettings settings() const noexcept override;
    void setSettings(const DocumentSettings& settings) noexcept override;
    [[nodiscard]] ResolvedLink resolveLink(const std::string& uri, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<Link> extractLinks(int page, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<OutlineNode> outline(std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<Font> fonts(const std::vector<int>& pages, std::string* error = nullptr) const override;
    [[nodiscard]] DocumentMetadata metadata(const std::vector<std::string>& keys,
                                            std::string* error = nullptr) const override;
    [[nodiscard]] bool savePdfFd(int fd, const std::vector<int>& pages, std::string* error = nullptr) override;

private:
    struct LayoutGeometry {
        float paperWidth = 0;
        float paperHeight = 0;
    };

    /// Returns the configured paper and its inset content area.
    [[nodiscard]] LayoutGeometry layoutGeometry() const noexcept;

    /// Lays out the EPUB once at the configured content dimensions and font size.
    [[nodiscard]] bool layoutTo(float widthPoints, float heightPoints, float fontSize) const;

    /// Loads an EPUB page handle for given index.
    [[nodiscard]] fz_page* loadPage(int page, std::string* error) const;

    /// Loads a page and obtains its bounds. The caller owns a non-null result
    /// and must drop it in the operation's fz_always cleanup block.
    [[nodiscard]] fz_page* loadPageWithBounds(int page, fz_rect* bounds, std::string* error) const;

    /// Extracts links from an already-loaded EPUB page.
    [[nodiscard]] std::vector<Link> extractPageLinks(fz_page* page, const fz_rect& bounds, std::string* error) const;

    /// Recursively copies MuPDF outline nodes into OutlineNode model vector.
    [[nodiscard]] std::vector<OutlineNode>
    copyOutline(const fz_outline* source, std::size_t depth, std::size_t* count, std::string* error) const;

    // MuPDF context state
    fz_context* m_context = nullptr;
    mutable fz_document* m_document = nullptr;
    fz_stream* m_stream = nullptr;
    FILE* m_input = nullptr;

    std::string m_displayName;
    mutable int m_pageCount = 0;
    mutable float m_layoutWidth = 0;
    mutable float m_layoutHeight = 0;
    mutable fz_archive* m_archive = nullptr;
    mutable std::optional<std::vector<Font>> m_fonts;
    DocumentSettings m_settings;
};

} // namespace Mu::Worker::Engine

#endif
