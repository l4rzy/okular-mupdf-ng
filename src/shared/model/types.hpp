// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_MODEL_TYPES_HPP
#define MU_SHARED_MODEL_TYPES_HPP

/// Qt-free values owned by the worker. Qt adapters belong in the plugin.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "shared/protocol/limits.hpp"

namespace Mu::Model {

/// Extensible scalar and container value used by annotation metadata.
struct Value {
    /// Recursive array, object, and byte containers used by the value variant.
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Bytes = std::vector<std::uint8_t>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Bytes, Array, Object>;

    /// The stored value; `std::monostate` represents null.
    Storage data;

    /// Returns whether this value contains the null alternative.
    [[nodiscard]] bool isNull() const { return std::holds_alternative<std::monostate>(data); }
};

/// Optional UTC timestamp represented as milliseconds since the Unix epoch.
struct Timestamp {
    /// Explicit validity avoids interpreting a missing PDF date in local time.
    bool valid = false;
    /// Meaningful only when `valid` is true.
    std::int64_t unixMilliseconds = 0;
};

/// Text and device-pixel bounds returned by page text extraction.
struct TextBox {
    /// UTF-8 text contained in the box.
    std::string text;
    /// Device-pixel bounds at the DPI requested by the caller.
    double left = 0, top = 0, right = 0, bottom = 0;
    /// Marks the final box generated from a source text line.
    bool endOfLine = false;
};

/// Outcome of an OCR request.
enum class OcrStatus : std::uint8_t { Success, Cancelled, Failed, Unavailable };

/// OCR output status and positioned text boxes.
struct OcrResult {
    OcrStatus status = OcrStatus::Failed;
    std::vector<TextBox> boxes;
};

/// Stable categories for errors returned across the worker boundary.
enum class ErrorCode : std::uint16_t {
    InvalidRequest,
    NotOpen,
    PermissionDenied,
    NotFound,
    ResourceLimit,
    Cancelled,
    Unavailable,
    Internal,
};

/// Result of opening a document.
enum class OpenStatus : std::uint8_t { Success, NeedsPassword, Failed };

/// Structured operation failure reported by the worker.
struct Error {
    /// Broad machine-readable failure category.
    ErrorCode code = ErrorCode::Internal;
    /// Operation that produced the failure.
    std::string operation;
    /// Human-readable diagnostic suitable for logs or UI display.
    std::string message;
};

/// Pixel format used by shared-memory render frames.
inline constexpr std::uint32_t PixelFormatRgba8888 = 1;

/// A point in normalized page or annotation coordinates, depending on context.
struct Point {
    double x = 0, y = 0;
};

/// Four-corner quadrilateral stored in clockwise page order.
struct Quad {
    Point upperLeft, upperRight, lowerRight, lowerLeft;
};

/// Optional visual properties for text and stamp annotations.
struct AnnotationAppearance {
    std::string icon;
    std::string fontName;
    double fontSize = 0;
    std::uint32_t textColor = 0;
    double borderWidth = 0;
    std::int32_t alignment = 0;
};

/// PDF line-ending styles used by line and callout annotations.
enum class AnnotationLineEnding : std::int32_t {
    None = 0,
    Square = 1,
    Circle = 2,
    Diamond = 3,
    OpenArrow = 4,
    ClosedArrow = 5,
    Butt = 6,
    ROpenArrow = 7,
    RClosedArrow = 8,
    Slash = 9,
};

/// Optional annotation style overrides supplied by the caller.
struct AnnotationStyle {
    std::optional<AnnotationAppearance> appearance;
    std::optional<double> borderWidth;
    std::optional<std::uint32_t> interiorColor;
    std::optional<std::int32_t> intent;
    std::optional<AnnotationLineEnding> firstLineEnding;
    std::optional<AnnotationLineEnding> lastLineEnding;
    std::optional<bool> closed;
};

/// Geometry and extension data specific to an annotation subtype.
struct AnnotationExtras {
    std::vector<Point> points;
    std::vector<Quad> quads;
    std::vector<std::vector<Point>> inkPaths;
    std::vector<Point> callout;
    AnnotationStyle style;
    bool caretSymbolP = false;
    Value::Object extension;
};

/// Annotation data exchanged between the plugin and worker.
struct Annotation {
    std::int32_t subtype = 0;
    std::string uuid;
    /// Normalized page bounds in the inclusive [0, 1] range.
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string contents, author;
    Timestamp creationDate, modificationDate;
    /// ARGB color, including annotation opacity in the high byte.
    std::uint32_t color = 0xffffffffU;
    std::int32_t flags = 0;
    AnnotationExtras extras;
    /// Page-local index used while extracting native annotations.
    std::int32_t nativeIndex = -1;
    /// Stable identity inside the open PDF xref, independent of page caches.
    std::int32_t pdfObjectNumber = -1;
    /// Session-local handle used by later annotation mutations.
    std::string handle;
};

/// Optional destination position within a page.
struct Viewport {
    /// Bit indicating that normalizedX is present in the destination.
    static constexpr std::uint8_t CoordinateX = 1U << 0;
    /// Bit indicating that normalizedY is present in the destination.
    static constexpr std::uint8_t CoordinateY = 1U << 1;

