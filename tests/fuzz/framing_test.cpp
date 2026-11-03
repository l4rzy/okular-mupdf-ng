// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv)
{
    if (argc > 2)
        return 1;

    struct Case {
        const char* name;
        std::vector<std::uint8_t> input;
    };

    // First byte controls fragmentation; remaining bytes are the raw stream.
    std::vector cases { Case { "empty", { 0 } },
                        Case { "partial-header", { 0, 0, 0 } },
                        Case { "partial-payload", { 0, 0, 0, 0, 2, 42 } },
                        Case { "single", { 0, 0, 0, 0, 1, 42 } },
                        Case { "multiple", { 127, 0, 0, 0, 1, 42, 0, 0, 0, 2, 1, 2 } },
                        Case { "invalid-zero", { 0, 0, 0, 0, 0 } },
                        Case { "invalid-oversize", { 127, 4, 0, 0, 1 } },
                        Case { "complete-then-truncated", { 127, 0, 0, 0, 1, 42, 0, 0 } } };
    cases.push_back({ "queued-close", { 255, 0, 0, 0, 1, 42, 0, 0, 0, 1, 43 } });
    Case fragmented { "fragmented-sequence", { 255 } };
    for (unsigned frame = 0; frame < 3; ++frame) {
        fragmented.input.insert(fragmented.input.end(), { 0, 0, 16, 0 });
        for (unsigned i = 0; i < 4096; ++i)
            fragmented.input.push_back(static_cast<std::uint8_t>(i + frame));
    }
    fragmented.input.insert(fragmented.input.end(), { 0, 0, 0, 4, 42 });
    cases.push_back(fragmented);
    if (argc == 2)
        std::filesystem::create_directories(argv[1]);
    for (const auto& test : cases) {
        LLVMFuzzerTestOneInput(test.input.data(), test.input.size());
        if (argc == 2) {
            std::ofstream file(std::filesystem::path(argv[1]) / test.name, std::ios::binary);
            file.write(reinterpret_cast<const char*>(test.input.data()),
                       static_cast<std::streamsize>(test.input.size()));
            if (!file)
                return 1;
        }
    }
}
