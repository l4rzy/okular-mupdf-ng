// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shared/protocol/ipc_debug.hpp"

#ifdef MU_DEBUG_ENABLED

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "shared/model/types.hpp"

namespace Mu::IPC::Debug {

namespace Color {

inline constexpr std::string_view Reset = "\033[0m";
inline constexpr std::string_view BoldGreen = "\033[1;32m";
inline constexpr std::string_view BoldRed = "\033[1;31m";
inline constexpr std::string_view Cyan = "\033[36m";
inline constexpr std::string_view Blue = "\033[34m";
inline constexpr std::string_view BrightGreen = "\033[1;92m";
inline constexpr std::string_view DimGreen = "\033[32m";
inline constexpr std::string_view BrightYellow = "\033[1;93m";
inline constexpr std::string_view BrightCyan = "\033[1;96m";
inline constexpr std::string_view Red = "\033[31m";
inline constexpr std::string_view BrightBlue = "\033[1;94m";
inline constexpr std::string_view Yellow = "\033[33m";
inline constexpr std::string_view BrightMagenta = "\033[1;95m";
inline constexpr std::string_view Magenta = "\033[35m";
inline constexpr std::string_view DimMagenta = "\033[2;35m";
inline constexpr std::string_view BrightWhite = "\033[1;97m";

inline std::string_view forType(std::string_view type) noexcept
{
    if (type == "open")
        return BoldGreen;
    if (type == "close")
        return BoldRed;
    if (type == "ping")
        return Cyan;
    if (type == "settings" || type == "permissions")
        return Blue;
    if (type == "render")
        return BrightGreen;
    if (type == "release-frame-slot")
        return DimGreen;
    if (type == "form-update")
        return BrightYellow;
    if (type == "annotation-add")
        return BrightCyan;
    if (type == "annotation-modify")
        return Cyan;
    if (type == "annotation-remove")
        return Red;
    if (type == "sign" || type == "sign-reply" || type == "sign-input")
        return BrightBlue;
    if (type == "ocr-page" || type == "ocr-done")
        return BrightMagenta;
    if (type == "ocr-result")
        return Magenta;
    if (type == "cancel-ocr")
        return DimMagenta;
    if (type == "save" || type == "save-pdf")
        return BrightWhite;
    if (type == "error")
        return BoldRed;
    if (type == "ok")
        return DimGreen;
    return Yellow;
}

inline std::string tag(std::string_view type, bool colorize = false)
{
    if (!colorize)
        return std::string("[") + std::string(type) + "]";
    const auto color = forType(type);
    return std::string("[") + std::string(color) + std::string(type) + std::string(Reset) + "]";
}

} // namespace Color

namespace Detail {

template <typename T> void field(std::ostringstream& out, std::string_view name, const T& value)
{
    out << ' ' << name << '=' << value;
}

inline void field(std::ostringstream& out, std::string_view name, std::string_view value)
{
    out << ' ' << name << '=' << std::quoted(std::string(value));
}

inline void field(std::ostringstream& out, std::string_view name, const std::string& value)
{
    field(out, name, std::string_view(value));
}

inline void bytes(std::ostringstream& out, std::string_view name, const std::vector<std::uint8_t>& value)
{
    if (!name.empty())
        out << ' ' << name << '=';
    out << "0x" << std::hex << std::setfill('0');
    for (const auto byte : value)
        out << std::setw(2) << static_cast<unsigned>(byte);
    out << std::dec << std::setfill(' ');
}

inline void timestamp(std::ostringstream& out, std::string_view name, const Model::Timestamp& value)
{
    out << ' ' << name << '=' << (value.valid ? std::to_string(value.unixMilliseconds) : "invalid");
}

inline void rect(std::ostringstream& out, std::string_view name, double left, double top, double right, double bottom)
{
    out << ' ' << name << "=[" << left << ',' << top << ',' << right << ',' << bottom << ']';
}

inline void value(std::ostringstream& out, const Model::Value& input, std::size_t depth = 0)
{
    if (depth > 16) {
        out << "<depth-limit>";
        return;
    }
    std::visit(
        [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                out << "null";
            else if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> || std::is_same_v<T, double>)
                out << item;
            else if constexpr (std::is_same_v<T, std::string>)
                out << std::quoted(item);
            else if constexpr (std::is_same_v<T, Model::Value::Bytes>) {
                out << "0x" << std::hex << std::setfill('0');
                for (const auto byte : item)
                    out << std::setw(2) << static_cast<unsigned>(byte);
                out << std::dec << std::setfill(' ');
            } else if constexpr (std::is_same_v<T, Model::Value::Array>) {
                out << '[';
                for (std::size_t i = 0; i < item.size(); ++i) {
                    if (i)
                        out << ',';
                    Detail::value(out, item[i], depth + 1);
                }
                out << ']';
            } else {
                out << '{';
                bool first = true;
                for (const auto& [key, child] : item) {
                    if (!first)
                        out << ',';
                    first = false;
                    out << std::quoted(key) << ':';
                    Detail::value(out, child, depth + 1);
                }
                out << '}';
            }
        },
        input.data);
}

inline void annotation(std::ostringstream& out, const Model::Annotation& input)
{
    out << "{subtype=" << input.subtype;
    field(out, "uuid", input.uuid);
    rect(out, "rect", input.x0, input.y0, input.x1, input.y1);
    field(out, "contents", input.contents);
    field(out, "author", input.author);
    timestamp(out, "created", input.creationDate);
    timestamp(out, "modified", input.modificationDate);
    field(out, "color", input.color);
    field(out, "flags", input.flags);
    field(out, "native", input.nativeIndex);
    field(out, "object", input.pdfObjectNumber);
    field(out, "handle", input.handle);
    out << " extras={points=" << input.extras.points.size() << " quads=" << input.extras.quads.size()
        << " inkPaths=" << input.extras.inkPaths.size() << " callout=" << input.extras.callout.size();
    if (input.extras.style.appearance)
        field(out, "icon", input.extras.style.appearance->icon);
    if (input.extras.style.borderWidth)
        field(out, "borderWidth", *input.extras.style.borderWidth);
    out << " extension=";
    Detail::value(out, Model::Value { input.extras.extension });
    out << "}}";
}

inline void signature(std::ostringstream& out, const Model::SignatureField& value)
{
    out << "{name=" << std::quoted(value.partialName) << " signer=" << std::quoted(value.signerName)
        << " status=" << static_cast<int>(value.signatureStatus)
        << " certStatus=" << static_cast<int>(value.certificateStatus);
    rect(out, "rect", value.left, value.top, value.right, value.bottom);
    field(out, "byteRange", value.byteRange.size());
    bytes(out, "cms", value.cmsSignature);
    out << " signed=" << value.signedField << "} ";
}

inline void link(std::ostringstream& out, const Model::Link& value)
{
    rect(out, "rect", value.left, value.top, value.right, value.bottom);
    out << " target={external=" << value.target.external;
    field(out, "uri", value.target.uri);
    field(out, "page", value.target.viewport.page);
    field(out, "x", value.target.viewport.normalizedX);
    field(out, "y", value.target.viewport.normalizedY);
    field(out, "coordinateMask", value.target.viewport.coordinateMask);
    field(out, "valid", value.target.valid);
    out << '}';
}

inline void formField(std::ostringstream& out, const Model::FormField& value)
{
    out << "{handle=" << std::quoted(value.handle) << " type=" << static_cast<int>(value.type)
        << " name=" << std::quoted(value.fullyQualifiedName);
    rect(out, "rect", value.rectangle.left, value.rectangle.top, value.rectangle.right, value.rectangle.bottom);
    out << " readOnly=" << value.readOnly << " textLen=" << value.text.size()
        << " value=\"[redacted]\" checked=" << value.checked << "} ";
}

void page(std::ostringstream& out, const Model::PageInfo& value)
{
    out << "{number=" << value.number << " geometry=[" << value.geometry.widthPoints << ','
        << value.geometry.heightPoints << ',' << value.geometry.duration << ']'
        << " annotations=" << value.annotations.size() << " signatures=" << value.signatureFields.size()
        << " links=" << value.links.size() << " formFields=" << value.formFields.size();
    for (const auto& annotationValue : value.annotations) {
        out << " annotation=";
        Detail::annotation(out, annotationValue);
    }
    for (const auto& signatureValue : value.signatureFields) {
        out << " signature=";
        Detail::signature(out, signatureValue);
    }
    for (const auto& linkValue : value.links) {
        out << " link=";
        link(out, linkValue);
    }
    for (const auto& fieldValue : value.formFields) {
        out << " formField=";
        formField(out, fieldValue);
    }
    out << '}';
}

inline void outline(std::ostringstream& out, const Model::OutlineNode& value)
{
    out << "{title=" << std::quoted(value.title) << " open=" << value.open << " children=" << value.children.size()
        << " link=";
    if (value.link.valid)
        link(out, Model::Link { 0, 0, 0, 0, value.link });
    else
        out << "invalid";
    for (const auto& child : value.children) {
        out << " child=";
        outline(out, child);
    }
    out << '}';
}

inline void requestPayload(std::ostringstream& out, const Model::RequestPayload& payload)
{
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Model::PingRequest>) {
                out << "ping";
                field(out, "compat", value.compat);
            } else if constexpr (std::is_same_v<T, Model::OpenRequest>) {
                out << "open";
                field(out, "transfer", value.file.transferId);
                field(out, "displayName", value.displayName);
                field(out, "password", std::string("[redacted]"));
            } else if constexpr (std::is_same_v<T, Model::CloseRequest>)
                out << "close";
            else if constexpr (std::is_same_v<T, Model::RenderRequest>) {
                out << "render";
                field(out, "page", value.page);
                field(out, "width", value.width);
                field(out, "height", value.height);
                if (value.tile)
                    field(out,
                          "tile",
                          std::string("[") + std::to_string(value.tile->x) + "," + std::to_string(value.tile->y) + ","
                              + std::to_string(value.tile->width) + "," + std::to_string(value.tile->height) + "]");
            } else if constexpr (std::is_same_v<T, Model::ReleaseFrameSlotRequest>) {
                out << "release-frame-slot";
                field(out, "slot", value.slotId);
                field(out, "lease", value.leaseId);
            } else if constexpr (std::is_same_v<T, Model::TextBoxesRequest>) {
                out << "text-boxes";
                field(out, "page", value.page);
                field(out, "dpiX", value.dpiX);
                field(out, "dpiY", value.dpiY);
                field(out, "skipAnnots", value.skipAnnots);
            } else if constexpr (std::is_same_v<T, Model::OcrPageRequest>) {
                out << "ocr-page";
                field(out, "transfer", value.file.transferId);
                field(out, "page", value.page);
                field(out, "dpi", value.dpi);
                field(out, "language", value.language);
                field(out, "async", value.asynchronous);
            } else if constexpr (std::is_same_v<T, Model::OcrResultRequest>) {
                out << "ocr-result";
                field(out, "job", value.jobId);
            } else if constexpr (std::is_same_v<T, Model::CancelOcrJobsRequest>) {
                out << "cancel-ocr";
            } else if constexpr (std::is_same_v<T, Model::MetadataRequest>) {
                out << "metadata";
                field(out, "keys", value.keys.size());
                for (const auto& key : value.keys)
                    field(out, "key", key);
            } else if constexpr (std::is_same_v<T, Model::SynopsisRequest>)
                out << "synopsis";
            else if constexpr (std::is_same_v<T, Model::FontsRequest>) {
                out << "fonts";
                field(out, "page", value.page);
            } else if constexpr (std::is_same_v<T, Model::EmbeddedFilesRequest>)
                out << "embedded-files";
            else if constexpr (std::is_same_v<T, Model::AnnotationAddRequest>) {
                out << "annotation-add";
                field(out, "page", value.page);
                out << " annotation=";
                annotation(out, value.annotation);
            } else if constexpr (std::is_same_v<T, Model::AnnotationModifyRequest>) {
                out << "annotation-modify";
                field(out, "page", value.mutation.page);
                field(out, "handle", value.mutation.handle.value);
                field(out, "appearanceChanged", value.mutation.appearanceChanged);
                out << " annotation=";
                annotation(out, value.mutation.annotation);
            } else if constexpr (std::is_same_v<T, Model::AnnotationRemoveRequest>) {
                out << "annotation-remove";
                field(out, "page", value.page);
                field(out, "handle", value.handle.value);
            } else if constexpr (std::is_same_v<T, Model::SettingsRequest>) {
                out << "settings";
                field(out, "graphicsAA", value.settings.graphicsAntialiasing);
                field(out, "textAA", value.settings.textAntialiasing);
                field(out, "imageQuality", value.settings.imageQuality);
                field(out, "interpolate", value.settings.interpolateImages);
                field(out, "epubFontSize", value.settings.epub.fontSize);
                field(out, "epubPageSize", static_cast<std::uint8_t>(value.settings.epub.pageSize));
                field(out, "epubFontFamily", static_cast<std::uint8_t>(value.settings.epub.fontFamily));
                field(out, "epubCustomCssBase64Bytes", value.settings.epub.customCssBase64.size());
            } else if constexpr (std::is_same_v<T, Model::SaveRequest>) {
                out << "save";
                field(out, "transfer", value.file.transferId);
            } else if constexpr (std::is_same_v<T, Model::SavePdfRequest>) {
                out << "save-pdf";
                field(out, "transfer", value.file.transferId);
                field(out, "pages", value.pages.size());
            } else if constexpr (std::is_same_v<T, Model::SignRequest>) {
                out << "sign";
                field(out, "transfer", value.file.transferId);
                field(out, "page", value.page);
                field(out, "existingField", value.existingFieldObjectNumber);
                rect(out,
                     "rect",
                     value.rectangle.left,
                     value.rectangle.top,
                     value.rectangle.right,
                     value.rectangle.bottom);
                field(out, "certificate", value.certificateNickname);
                field(out, "reason", value.reason);
                field(out, "location", value.location);
                field(out, "hasBackground", !value.backgroundImage.empty());
            } else if constexpr (std::is_same_v<T, Model::SignReply>) {
                out << "sign-reply";
                field(out, "job", value.jobId);
                field(out, "nonce", value.nonce);
                field(out, "result", static_cast<std::int32_t>(value.result));
            } else if constexpr (std::is_same_v<T, Model::FormUpdateRequest>) {
                out << "form-update";
                field(out, "handle", value.handle);
                std::visit(
                    [&](const auto& v) {
                        using VT = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<VT, Model::FormTextValue>) {
                            field(out, "type", "text");
                            field(out, "len", v.text.size());
                            field(out, "value", "[redacted]");
                        } else if constexpr (std::is_same_v<VT, Model::FormCheckValue>) {
                            field(out, "type", "check");
                            field(out, "checked", v.checked);
                        } else if constexpr (std::is_same_v<VT, Model::FormChoiceSelection>) {
                            field(out, "type", "choice-selection");
                            field(out, "count", v.selectedIndices.size());
                        } else if constexpr (std::is_same_v<VT, Model::FormChoiceCustomText>) {
                            field(out, "type", "choice-custom-text");
                            field(out, "len", v.text.size());
                            field(out, "value", "[redacted]");
                        }
                    },
                    value.value);
            } else if constexpr (std::is_same_v<T, Model::FormResetRequest>) {
                out << "form-reset";
                field(out, "handle", value.handle);
            }
        },
        payload);
}