    /// Destination page index, or -1 when the destination is unresolved.
    std::int32_t page = -1;
    /// Optional normalized horizontal destination coordinate.
    double normalizedX = 0;
    /// Optional normalized vertical destination coordinate.
    double normalizedY = 0;
    /// Combination of CoordinateX and CoordinateY describing present coordinates.
    std::uint8_t coordinateMask = 0;
};

/// Link target that is either external or a document viewport.
struct ResolvedLink {
    /// True when uri should be opened outside the current document.
    bool external = false;
    /// External URI or the original unresolved link target.
    std::string uri;
    /// Internal destination, when one was resolved.
    Viewport viewport;
    /// Whether the target was resolved successfully.
    bool valid = false;
};

/// A normalized clickable page rectangle and its resolved destination.
struct Link {
    double left = 0, top = 0, right = 0, bottom = 0;
    ResolvedLink target;
};

/// Font technology reported by the document engine.
enum class FontType : std::int32_t {
    Unknown = 0,
    Type1 = 1,
    Type1C = 2,
    Type1COT = 3,
    Type3 = 4,
    TrueType = 5,
    TrueTypeOT = 6,
    CIDType0 = 7,
    CIDType0C = 8,
    CIDType0COT = 9,
    CIDTrueType = 10,
    CIDTrueTypeOT = 11,
};

/// Embedding state of a document font.
enum class FontEmbedType : std::int32_t {
    NotEmbedded = 0,
    EmbeddedSubset = 1,
    FullyEmbedded = 2,
};

/// Font metadata reported for a document or page.
struct Font {
    std::string name;
    std::string file;
    FontType type = FontType::Unknown;
    FontEmbedType embedType = FontEmbedType::NotEmbedded;
};

/// Embedded document attachment and bounded-content status.
struct EmbeddedFile {
    std::string name;
    std::string description;
    /// Declared or discovered byte size, or -1 when unknown.
    std::int64_t size = -1;
    Timestamp creationDate;
    Timestamp modificationDate;
    std::vector<std::uint8_t> data;
    /// True when the attachment was intentionally not fully buffered.
    bool contentTooLarge = false;
};

/// Physical page dimensions and optional presentation metadata.
struct PageGeometry {
    /// Page width in PDF points.
    double widthPoints = 0;
    /// Page height in PDF points.
    double heightPoints = 0;
    /// Presentation duration in seconds, or -1 when not specified.
    double duration = -1;
    /// Format-provided page label, if available.
    std::string label;
};

/// Supported EPUB layout paper sizes.
enum class EpubPageSize : std::uint8_t { B5, A5, SixByNine, Letter };

/// Supported EPUB layout font families.
enum class EpubFontFamily : std::uint8_t { Default, Serif, SansSerif, Monospace };

using Limit::MaxEpubCustomCssBase64Bytes;
using Limit::MaxEpubCustomCssCharacters;

/// User-controlled EPUB reflow and styling settings.
struct EpubLayoutSettings {
    /// Base font size in points.
    std::int32_t fontSize = 11;
    /// Target paper size used by EPUB pagination.
    EpubPageSize pageSize = EpubPageSize::B5;
    /// Default family used when EPUB content does not select one.
    EpubFontFamily fontFamily = EpubFontFamily::Default;
    /// Base64-encoded UTF-8 CSS supplied by the caller.
    std::string customCssBase64;
};

/// Rendering and layout settings shared by document engines.
struct DocumentSettings {
    /// MuPDF graphics antialiasing level.
    std::int32_t graphicsAntialiasing = 8;
    /// MuPDF text antialiasing level.
    std::int32_t textAntialiasing = 8;
    /// Image quality setting selected by the caller.
    std::int32_t imageQuality = 0;
    /// Whether images should be interpolated while rendering.
    bool interpolateImages = true;
    /// Maximum document-engine cache size in bytes.
    std::int64_t memoryCacheBytes = 64ULL * 1024ULL * 1024ULL;
    /// Opaque page background as 0xRRGGBB (Okular paper-color setting).
    std::uint32_t paperColorRgb = 0xFFFFFF;
    EpubLayoutSettings epub;
};

/// Document metadata values and basic document identity.
struct DocumentMetadata {
    std::map<std::string, std::string, std::less<>> values;
    std::int32_t pageCount = 0;
    std::string mimeType;
};

/// Hierarchical outline entry with an optional destination and children.
struct OutlineNode {
    std::string title;
    bool open = false;
    ResolvedLink link;
    std::vector<OutlineNode> children;
};

/// Certificate data and trust-independent cryptographic metadata.
struct Certificate {
    /// True when no certificate was available.
    bool null = true;
    std::int32_t version = 0;
    std::vector<std::uint8_t> serialNumber;
    std::string issuerCommonName;
    std::string issuerDistinguishedName;
    std::string issuerEmail;
    std::string issuerOrganization;
    std::string subjectCommonName;
    std::string subjectDistinguishedName;
    std::string subjectEmail;
    std::string subjectOrganization;
    std::string nickname;
    Timestamp validityStart;
    Timestamp validityEnd;
    std::uint32_t keyUsage = 0;
    std::vector<std::uint8_t> publicKey;
    std::int32_t publicKeyType = 0;
    std::int32_t publicKeyStrength = 0;
    bool selfSigned = false;
    std::vector<std::uint8_t> der;
};

/// Result of validating a digital signature's cryptographic contents.
enum class SignatureStatus : std::int32_t {
    Unknown = 0,
    Valid = 1,
    Invalid = 2,
    DigestMismatch = 3,
    DecodingError = 4,
    GenericError = 5,
    NotFound = 6,
    NotVerified = 7,
};

/// Trust state of the certificate associated with a signature.
enum class CertificateStatus : std::int32_t {
    Unknown = 0,
    Trusted = 1,
    UntrustedIssuer = 2,
    UnknownIssuer = 3,
    Revoked = 4,
    Expired = 5,
    GenericError = 6,
    NotVerified = 7,
    VerificationInProgress = 8,
};

/// Digest algorithm identified in a signature's CMS payload.
enum class HashAlgorithm : std::int32_t {
    Unknown = 0,
    Md2 = 1,
    Md5 = 2,
    Sha1 = 3,
    Sha256 = 4,
    Sha384 = 5,
    Sha512 = 6,
    Sha224 = 7,
};

/// Outcome of a signing operation.
enum class SigningResult : std::int32_t {
    Success = 0,
    GenericError = 1,
    FieldAlreadySigned = 2,
    KeyMissing = 3,
    WriteFailed = 4,
    UserCancelled = 5,
    BadPassphrase = 6,
};

/// Signature widget metadata and validation results for one page.
struct SignatureField {
    /// Page containing the signature widget.
    std::int32_t page = -1;
    /// PDF object number of the widget annotation.
    std::int32_t objectNumber = -1;
    std::string partialName;
    std::string fullyQualifiedName;
    bool readOnly = false;
    bool visible = true;
    std::string signerName;
    std::string signerSubjectDn;
    std::string reason;
    std::string location;
    Timestamp signingTime;
    std::string subFilter;
    double left = 0, top = 0, right = 0, bottom = 0;
    SignatureStatus signatureStatus = SignatureStatus::Unknown;
    CertificateStatus certificateStatus = CertificateStatus::Unknown;
    CertificateStatus certificateStatusAtSigningTime = CertificateStatus::Unknown;
    CertificateStatus certificateStatusCurrent = CertificateStatus::Unknown;
    /// Time at which the current certificate status was evaluated.
    Timestamp certificateValidationTime;
    bool hasTrustedTimestamp = false;
    HashAlgorithm hashAlgorithm = HashAlgorithm::Unknown;
    Certificate certificate;
    /// Four values describing the signed byte ranges in the source file.
    std::vector<std::int64_t> byteRange;
    /// DER-encoded CMS signature contents, when available.
    std::vector<std::uint8_t> cmsSignature;
    /// Whether the signature's byte ranges cover the complete document.
    bool signsTotalDocument = true;
    /// Whether the field contains a signature value rather than an empty widget.
    bool signedField = false;
};

/// Rectangle whose coordinates are normalized to the inclusive [0, 1] range.
struct NormalizedRect {
    double left = 0, top = 0, right = 0, bottom = 0;
};

/// Widget type represented by a form field.
enum class FormFieldType : std::uint8_t {
    Text,
    CheckBox,
    RadioButton,
    ComboBox,
    ListBox,
    PushButton,
};

/// Action performed by a push button form field.
enum class FormPushButtonAction : std::uint8_t {
    None,
    Reset,
};

/// Form widget metadata and its current value state.
struct FormField {
    /// Stable handle used by later form update/reset requests.
    std::string handle;
    /// Page containing the widget.
    int page = -1;
    /// PDF object number of the widget annotation.
    std::int32_t pdfObjectNumber = -1;
    /// PDF object number of the logical field object.
    std::int32_t fieldObjectNumber = -1;
    FormFieldType type = FormFieldType::Text;

