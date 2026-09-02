// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_WORKER_ENGINE_DOCUMENT_BASE_HPP
#define MUPDF_WORKER_ENGINE_DOCUMENT_BASE_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/signer.hpp"
#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

using ::Mu::Model::Annotation;
using ::Mu::Model::DocumentMetadata;
using ::Mu::Model::DocumentSettings;
using ::Mu::Model::EmbeddedFile;
using ::Mu::Model::Font;
using ::Mu::Model::FormField;
using ::Mu::Model::Link;
using ::Mu::Model::NormalizedRect;
using ::Mu::Model::OutlineNode;
using ::Mu::Model::PageGeometry;
using ::Mu::Model::ResolvedLink;
using ::Mu::Model::SignatureField;
using ::Mu::Model::SigningResult;
using ::Mu::Model::TextBox;
using ::Mu::Model::Timestamp;

/**
 * Abstract base class representing a document engine instance (PDF, EPUB).
 *
 * Encapsulates format-agnostic document operations (opening, rendering, text extraction,
 * outline, metadata) while providing virtual hooks for format-specific features (annotations,
 * digital signatures, font listing, attachment extraction).
 */
class DocumentBase {
public:
    /// Combined metadata and element details for a single page.
    struct PageDetails {
        PageGeometry geometry;
        std::vector<Annotation> annotations;
        std::vector<SignatureField> signatures;
        std::vector<Link> links;
        std::vector<FormField> formFields;
    };

    /// Sub-tile pixel coordinates for partial page rendering.
    struct RenderTile {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    /// Page rendering request parameters.
    struct RenderRequest {
        int page = -1;
        int width = 0;
        int height = 0;
        std::optional<RenderTile> tile;
    };

    DocumentBase() = default;
    virtual ~DocumentBase() = default;

    DocumentBase(const DocumentBase&) = delete;
    DocumentBase& operator=(const DocumentBase&) = delete;
    DocumentBase(DocumentBase&&) noexcept = delete;
    DocumentBase& operator=(DocumentBase&&) noexcept = delete;

    // -------------------------------------------------------------------------
    // Lifecycle & Document State
    // -------------------------------------------------------------------------

    /// Opens a document from an inherited open file descriptor.
    /// Takes ownership of `fd` regardless of success or failure.
    [[nodiscard]] virtual bool openFd(int fd, std::string displayName, std::string* error = nullptr) = 0;

    /// Attempts to unlock a password-protected document.
    [[nodiscard]] virtual bool unlock(const std::string& password, std::string* error = nullptr) = 0;

    /// Closes the active document and releases associated context resources.
    virtual void close() noexcept = 0;

    /// Returns true if a document file is currently loaded and valid.
    [[nodiscard]] virtual bool isOpen() const noexcept = 0;

    /// Returns true if the document is encrypted and requires a password to unlock.
    [[nodiscard]] virtual bool isLocked() const noexcept = 0;

    /// Records whether the open sequence required a non-empty password.
    /// Called by the runtime after a successful unlock sequence.
    void setPasswordRequired(bool required) noexcept { m_passwordRequired = required; }

    /// Returns the total number of loadable pages in the document.
    [[nodiscard]] virtual int pageCount() const noexcept = 0;

    // -------------------------------------------------------------------------
    // Page Geometry, Details & Rendering
    // -------------------------------------------------------------------------

    /// Queries the bounding box geometry for a given page index.
    [[nodiscard]] virtual PageGeometry pageGeometry(int page, std::string* error = nullptr) const = 0;

    /// Extracts all annotations present on a given page index.
    [[nodiscard]] virtual std::vector<Annotation> extractAnnotations(int page, std::string* error = nullptr) const = 0;

    /// Queries page geometry, annotations, signatures, and links in a single optimized pass.
    [[nodiscard]] virtual PageDetails
    pageDetails(int page, std::string* error = nullptr, bool includeLinks = true) const = 0;

    /// Renders a page or page tile directly into a pre-allocated target pixel buffer (RGBA8888).
    [[nodiscard]] virtual bool renderToBuffer(const RenderRequest& request,
                                              void* dstPixels,
                                              std::size_t dstStride,
                                              std::string* error = nullptr) const = 0;

