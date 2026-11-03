// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/protocol/zpp_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#ifdef IPC_FUZZ_SEEDS
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#endif

namespace {

template <typename T> void checkDecode(std::span<const std::byte> bytes, T destination)
{
    const auto before = Mu::IPC::ZppCodec::encode(destination);
    if (!before)
        std::abort();
    if (!Mu::IPC::ZppCodec::decode(bytes, &destination)) {
        if (Mu::IPC::ZppCodec::encode(destination) != before)
            std::abort();
        return;
    }
    const auto encoded = Mu::IPC::ZppCodec::encode(destination);
    T roundTrip { };
    if (!encoded || !Mu::IPC::ZppCodec::decode(*encoded, &roundTrip))
        std::abort();
    if (Mu::IPC::ZppCodec::encode(roundTrip) != encoded)
        std::abort();
}

} // namespace

#ifndef IPC_FUZZ_SEEDS
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const auto bytes = std::as_bytes(std::span(data, size));
    checkDecode(bytes,
                Mu::Model::RequestMessage {
                    42,
                    Mu::Model::OpenRequest {
                        { 7 }, "document.epub", "password", Mu::Model::DocumentType::Epub, { 1, 2, 3 } } });
    checkDecode(bytes,
                Mu::Model::ResponseMessage {
                    42, std::monostate { }, Mu::Model::Error { Mu::Model::ErrorCode::Internal, "open", "failure" } });
    checkDecode(bytes, Mu::Model::NotificationMessage { Mu::Model::ExportDoneNotification { 42, false, "failure" } });
    return 0;
}
#else
namespace {

template <typename Message> bool writeSeed(const std::filesystem::path& path, const Message& message)
{
    const auto encoded = Mu::IPC::ZppCodec::encode(message);
    Message decoded { };
    if (!encoded || !Mu::IPC::ZppCodec::decode(*encoded, &decoded))
        return false;
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(encoded->data()), static_cast<std::streamsize>(encoded->size()));
    return file.good();
}

template <typename Message, typename Variant, std::size_t... I>
bool writeSeeds(const std::filesystem::path& directory, const char* prefix, std::index_sequence<I...>)
{
    bool success = true;
    auto write = [&](std::size_t index, const Message& message) {
        success = writeSeed(directory / (std::string(prefix) + std::to_string(index)), message) && success;
    };
    if constexpr (std::is_same_v<Message, Mu::Model::NotificationMessage>)
        (write(I, Message { Variant { std::in_place_index<I> } }), ...);
    else
        (write(I, Message { 1, Variant { std::in_place_index<I> } }), ...);
    return success;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
        return 1;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    using namespace Mu::Model;
    const OpenRequest open { { 42 }, "book.epub", "password", DocumentType::Epub, { 1, 2, 3, 4 } };
    const RenderRequest render { 2, 800, 600, RenderTile { 10, 20, 200, 300 }, 9, true, 5 };
    OpenResponse opened;
    opened.pages.resize(2);
    opened.pages[0].number = 1;
    opened.epubAccelerator = { 1, 2, 3, 4 };
    const PageLinksNotification links { 3, { PageLinks { 0, { } }, PageLinks { 2, { } } }, true, "partial" };
    return !(writeSeeds<RequestMessage, RequestPayload>(
                 directory, "request-", std::make_index_sequence<std::variant_size_v<RequestPayload>> { })
             && writeSeeds<ResponseMessage, ResponsePayload>(
                 directory, "response-", std::make_index_sequence<std::variant_size_v<ResponsePayload>> { })
             && writeSeeds<NotificationMessage, NotificationPayload>(
                 directory, "notification-", std::make_index_sequence<std::variant_size_v<NotificationPayload>> { })
             && writeSeed(directory / "request-open-populated", RequestMessage { 42, open })
             && writeSeed(directory / "request-render-tile", RequestMessage { 43, render })
             && writeSeed(directory / "response-open-populated", ResponseMessage { 42, opened, std::nullopt })
             && writeSeed(directory / "response-error",
                          ResponseMessage { 43, std::monostate { }, Error { ErrorCode::Internal, "open", "failure" } })
             && writeSeed(directory / "notification-page-links", NotificationMessage { links })
             && writeSeed(directory / "notification-sign-input",
                          NotificationMessage { SignInput { 42, "nonce", "certificate", { 1, 2, 3 } } }));
}
#endif