    std::string partialName;
    std::string uiName;
    std::string fullyQualifiedName;
    std::string groupName;
    NormalizedRect rectangle;

    bool readOnly = false;
    bool visible = true;
    bool printable = false;

    /// Text field attributes.
    std::string text;
    int maximumLength = 0;
    bool multiline = false;
    bool password = false;

    /// Button (CheckBox / RadioButton) attributes.
    bool checked = false;
    std::string onState;
    bool noToggleToOff = false;

    /// Push button attributes.
    std::string buttonCaption;
    FormPushButtonAction pushButtonAction = FormPushButtonAction::None;

    /// Choice (ComboBox / ListBox) attributes.
    std::vector<std::string> choices;
    std::vector<std::string> exportValues;
    std::vector<int> currentChoices;
    bool editableCombo = false;
    bool multiSelect = false;
};

/// Text assigned to a text or editable combo-box field.
struct FormTextValue {
    std::string text;
};

/// Checked state assigned to a checkbox or radio-button field.
struct FormCheckValue {
    bool checked = false;
};

/// Selected zero-based choice indices assigned to a list or combo field.
struct FormChoiceSelection {
    std::vector<int> selectedIndices;
};

/// Custom text assigned to an editable combo-box field.
struct FormChoiceCustomText {
    std::string text;
};

/// Type-safe value accepted by a form update request.
using FormValue = std::variant<FormTextValue, FormCheckValue, FormChoiceSelection, FormChoiceCustomText>;

/// Request to replace the value of a form field identified by handle.
struct FormUpdateRequest {
    std::string handle;
    FormValue value;
};

/// Request to reset a form field identified by handle.
struct FormResetRequest {
    std::string handle;
};

/// Value of a form field returned after an update.
struct FormFieldState {
    std::string handle;
    FormValue value;
};

/// Fields and pages affected by a form update.
struct FormUpdateResponse {
    std::vector<FormFieldState> affectedFields;
    std::vector<int> affectedPages;
};

/// All model data collected for one document page.
struct PageInfo {
    std::int32_t number = 0;
    PageGeometry geometry;
    std::vector<Annotation> annotations;
    std::vector<SignatureField> signatureFields;
    std::vector<Link> links;
    std::vector<FormField> formFields;
};

/// Notification payload containing links for one page.
struct PageLinks {
    std::int32_t page = -1;
    std::vector<Link> links;
};

/// Description of a rendered shared-memory frame.
struct RenderFrame {
    /// Identifier used to match a newly-created or transient frame with its
    /// FD-channel transfer. It is zero when an existing pooled slot is reused.
    std::uint64_t transferId = 0;
    /// Non-zero worker-owned reusable frame slot, or zero for a transient frame.
    std::uint64_t slotId = 0;
    /// Lease generation required to release a pooled slot.
    std::uint64_t leaseId = 0;
    /// Actual pixel dimensions and row stride in bytes. Dimensions can be
    /// lower than the request when the shared-memory frame budget is reached.
    std::int32_t width = 0, height = 0, stride = 0;
    /// PixelFormatRgba8888 for the current frame protocol.
    std::uint32_t format = PixelFormatRgba8888;
};

/// Status of the worker's process-isolation controls.
struct SandboxStatus {
    bool landlock = false;
    std::int32_t landlockAbi = 0;
    bool seccomp = false;
    bool linuxNamespace = false;
    bool resourceLimits = false;
    bool memoryProtection = false;
    std::string reason;