    /// Extracts positioned text boxes for a page at given target DPI scale.
    [[nodiscard]] virtual std::vector<TextBox> textBoxes(int page,
                                                         double dpiX,
                                                         double dpiY,
                                                         std::size_t maxBoxes,
                                                         bool skipAnnots = false,
                                                         std::string* error = nullptr) const = 0;

    /// Returns active document configuration and quality settings.
    [[nodiscard]] virtual DocumentSettings settings() const noexcept = 0;

    /// Updates active document configuration and quality settings.
    virtual void setSettings(const DocumentSettings& settings) noexcept = 0;

    /// Resolves an internal or external destination link URI.
    [[nodiscard]] virtual ResolvedLink resolveLink(const std::string& uri, std::string* error = nullptr) const = 0;

    /// Extracts all hyper-links present on a given page index.
    [[nodiscard]] virtual std::vector<Link> extractLinks(int page, std::string* error = nullptr) const = 0;

    /// Extracts the table-of-contents outline tree.
    [[nodiscard]] virtual std::vector<OutlineNode> outline(std::string* error = nullptr) const = 0;

    /// Queries document metadata properties for requested metadata keys.
    [[nodiscard]] virtual DocumentMetadata metadata(const std::vector<std::string>& keys,
                                                    std::string* error = nullptr) const = 0;

    // -------------------------------------------------------------------------
    // PDF-Specific Features (Default "not supported" implementations)
    // -------------------------------------------------------------------------

    /// Adds a new annotation to a page (PDF only).
    [[nodiscard]] virtual bool
    addAnnotation(int page, const Annotation& annotation, std::int32_t* objectNumber, std::string* error = nullptr);

    /// Modifies an existing annotation on a page (PDF only).
    [[nodiscard]] virtual bool modifyAnnotation(int page,
                                                std::int32_t objectNumber,
                                                const Annotation& annotation,
                                                bool appearanceChanged,
                                                std::string* error = nullptr);

    /// Removes an annotation from a page (PDF only).
    [[nodiscard]] virtual bool removeAnnotation(int page, std::int32_t objectNumber, std::string* error = nullptr);

    /// Saves document modifications to an output file descriptor (PDF only).
    [[nodiscard]] virtual bool saveFd(int fd, std::string* error = nullptr);

    /// Exports page subsets to a PDF output file descriptor (PDF only).
    [[nodiscard]] virtual bool savePdfFd(int fd, const std::vector<int>& pages, std::string* error = nullptr);

    /// Digitally signs a signature field or rectangle on a page (PDF only).
    [[nodiscard]] virtual bool signFd(const Model::SignRequest& request,
                                      CmsCallback callback,
                                      int outputFd,
                                      SigningResult* signingResult = nullptr,
                                      std::string* error = nullptr);

    struct FieldMutation {
        int page = 0;
        std::int32_t objectNumber = 0;
        Model::FormValue actualValue;
    };

    /// Updates an interactive form field value (PDF only).
    [[nodiscard]] virtual bool updateFormField(int page,
                                               std::int32_t objectNumber,
                                               const Model::FormValue& value,
                                               std::vector<FieldMutation>* mutations,
                                               std::string* error = nullptr);

    /// Resets an interactive form through a ResetForm push button (PDF only).
    [[nodiscard]] virtual bool
    resetForm(int page, std::int32_t objectNumber, std::vector<FieldMutation>* mutations, std::string* error = nullptr);

    /// Lists embedded fonts used across specified pages (PDF only).
    [[nodiscard]] virtual std::vector<Font> fonts(const std::vector<int>& pages, std::string* error = nullptr) const;

    /// Extracts embedded file attachments (PDF only).
    [[nodiscard]] virtual std::vector<EmbeddedFile>
    embeddedFiles(std::size_t maxBytes, std::size_t maxFiles, bool* resourceLimit, std::string* error = nullptr) const;

protected:
    /// Helper to populate an error string pointer and return false.
    static bool fail(std::string* error, const char* message);
    static bool fail(std::string* error, std::string_view message);

    /// True when the last successful open was locked and given a non-empty
    /// password. Recorded by the runtime after a successful unlock sequence
    /// and reported through the "documentHasPassword" metadata key.
    bool m_passwordRequired = false;
};

} // namespace Mu::Worker::Engine

#endif
