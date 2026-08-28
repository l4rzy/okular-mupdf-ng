// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/protocol/zpp_codec.hpp"

#include <system_error>
#include <type_traits>
#include <utility>

#include <zpp/zpp_bits.h>

#include "shared/logging.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/transport/common.hpp"

namespace Mu::IPC::ZppCodec {

namespace {

// Every protocol payload starts with a fixed envelope so peers can reject the
// wrong protocol version or message family before allocating the message body.
inline constexpr std::uint32_t WireMagic = 0x4d554343;
inline constexpr std::uint32_t WireVersion = static_cast<std::uint32_t>(::Mu::IPC::PROTOCOL_VERSION);
inline constexpr std::uint8_t RequestKind = 1;
inline constexpr std::uint8_t ResponseKind = 2;
inline constexpr std::uint8_t NotificationKind = 3;

// Keep the mapping in one place so encode and decode cannot silently assign
// different wire kinds to the same message type.
template <typename T> constexpr std::uint8_t kindOf()
{
    if constexpr (std::is_same_v<T, Model::RequestMessage>)
        return RequestKind;
    else if constexpr (std::is_same_v<T, Model::ResponseMessage>)
        return ResponseKind;
    else if constexpr (std::is_same_v<T, Model::NotificationMessage>)
        return NotificationKind;
    else
        static_assert(std::is_same_v<T, void>, "unsupported IPC message type");
}

std::string errorMessage(std::errc error)
{
    // zpp reports failures as std::errc-compatible values; use the standard
    // library's localized text instead of exposing numeric codec errors.
    return std::make_error_code(error).message();
}

bool setError(std::string* error, std::string message)
{
    // Error output is optional because most callers only need the success bit.
    if (error)
        *error = std::move(message);
    return false;
}

template <typename T> bool validateDecoded(const T& value, std::string* error)
{
    // Serialization validates representation and bounds; these checks enforce
    // invariants that are meaningful only after the typed message is decoded.
    if constexpr (std::is_same_v<T, Model::RequestMessage>) {
        if (value.id == 0)
            return setError(error, "request id is zero");
    } else if constexpr (std::is_same_v<T, Model::ResponseMessage>) {
        if (value.id == 0)
            return setError(error, "response id is zero");
        if (value.error && !std::holds_alternative<std::monostate>(value.payload))
            return setError(error, "error response contains a payload");
    } else if constexpr (std::is_same_v<T, Model::NotificationMessage>) {
        if (const auto* pageLinks = std::get_if<Model::PageLinksNotification>(&value.payload)) {
            for (const auto& page : pageLinks->pages) {
                if (page.page < 0)
                    return setError(error, "page-link notification contains a negative page");
            }
        }
    }
    return true;
}

template <typename T>
std::optional<std::vector<std::byte>> encodeImpl(const T& value, std::string* error, EncodeError* errorCode)
{
    if (errorCode)
        *errorCode = EncodeError::None;

    // zpp writes the envelope and message into one contiguous buffer using the
    // protocol's fixed little-endian representation.
    // The allocation limit applies while zpp grows the output; the final size
    // check below keeps the wire-size contract explicit at this boundary.
    std::vector<std::byte> data;
    auto output = zpp::bits::out(
        data, zpp::bits::size4b { }, zpp::bits::endian::little { }, zpp::bits::alloc_limit<Limit::MaxFrameBytes> { });
    const auto result = output(WireMagic, WireVersion, kindOf<T>(), value);
    if (zpp::bits::failure(result)) {
        // zpp uses allocation-related errors for both capacity and message-size
        // failures; normalize both to the public FrameLimit category.
        const auto code = static_cast<std::errc>(result);
        const bool frameLimit = code == std::errc::no_buffer_space || code == std::errc::message_size;
        if (errorCode)
            *errorCode = frameLimit ? EncodeError::FrameLimit : EncodeError::Serialization;
        return setError(error, frameLimit ? "message exceeds frame limit" : errorMessage(code)), std::nullopt;
    }

    data.resize(output.position());
    // The allocator limit protects growth, while this check protects the final
    // serialized size if the codec's accounting changes in the future.
    if (data.size() > Limit::MaxFrameBytes) {
        if (errorCode)
            *errorCode = EncodeError::FrameLimit;
        setError(error, "message exceeds frame limit");
        return std::nullopt;
    }
    return data;
}

template <typename T> bool decodeImpl(std::span<const std::byte> bytes, T* value, std::string* error)
{
    // Reject invalid destinations and impossible frame sizes before invoking
    // the codec, which also avoids unbounded work on attacker-controlled input.
    if (!value || bytes.empty() || bytes.size() > Limit::MaxFrameBytes)
        return setError(error, "message is empty or too large");

    // Keep decoded data private until the whole frame, including its trailing
    // byte check, has succeeded so callers never observe a partial message.
    auto input = zpp::bits::in(bytes,
                               zpp::bits::size4b { },
                               zpp::bits::endian::little { },
                               zpp::bits::alloc_limit<Limit::MaxDecodedAllocationBytes> { },
                               zpp::bits::nesting_limit<Limit::MaxDepth> { });
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint8_t kind = 0;
    auto result = input(magic, version, kind);
    if (zpp::bits::failure(result))
        return setError(error, errorMessage(static_cast<std::errc>(result)));
    // Check the envelope before decoding the variant-heavy message body so a
    // frame for another protocol or message family is rejected early.
    if (magic != WireMagic || version != WireVersion)
        return setError(error, "unsupported protocol header");
    if (kind != kindOf<T>())
        return setError(error, "message kind mismatch");

    T decoded { };
    result = input(decoded);
    if (zpp::bits::failure(result))
        return setError(error, errorMessage(static_cast<std::errc>(result)));
    // A valid frame must be consumed exactly. Extra bytes could otherwise hide
    // a second message or indicate that peers disagree about the schema.
    if (input.position() != bytes.size())
        return setError(error, "trailing bytes");
    if (!validateDecoded(decoded, error))
        return false;
    *value = std::move(decoded);
    return true;
}

template <typename T>
bool writeMessageImpl(
    CtrlChannel& channel, const T& value, int timeoutMs, std::string* error, std::string_view debugSide)
{
    // Keep serialization separate from transport so framed I/O has one path
    // for all three protocol message families.
    const auto data = encodeImpl(value, error, nullptr);
    if (!data)
        return false;
    // Keep diagnostics limited to the frame size; protocol payloads may carry
    // document data or other sensitive values.
    if (!debugSide.empty())
        MU_LOG(debug, debugSide, std::string("write frame bytes=") + std::to_string(data->size()));
    return writeFrame(channel, *data, timeoutMs, error);
}

template <typename T>
bool readMessageImpl(CtrlChannel& channel, T* value, int timeoutMs, std::string* error, std::string_view debugSide)
{
    // Framing remains the transport layer's responsibility; this function
    // only connects the received frame to the typed codec path.
    std::vector<std::byte> frame;
    if (!readFrame(channel, &frame, timeoutMs, error))
        return false;
    // The optional side label records transport activity without dumping the
    // decoded message, which may contain sensitive document information.
    if (!debugSide.empty())
        MU_LOG(debug, debugSide, std::string("read frame bytes=") + std::to_string(frame.size()));
    return decodeImpl(frame, value, error);
}

} // namespace

std::optional<std::vector<std::byte>> encode(const Model::RequestMessage& value, std::string* error, EncodeError* code)
{
    return encodeImpl(value, error, code);
}

std::optional<std::vector<std::byte>> encode(const Model::ResponseMessage& value, std::string* error, EncodeError* code)
{
    return encodeImpl(value, error, code);
}

std::optional<std::vector<std::byte>>
encode(const Model::NotificationMessage& value, std::string* error, EncodeError* code)
{
    return encodeImpl(value, error, code);
}

bool decode(std::span<const std::byte> bytes, Model::RequestMessage* value, std::string* error)
{
    return decodeImpl(bytes, value, error);
}

bool decode(std::span<const std::byte> bytes, Model::ResponseMessage* value, std::string* error)
{
    return decodeImpl(bytes, value, error);
}

bool decode(std::span<const std::byte> bytes, Model::NotificationMessage* value, std::string* error)
{
    return decodeImpl(bytes, value, error);
}

bool writeMessage(CtrlChannel& channel,
                  const Model::RequestMessage& value,
                  int timeoutMs,
                  std::string* error,
                  std::string_view debugSide)
{
    return writeMessageImpl(channel, value, timeoutMs, error, debugSide);
}

bool writeMessage(CtrlChannel& channel,
                  const Model::ResponseMessage& value,
                  int timeoutMs,
                  std::string* error,
                  std::string_view debugSide)
{
    return writeMessageImpl(channel, value, timeoutMs, error, debugSide);
}

bool writeMessage(CtrlChannel& channel,
                  const Model::NotificationMessage& value,
                  int timeoutMs,
                  std::string* error,
                  std::string_view debugSide)
{
    return writeMessageImpl(channel, value, timeoutMs, error, debugSide);
}

bool readMessage(
    CtrlChannel& channel, Model::RequestMessage* value, int timeoutMs, std::string* error, std::string_view debugSide)
{
    return readMessageImpl(channel, value, timeoutMs, error, debugSide);
}

bool readMessage(
    CtrlChannel& channel, Model::ResponseMessage* value, int timeoutMs, std::string* error, std::string_view debugSide)
{
    return readMessageImpl(channel, value, timeoutMs, error, debugSide);
}

bool readMessage(CtrlChannel& channel,
                 Model::NotificationMessage* value,
                 int timeoutMs,
                 std::string* error,
                 std::string_view debugSide)
{
    return readMessageImpl(channel, value, timeoutMs, error, debugSide);
}

} // namespace Mu::IPC::ZppCodec