    /// Returns true if all primary isolation controls are active.
    [[nodiscard]] constexpr bool isFullyHardened() const noexcept
    {
        return landlock && seccomp && linuxNamespace && resourceLimits;
    }

    /// Returns true if some isolation mechanisms are active but full hardening is unavailable.
    [[nodiscard]] constexpr bool isPartiallyActive() const noexcept
    {
        return !isFullyHardened() && (landlock || seccomp || linuxNamespace);
    }
};

/// Opaque handle for a previously extracted annotation.
struct AnnotationHandle {
    std::string value;
};

/// Annotation mutation request with its target page and identity.
struct AnnotationMutation {
    std::int32_t page = -1;
    AnnotationHandle handle;
    Annotation annotation;
    bool appearanceChanged = true;
};

/// Identifier for a file transferred over the descriptor side channel.
struct FileTransfer {
    std::uint64_t transferId = 0;
};

/// Input supplied to the plugin for one CMS signing round trip.
struct SignInput {
    /// Identifier matching the eventual signing response.
    std::uint64_t jobId = 0;
    /// Nonce echoed by the signer to bind the response to this request.
    std::string nonce;
    /// NSS certificate nickname selected for signing.
    std::string certificateNickname;
    /// SHA-256 digest of the byte range that must be signed.
    std::array<std::uint8_t, 32> digest { };
};

/// Result returned by the plugin after one CMS signing round trip.
struct SignReply {
    /// Identifier and nonce copied from the corresponding SignInput.
    std::uint64_t jobId = 0;
    std::string nonce;
    SigningResult result = SigningResult::GenericError;
    /// Human-readable failure details or signing diagnostics.
    std::string details;
    /// DER-encoded CMS signature when signing succeeds.
    std::vector<std::uint8_t> cmsSignature;
};

/// Document formats supported by the worker.
enum class DocumentType : std::uint8_t { Pdf = 0, Epub = 1, Unknown = 255 };

/// Converts a document type to its canonical MIME type.
inline std::string documentTypeToMime(DocumentType type)
{
    switch (type) {
    case DocumentType::Epub:
        return "application/epub+zip";
    case DocumentType::Pdf:
        return "application/pdf";
    default:
        return "application/octet-stream";
    }
}

/// Converts a PDF/EPUB MIME type to a document type.
inline DocumentType documentTypeFromMime(const std::string& mime)
{
    if (mime == "application/epub+zip")
        return DocumentType::Epub;
    if (mime == "application/pdf" || mime == "application/x-pdf")
        return DocumentType::Pdf;
    return DocumentType::Unknown;
}

/// Liveness and compatibility request sent when opening the worker session.
struct PingRequest {
    std::string compat;
};

/// Opens a transferred document and optionally supplies EPUB acceleration data.
struct OpenRequest {
    /// Input document descriptor received over the FD channel.
    FileTransfer file;
    /// Display name used for format detection and diagnostics.
    std::string displayName;
    /// Password supplied when opening an encrypted document.
    std::string password;
    /// Explicit document format selected by the caller.
    DocumentType documentType = DocumentType::Pdf;
    /// Optional cached EPUB layout accelerator.
    std::vector<std::uint8_t> epubAccelerator;
};

/// Maximum accepted serialized EPUB accelerator size.
inline constexpr std::size_t MaxEpubAcceleratorBytes = 512U * 1024U;

/// Closes the active document.
struct CloseRequest { };

/// Pixel rectangle requested for a partial page render.
struct RenderTile {
    std::int32_t x = 0, y = 0, width = 0, height = 0;
};

/// Page render request, optionally restricted to a tile. A response may use a
/// lower raster resolution when the requested RGBA frame exceeds its limit.
struct RenderRequest {
    std::int32_t page = -1, width = 0, height = 0;
    std::optional<RenderTile> tile;
};

/// Releases a pooled render slot after the final QImage copy is destroyed.
struct ReleaseFrameSlotRequest {
    std::uint64_t slotId = 0;
    std::uint64_t leaseId = 0;
};

/// Requests text extraction at the specified device resolution.
struct TextBoxesRequest {
    std::int32_t page = -1;
    double dpiX = 0, dpiY = 0;
    bool skipAnnots = false;
};

/// Requests OCR for a transferred document page.
struct OcrPageRequest {
    FileTransfer file;
    std::int32_t page = -1, dpi = 225;
    std::string language;
    bool asynchronous = false;
};

/// Requests the result of a previously submitted OCR job.
struct OcrResultRequest {
    std::uint64_t jobId = 0;
};

/// Cancels all pending OCR jobs.
struct CancelOcrJobsRequest { };

/// Requests selected metadata keys from the open document.
struct MetadataRequest {
    std::vector<std::string> keys;
};

/// Requests the document outline.
struct SynopsisRequest { };

/// Requests font metadata for a page.
struct FontsRequest {
    std::int32_t page = -1;
};

/// Requests all embedded files in the open document.
struct EmbeddedFilesRequest { };

/// Adds an annotation to a page.
struct AnnotationAddRequest {
    std::int32_t page = -1;
    Annotation annotation;
};

/// Modifies an existing annotation.
struct AnnotationModifyRequest {
    AnnotationMutation mutation;
};

/// Removes an annotation identified by handle.
struct AnnotationRemoveRequest {
    std::int32_t page = -1;
    AnnotationHandle handle;
};

/// Updates the active document rendering and layout settings.
struct SettingsRequest {
    DocumentSettings settings;
};

/// Saves the complete document to a transferred output descriptor.
struct SaveRequest {
    FileTransfer file;
};

/// Saves selected PDF pages to a transferred output descriptor.
struct SavePdfRequest {
    FileTransfer file;
    std::vector<std::int32_t> pages;
};

/// Creates or updates a PDF signature widget and requests CMS signing.
struct SignRequest {
    FileTransfer file;
    std::int32_t page = -1;
    NormalizedRect rectangle;
    std::string certificateNickname;
    std::string certificateSubjectCommonName;
    std::string reason;
    std::string location;
    /// Existing widget object number, or a negative value to create a widget.
    std::int32_t existingFieldObjectNumber = -1;
    std::vector<std::uint8_t> backgroundImage;
};

/// All request body alternatives supported by the worker protocol.
using RequestPayload = std::variant<PingRequest,
                                    OpenRequest,
                                    CloseRequest,
                                    RenderRequest,
                                    ReleaseFrameSlotRequest,
                                    TextBoxesRequest,
                                    OcrPageRequest,
                                    OcrResultRequest,
                                    CancelOcrJobsRequest,
                                    MetadataRequest,
                                    SynopsisRequest,
                                    FontsRequest,
                                    EmbeddedFilesRequest,
                                    AnnotationAddRequest,
                                    AnnotationModifyRequest,
                                    AnnotationRemoveRequest,
                                    SettingsRequest,
                                    SaveRequest,
                                    SavePdfRequest,
                                    SignRequest,
                                    SignReply,
                                    FormUpdateRequest,
                                    FormResetRequest>;

/// Correlated request envelope sent over the control channel.
struct RequestMessage {
    /// Non-zero identifier echoed by the matching response.
    std::uint64_t id = 0;
    /// One of the request payload alternatives listed in RequestPayload.
    RequestPayload payload;
};

/// Result of opening a document, including page data and EPUB acceleration.
struct OpenResponse {
    std::vector<PageInfo> pages;
    std::uint64_t linkGeneration = 0;
    std::vector<std::uint8_t> epubAccelerator;
};

/// Incremental page-link update associated with a document link generation.
struct PageLinksNotification {
    /// Generation used to discard updates from a previous document state.
    std::uint64_t generation = 0;
    std::vector<PageLinks> pages;
    /// True when one or more links could not be returned due to resource limits.
    bool resourceLimited = false;
    std::string error;
};

/// Render response carrying the shared-memory frame descriptor.
struct RenderResponse {
    RenderFrame frame;
};

/// Identifier of an asynchronous worker job.
struct JobResponse {
    std::uint64_t jobId = 0;
};

/// Text extraction response for one page.
struct TextBoxesResponse {
    std::vector<TextBox> boxes;
};

/// OCR response containing status and any recognized text boxes.
struct OcrResponse {
    OcrResult result;
};

/// Document metadata response.
struct MetadataResponse {
    DocumentMetadata metadata;
};

/// Document outline response.
struct OutlineResponse {
    std::vector<OutlineNode> nodes;
};

/// Font metadata response.
struct FontsResponse {
    std::vector<Font> fonts;
};

/// Embedded-file metadata and content response.
struct EmbeddedFilesResponse {
    std::vector<EmbeddedFile> files;
};

/// Handle assigned to a newly created annotation.
struct AnnotationResponse {
    AnnotationHandle handle;
};

/// Compatibility and sandbox status returned by a ping.
struct PingResponse {
    std::string compat;
    std::int64_t pid = 0;
    SandboxStatus sandbox;
};

/// Signing status returned after the worker processes a sign request.
struct SignResponse {
    SigningResult result = SigningResult::Success;
    std::string details;
};

/// All successful response body alternatives supported by the worker protocol.
using ResponsePayload = std::variant<std::monostate,
                                     OpenResponse,
                                     RenderResponse,
                                     JobResponse,
                                     TextBoxesResponse,
                                     OcrResponse,
                                     MetadataResponse,
                                     OutlineResponse,
                                     FontsResponse,
                                     EmbeddedFilesResponse,
                                     AnnotationResponse,
                                     PingResponse,
                                     SignResponse,
                                     FormUpdateResponse>;

struct ResponseMessage {
    /// Identifier matching the request that produced this response.
    std::uint64_t id = 0;
    /// Successful response payload; monostate is used for empty responses.
    ResponsePayload payload;
    /// Present when the request failed instead of producing a normal payload.
    std::optional<Error> error;
};

/// Notification that an asynchronous OCR job has finished.
struct OcrDoneNotification {
    std::uint64_t jobId = 0;
    std::int32_t page = -1;
};

/// Uncorrelated event body alternatives sent outside request/response matching.
using NotificationPayload = std::variant<OcrDoneNotification, PageLinksNotification, SignInput>;

/// Uncorrelated event sent by the worker or plugin.
struct NotificationMessage {
    NotificationPayload payload;
};

} // namespace Mu::Model

#endif // MU_SHARED_MODEL_TYPES_HPP
