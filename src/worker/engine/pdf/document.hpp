// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_WORKER_ENGINE_PDF_DOCUMENT_HPP
#define MU_WORKER_ENGINE_PDF_DOCUMENT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
}

#include "engine/constants.hpp"
#include "engine/document_base.hpp"
#include "engine/mupdf_helpers.hpp"
#include "engine/signer.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

/**
 * High-performance PDF document engine backed by libmupdf (`pdf_document`).
 *
 * Implements rendering, text extraction, annotation editing, digital signatures,
 * font listing, and attachment extraction for PDF files.
 *
 * Stream Management:
 * - Adopts the open input file descriptor into a `FILE*` stream via `fdopen`.
 * - Uses `fz_open_file_ptr_no_close` so Fitz never takes ownership of the underlying descriptor.
 * - Manages link resolution caches and memory trimming across open/close cycles.
 */
class PdfDocument : public DocumentBase {
public:
    explicit PdfDocument(std::size_t storeSize = Constant::DefaultStoreSize);
    ~PdfDocument() override;

    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;
    PdfDocument(PdfDocument&&) noexcept = delete;
    PdfDocument& operator=(PdfDocument&&) noexcept = delete;

    // -------------------------------------------------------------------------
    // Lifecycle & Document State
    // -------------------------------------------------------------------------

    [[nodiscard]] bool openFd(int fd, std::string displayName, std::string* error = nullptr) override;
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
    /// Discards the temporary link resolution cache used during incremental page-link aggregation.
    void discardResolvedLinkCache() noexcept;
    [[nodiscard]] std::vector<Link> extractLinks(int page, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<OutlineNode> outline(std::string* error = nullptr) const override;
    [[nodiscard]] DocumentMetadata metadata(const std::vector<std::string>& keys,
                                            std::string* error = nullptr) const override;

    // -------------------------------------------------------------------------
    // PDF Annotations, Signatures & Attachments
    // -------------------------------------------------------------------------

    [[nodiscard]] bool addAnnotation(int page,
                                     const Annotation& annotation,
                                     std::int32_t* objectNumber,
                                     std::string* error = nullptr) override;
    [[nodiscard]] bool modifyAnnotation(int page,
                                        std::int32_t objectNumber,
                                        const Annotation& annotation,
                                        bool appearanceChanged,
                                        std::string* error = nullptr) override;
    [[nodiscard]] bool removeAnnotation(int page, std::int32_t objectNumber, std::string* error = nullptr) override;
    [[nodiscard]] bool saveFd(int fd, std::string* error = nullptr) override;
    [[nodiscard]] bool savePdfFd(int fd, const std::vector<int>& pages, std::string* error = nullptr) override;
    [[nodiscard]] bool signFd(const Model::SignRequest& request,
                              CmsCallback callback,
                              int outputFd,
                              SigningResult* signingResult = nullptr,
                              std::string* error = nullptr) override;
    [[nodiscard]] bool updateFormField(int page,
                                       std::int32_t objectNumber,
                                       const Model::FormValue& value,
                                       std::vector<FieldMutation>* mutations,
                                       std::string* error = nullptr) override;
    [[nodiscard]] bool resetForm(int page,
                                 std::int32_t objectNumber,
                                 std::vector<FieldMutation>* mutations,
                                 std::string* error = nullptr) override;
    [[nodiscard]] std::vector<Font> fonts(const std::vector<int>& pages, std::string* error = nullptr) const override;
    [[nodiscard]] std::vector<EmbeddedFile> embeddedFiles(std::size_t maxBytes,
                                                          std::size_t maxFiles,
                                                          bool* resourceLimit,
                                                          std::string* error = nullptr) const override;

private:
    /// RAII helper executing a lambda callback with a target annotation handle.
    template <typename Callback>
    bool withAnnotation(int page, std::int32_t objectNumber, std::string* error, Callback&& callback);

    /// Recursively copies MuPDF outline nodes into OutlineNode model vector.
    [[nodiscard]] std::vector<OutlineNode>
    copyOutline(const fz_outline* source, std::size_t depth, std::size_t* count, std::string* error) const;

    /// Loads a page handle with exception protection.
    [[nodiscard]] fz_page* loadPage(int page, std::string* error) const;

    /// Updates the cached presence of the catalog AcroForm dictionary.
    void updateAcroFormPresence();

    /// Converts MuPDF page bounds rect to PageGeometry.
    [[nodiscard]] PageGeometry geometryFromPage(fz_page* page, const fz_rect& bounds) const;

    /// Parses annotations from a MuPDF page handle.
    [[nodiscard]] std::vector<Annotation>
    extractPageAnnotations(fz_page* page, const fz_rect& bounds, std::string* error) const;

    /// Parses signature fields from a MuPDF page handle.
    [[nodiscard]] std::vector<SignatureField> extractPageSignatures(fz_page* page, const fz_rect& bounds) const;

    /// Parses interactive form fields (text, checkbox, radio, choice) from a MuPDF page handle.
    [[nodiscard]] std::vector<FormField>
    extractPageFormFields(fz_page* page, const fz_rect& bounds, int pageIndex, std::string* error) const;

    /// Parses links from a MuPDF page handle.
    [[nodiscard]] std::vector<Link> extractPageLinks(fz_page* page, const fz_rect& bounds, std::string* error) const;

    /// Returns true if an annotation type is supported for editing.
    [[nodiscard]] static constexpr bool isEditableAnnotation(std::int32_t type) noexcept;

    /// Applies properties to a MuPDF target annotation.
    static void applyAnnotation(fz_context* context,
                                pdf_annot* target,
                                const Annotation& annotation,
                                const fz_rect& pageBounds,
                                bool updateAppearance);

    /// Parses a PDF date string object into Timestamp model.
    static Timestamp parsePdfDate(fz_context* context, pdf_obj* object);

    /// Parses a PDF filespec object into EmbeddedFile model.
    static EmbeddedFile parseFilespec(fz_context* context, pdf_obj* object, std::size_t remainingBytes);

    /// Collects embedded attachment tree nodes recursively.
    static void collectEmbeddedTree(fz_context* context,
                                    pdf_obj* node,
                                    std::vector<EmbeddedFile>& output,
                                    int depth,
                                    std::size_t& remainingBytes,
                                    std::size_t& remainingFiles,
                                    bool* resourceLimit);

    /// Calculates device scale transformation matrix.
    [[nodiscard]] static fz_matrix pageToDevice(const fz_rect& bounds, float scaleX, float scaleY) noexcept;

    /// Maps a PDF font subtype string to font type enum.
    [[nodiscard]] static Model::FontType mapFontType(const char* subtype) noexcept;

    // MuPDF context state.
    fz_context* m_context = nullptr;
    fz_document* m_document = nullptr;
    fz_stream* m_stream = nullptr;
    FILE* m_input = nullptr;
    std::int64_t m_sourceSize = 0;

    std::string m_displayName;
    int m_pageCount = 0;
    bool m_locked = false;
    bool m_hasAcroForm = false;
    DocumentSettings m_settings;

    // Link destinations are immutable while a document is open. Keep a bounded
    // cache because page metadata can resolve the same URI more than once.
    mutable std::unordered_map<std::string, ResolvedLink> m_resolvedLinks;
    mutable std::size_t m_resolvedLinkKeyBytes = 0;
    bool m_resolvedLinkCacheEnabled = true;
};

} // namespace Mu::Worker::Engine

#endif // MU_WORKER_ENGINE_PDF_DOCUMENT_HPP