inline std::string_view responseName(const Model::ResponsePayload& payload)
{
    return std::visit(
        [](const auto& value) -> std::string_view {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::monostate>)
                return "ok";
            else if constexpr (std::is_same_v<T, Model::OpenResponse>)
                return "open";
            else if constexpr (std::is_same_v<T, Model::RenderResponse>)
                return "render";
            else if constexpr (std::is_same_v<T, Model::JobResponse>)
                return "job";
            else if constexpr (std::is_same_v<T, Model::TextBoxesResponse>)
                return "text-boxes";
            else if constexpr (std::is_same_v<T, Model::OcrResponse>)
                return "ocr";
            else if constexpr (std::is_same_v<T, Model::MetadataResponse>)
                return "metadata";
            else if constexpr (std::is_same_v<T, Model::OutlineResponse>)
                return "outline";
            else if constexpr (std::is_same_v<T, Model::FontsResponse>)
                return "fonts";
            else if constexpr (std::is_same_v<T, Model::EmbeddedFilesResponse>)
                return "embedded-files";
            else if constexpr (std::is_same_v<T, Model::AnnotationResponse>)
                return "annotation";
            else if constexpr (std::is_same_v<T, Model::PingResponse>)
                return "ping";
            else if constexpr (std::is_same_v<T, Model::FormUpdateResponse>)
                return "form-update";
            else
                return "sign";
        },
        payload);
}

} // namespace Detail

