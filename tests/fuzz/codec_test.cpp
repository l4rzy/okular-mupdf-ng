// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shared/protocol/zpp_codec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main()
{
    using namespace Mu;
    const auto valid = IPC::ZppCodec::encode(Model::RequestMessage { 1, Model::PingRequest { } });
    if (!valid || valid->size() < 17)
        return 1;

    auto badMagic = *valid;
    badMagic[0] ^= std::byte { 1 };
    auto badVersion = *valid;
    badVersion[4] ^= std::byte { 1 };
    auto badKind = *valid;
    badKind[8] = std::byte { 2 };
    auto zeroId = *valid;
    for (std::size_t i = 9; i < 17; ++i)
        zeroId[i] = std::byte { 0 };
    auto trailing = *valid;
    trailing.push_back(std::byte { 0 });
    auto truncated = *valid;
    truncated.pop_back();

    struct Case {
        const char* name;
        std::vector<std::byte> input;
        bool accepted;
    };

    const std::array cases { Case { "valid", *valid, true },       Case { "empty", { }, false },
                             Case { "magic", badMagic, false },    Case { "version", badVersion, false },
                             Case { "kind", badKind, false },      Case { "zero id", zeroId, false },
                             Case { "trailing", trailing, false }, Case { "truncated", truncated, false } };
    for (const auto& test : cases) {
        Model::RequestMessage decoded;
        if (IPC::ZppCodec::decode(test.input, &decoded) != test.accepted) {
            std::fprintf(stderr, "unexpected decode result: %s\n", test.name);
            return 1;
        }
        LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(test.input.data()), test.input.size());
    }
}
