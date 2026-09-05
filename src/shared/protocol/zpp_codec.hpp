// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_PROTOCOL_ZPP_CODEC_HPP
#define MU_SHARED_PROTOCOL_ZPP_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "shared/model/types.hpp"
#include "shared/transport/ctrl_channel.hpp"

namespace Mu::IPC::ZppCodec {

/// Distinguishes serialization failures from control messages that exceed the IPC limit.
enum class EncodeError {
    None, ///< Encoding succeeded or no error has been reported yet.
    Serialization, ///< The message could not be serialized.
    ControlMessageLimit, ///< The encoded message exceeds the control-message limit.
};

/// Encodes one complete protocol message, including its wire envelope.
///
/// The returned bytes are the protocol payload passed to CtrlChannel, while
/// CtrlChannel supplies the outer length prefix. On failure, `error` receives
/// a diagnostic and `errorCode` identifies whether the control-message limit was hit.
std::optional<std::vector<std::byte>>
encode(const Model::RequestMessage& value, std::string* error = nullptr, EncodeError* errorCode = nullptr);
std::optional<std::vector<std::byte>>
encode(const Model::ResponseMessage& value, std::string* error = nullptr, EncodeError* errorCode = nullptr);
std::optional<std::vector<std::byte>>
encode(const Model::NotificationMessage& value, std::string* error = nullptr, EncodeError* errorCode = nullptr);

/// Decodes a complete protocol payload without modifying `value` on failure.
///
/// Decoding validates the wire envelope, rejects trailing bytes, applies the
/// message invariants, and bounds allocations before publishing the result.
bool decode(std::span<const std::byte> bytes, Model::RequestMessage* value, std::string* error = nullptr);
bool decode(std::span<const std::byte> bytes, Model::ResponseMessage* value, std::string* error = nullptr);
bool decode(std::span<const std::byte> bytes, Model::NotificationMessage* value, std::string* error = nullptr);

/// Convenience overload that decodes bytes owned by a vector.
inline bool decode(const std::vector<std::byte>& bytes, Model::RequestMessage* value, std::string* error = nullptr)
{
    return decode(std::span<const std::byte>(bytes), value, error);
}

inline bool decode(const std::vector<std::byte>& bytes, Model::ResponseMessage* value, std::string* error = nullptr)
{
    return decode(std::span<const std::byte>(bytes), value, error);
}

inline bool decode(const std::vector<std::byte>& bytes, Model::NotificationMessage* value, std::string* error = nullptr)
{
    return decode(std::span<const std::byte>(bytes), value, error);
}

/// Encodes and sends one framed message through the control channel.
///
/// `debugSide`, when non-empty, is used only as a logging label; message
/// contents are not logged by this codec.
bool writeMessage(CtrlChannel& channel,
                  const Model::RequestMessage& value,
                  int timeoutMs,
                  std::string* error = nullptr,
                  std::string_view debugSide = { });
bool writeMessage(CtrlChannel& channel,
                  const Model::ResponseMessage& value,
                  int timeoutMs,
                  std::string* error = nullptr,
                  std::string_view debugSide = { });
bool writeMessage(CtrlChannel& channel,
                  const Model::NotificationMessage& value,
                  int timeoutMs,
                  std::string* error = nullptr,
                  std::string_view debugSide = { });

/// Reads one framed message from the control channel and decodes its type.
///
/// The destination is unchanged if transport, decoding, validation, or
/// protocol-envelope checks fail.
bool readMessage(CtrlChannel& channel,
                 Model::RequestMessage* value,
                 int timeoutMs,
                 std::string* error = nullptr,
                 std::string_view debugSide = { });
bool readMessage(CtrlChannel& channel,
                 Model::ResponseMessage* value,
                 int timeoutMs,
                 std::string* error = nullptr,
                 std::string_view debugSide = { });
bool readMessage(CtrlChannel& channel,
                 Model::NotificationMessage* value,
                 int timeoutMs,
                 std::string* error = nullptr,
                 std::string_view debugSide = { });

} // namespace Mu::IPC::ZppCodec

#endif // MU_SHARED_PROTOCOL_ZPP_CODEC_HPP
