// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_WORKER_PDF_GENERATED_OUTLINE_HPP
#define MU_WORKER_PDF_GENERATED_OUTLINE_HPP

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "shared/model/types.hpp"

namespace Mu::Worker::Engine {

inline constexpr std::size_t MaxGeneratedOutlineNodes = 5000;
inline constexpr std::size_t MaxGeneratedHeadingBytes = 160;

struct HeadingNumber {
    std::vector<unsigned> components;
    std::string title;
};

std::optional<HeadingNumber> parseHeading(std::string_view text);

/// Native page text in reading coordinates. Size/font describe the dominant
/// glyph style, so superscripts and inline emphasis cannot inflate a heading.
struct OutlineLine {
    std::string text;
    std::string fontFamily;
    double size = 0;
    double boldFraction = 0;
    bool monospaced = false;
    int page = 0;
    double left = 0;
    double top = 0;
    double right = 0;
    double bottom = 0;
    double pageWidth = 0;
    double pageHeight = 0;
    Model::Viewport viewport;
};

/// Reconstructs printed contents when corroborated by body headings; otherwise
/// infers headings from document typography and explicit numbering.
std::vector<Model::OutlineNode> buildGeneratedOutline(std::vector<OutlineLine> lines);

} // namespace Mu::Worker::Engine
#endif