std::string request(const Model::RequestMessage& message, bool colorize)
{
    std::ostringstream payload;
    Detail::requestPayload(payload, message.payload);
    const auto serialized = payload.str();
    const auto separator = serialized.find(' ');
    const auto typeName = serialized.substr(0, separator);

    std::ostringstream out;
    out << Color::tag(typeName, colorize) << " id=" << message.id;
    if (separator != std::string::npos)
        out << serialized.substr(separator);
    return out.str();
}

std::string response(const Model::ResponseMessage& message, bool colorize)
{
    std::ostringstream out;
    const std::string_view typeName = message.error ? "error" : Detail::responseName(message.payload);
    out << Color::tag(typeName, colorize) << " id=" << message.id;
    if (message.error) {
        out << " code=" << static_cast<int>(message.error->code);
        Detail::field(out, "operation", message.error->operation);
        Detail::field(out, "message", message.error->message);
    } else {
        std::visit(
            [&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    out << " success";
                else if constexpr (std::is_same_v<T, Model::OpenResponse>)
                    out << " pages=" << value.pages.size();
                else if constexpr (std::is_same_v<T, Model::RenderResponse>) {
                    out << " frame={transfer=" << value.frame.transferId << " slot=" << value.frame.slotId
                        << " lease=" << value.frame.leaseId << " width=" << value.frame.width
                        << " height=" << value.frame.height << " stride=" << value.frame.stride
                        << " format=" << value.frame.format << '}';
                } else if constexpr (std::is_same_v<T, Model::JobResponse>)
                    out << " job=" << value.jobId;
                else if constexpr (std::is_same_v<T, Model::TextBoxesResponse>)
                    out << " count=" << value.boxes.size();
                else if constexpr (std::is_same_v<T, Model::OcrResponse>) {
                    out << " status=" << static_cast<int>(value.result.status)
                        << " boxes=" << value.result.boxes.size();
                    for (const auto& box : value.result.boxes)
                        out << " {text=" << std::quoted(box.text) << " rect=[" << box.left << ',' << box.top << ','
                            << box.right << ',' << box.bottom << "] end=" << box.endOfLine << '}';
                } else if constexpr (std::is_same_v<T, Model::MetadataResponse>) {
                    out << " pageCount=" << value.metadata.pageCount
                        << " mime=" << std::quoted(value.metadata.mimeType);
                    for (const auto& [key, item] : value.metadata.values)
                        out << ' ' << std::quoted(key) << '=' << std::quoted(item);
                } else if constexpr (std::is_same_v<T, Model::OutlineResponse>)
                    out << " count=" << value.nodes.size();
                else if constexpr (std::is_same_v<T, Model::FontsResponse>) {
                    out << " count=" << value.fonts.size();
                    for (const auto& font : value.fonts)
                        out << " {name=" << std::quoted(font.name) << " file=" << std::quoted(font.file)
                            << " type=" << static_cast<std::int32_t>(font.type)
                            << " embed=" << static_cast<std::int32_t>(font.embedType) << '}';
                } else if constexpr (std::is_same_v<T, Model::EmbeddedFilesResponse>) {
                    out << " count=" << value.files.size();
                    for (const auto& file : value.files) {
                        out << " {name=" << std::quoted(file.name) << " description=" << std::quoted(file.description)
                            << " size=" << file.size << " tooLarge=" << file.contentTooLarge << " data=";
                        Detail::bytes(out, "", file.data);
                        out << '}';
                    }
                } else if constexpr (std::is_same_v<T, Model::AnnotationResponse>)
                    out << " handle=" << std::quoted(value.handle.value);
                else if constexpr (std::is_same_v<T, Model::PingResponse>) {
                    out << " compat=" << std::quoted(value.compat) << " pid=" << value.pid
                        << " sandbox={landlock=" << value.sandbox.landlock;
                    if (value.sandbox.landlockAbi > 0)
                        out << " abi=" << value.sandbox.landlockAbi;
                    out << " seccomp=" << value.sandbox.seccomp << " ns=" << value.sandbox.linuxNamespace
                        << " rlimit=" << value.sandbox.resourceLimits << " memProt=" << value.sandbox.memoryProtection;
                    if (!value.sandbox.reason.empty())
                        out << " reason=" << std::quoted(value.sandbox.reason);
                    out << '}';
                } else if constexpr (std::is_same_v<T, Model::SignResponse>)
                    out << " result=" << static_cast<std::int32_t>(value.result)
                        << " details=" << std::quoted(value.details);
                else if constexpr (std::is_same_v<T, Model::FormUpdateResponse>) {
                    out << " fields=" << value.affectedFields.size() << " pages=" << value.affectedPages.size();
                }
            },
            message.payload);
    }
    return out.str();
}

std::string notification(const Model::NotificationMessage& message, bool colorize)
{
    std::ostringstream out;
    std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Model::OcrDoneNotification>)
                out << Color::tag("ocr-done", colorize) << " job=" << value.jobId << " page=" << value.page;
            else if constexpr (std::is_same_v<T, Model::PageLinksNotification>)
                out << Color::tag("page-links", colorize) << " generation=" << value.generation
                    << " pages=" << value.pages.size();
            else if constexpr (std::is_same_v<T, Model::SignInput>) {
                out << Color::tag("sign-input", colorize) << " job=" << value.jobId;
                Detail::field(out, "nonce", value.nonce);
                Detail::field(out, "certificate", value.certificateNickname);
            }
        },
        message.payload);
    return out.str();
}

} // namespace Mu::IPC::Debug

#endif // MU_DEBUG_ENABLED
