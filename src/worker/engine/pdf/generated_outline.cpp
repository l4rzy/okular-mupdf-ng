// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/pdf/generated_outline.hpp"
#include "engine/pdf/destination.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Mu::Worker::Engine {

namespace {

bool hasTitleLetters(std::string_view text)
{
    // Preserve the existing UTF-8 policy so non-English titles remain usable.
    return std::any_of(text.begin(), text.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch >= 128;
    });
}

} // namespace

std::optional<HeadingNumber> parseHeading(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    if (text.empty() || text.size() > MaxGeneratedHeadingBytes)
        return std::nullopt;
    HeadingNumber result;
    result.title = text;
    while (!text.empty()) {
        unsigned number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc() || number > 9999 || result.components.size() >= 8)
            return std::nullopt;
        result.components.push_back(number);
        text.remove_prefix(static_cast<std::size_t>(end - text.data()));
        if (text.empty())
            return std::nullopt;
        if (text.front() != '.')
            break;
        text.remove_prefix(1);
        if (text.empty())
            return std::nullopt;
        if (text.front() < '0' || text.front() > '9')
            break;
    }
    if (text.empty() || (text.front() != ' ' && text.front() != '\t'))
        return std::nullopt;
    return hasTitleLetters(text) ? std::optional<HeadingNumber>(std::move(result)) : std::nullopt;
}

namespace {

std::string trimText(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return { };
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

std::string normalizeTitle(std::string_view text)
{
    std::string result;
    for (std::size_t i = 0; i < text.size();) {
        // Trademark/superscript markers and Unicode dash/quote punctuation do
        // not distinguish a contents entry from its body heading.
        if (text.substr(i, 2) == "®") {
            i += 2;
            continue;
        }
        if (text.substr(i, 3) == "™" || text.substr(i, 3) == "–" || text.substr(i, 3) == "—" || text.substr(i, 3) == "’"
            || text.substr(i, 3) == "‘") {
            i += 3;
            continue;
        }
        const auto ch = static_cast<unsigned char>(text[i++]);
        if (ch >= 'A' && ch <= 'Z')
            result.push_back(static_cast<char>(ch + ('a' - 'A')));
        else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch >= 128)
            result.push_back(static_cast<char>(ch));
    }
    return result;
}

std::optional<int> parsePageNumber(std::string_view text)
{
    const auto trimmed = trimText(text);
    int number = 0;
    const auto [end, error] = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), number);
    if (error != std::errc() || end != trimmed.data() + trimmed.size() || number <= 0 || number > 100'000)
        return std::nullopt;
    return number;
}

std::string stripEdgeNumbers(std::string_view text)
{
    std::string value = trimText(text);
    while (!value.empty() && ((value.front() >= '0' && value.front() <= '9') || value.front() == ' '))
        value.erase(value.begin());
    while (!value.empty() && ((value.back() >= '0' && value.back() <= '9') || value.back() == ' '))
        value.pop_back();
    return normalizeTitle(value);
}

std::string headingKey(std::string_view text)
{
    if (parseHeading(text)) {
        const auto separator = text.find_first_of(" \t");
        return normalizeTitle(text.substr(separator));
    }
    return normalizeTitle(text);
}

bool isSectionTitle(std::string_view text)
{
    const auto value = trimText(text);
    const auto word = normalizeTitle(std::string_view(value).substr(0, value.find_first_of(" \t:")));
    return word == "section" || word == "part";
}

bool isContentsLabel(std::string_view text)
{
    const auto trimmed = trimText(text);
    std::string_view label = trimmed;
    const auto isPageMarker = [](std::string_view word) {
        return parsePageNumber(word).has_value()
            || (!word.empty() && word.size() <= 8
                && word.find_first_not_of("ivxlcdmIVXLCDM") == std::string_view::npos);
    };
    const auto first = label.find_first_of(" \t");
    if (first != std::string_view::npos && isPageMarker(label.substr(0, first)))
        label.remove_prefix(first + 1);
    const auto last = label.find_last_of(" \t");
    if (last != std::string_view::npos && isPageMarker(label.substr(last + 1)))
        label = label.substr(0, last);
    const auto key = normalizeTitle(label);
    return key == "contents" || key == "tableofcontents" || key == "tableofcontentscontinued";
}

bool isCaption(std::string_view text)
{
    const auto value = trimText(text);
    const auto separator = value.find_first_of(" \t:");
    const auto word = normalizeTitle(std::string_view(value).substr(0, separator));
    return word == "figure" || word == "fig" || word == "table" || word == "note" || word == "tip" || word == "warning";
}

bool isMarginLine(const OutlineLine& line)
{
    return line.top < line.pageHeight * 0.09 || line.bottom > line.pageHeight * 0.93;
}

bool hasMatchingStyle(const OutlineLine& a, const OutlineLine& b)
{
    return a.fontFamily == b.fontFamily && std::abs(a.size - b.size) <= std::max(a.size, b.size) * 0.05
        && std::abs(a.boldFraction - b.boldFraction) < 0.3;
}

bool canJoinLines(const OutlineLine& a, const OutlineLine& b)
{
    if (a.page != b.page || !hasMatchingStyle(a, b) || b.top < a.bottom - a.size * 0.5
        || b.top - a.bottom > a.size * 0.8)
        return false;
    // Left, right, and centered display titles can all wrap across blocks.
    return std::abs(a.left - b.left) < 5 || std::abs(a.right - b.right) < a.size * 0.35
        || std::abs((a.left + a.right) - (b.left + b.right)) < 10;
}

void appendLine(OutlineLine& target, const OutlineLine& next)
{
    target.text += " " + trimText(next.text);
    target.left = std::min(target.left, next.left);
    target.right = std::max(target.right, next.right);
    target.bottom = next.bottom;
}

struct ContentsEntry {
    OutlineLine line;
    std::optional<int> printedPage;
    int level = 0;
};

std::optional<int> takeTrailingPage(std::string& title)
{
    const auto separator = title.find_last_of(" \t");
    if (separator == std::string::npos)
        return std::nullopt;
    auto page = parsePageNumber(std::string_view(title).substr(separator + 1));
    if (page)
        title = trimText(std::string_view(title).substr(0, separator));
    return page;
}

std::vector<ContentsEntry> readContents(const std::vector<OutlineLine>& lines, const std::set<int>& contentsPages)
{
    std::vector<ContentsEntry> result;
    std::unordered_set<const OutlineLine*> consumed;
    // A deterministic work bound also covers pathological overlapping layouts.
    // The outer line/node bounds alone do not bound these nested searches.
    std::size_t comparisons = 0;
    const auto checkWork = [&] {
        if (++comparisons > 2'000'000)
            throw std::length_error("generated outline contents work limit exceeded");
    };
    std::map<int, std::vector<const OutlineLine*>> pageNumbers;
    std::map<int, int> rightColumnTitles;
    for (const auto& line : lines) {
        if (!contentsPages.contains(line.page))
            continue;
        if (parsePageNumber(line.text))
            pageNumbers[line.page].push_back(&line);
        else if (line.left > line.pageWidth * 0.5 && line.text.size() > 8 && !isMarginLine(line))
            ++rightColumnTitles[line.page];
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (consumed.contains(&lines[i]) || !contentsPages.contains(lines[i].page) || isContentsLabel(lines[i].text)
            || isMarginLine(lines[i]) || parsePageNumber(lines[i].text))
            continue;
        OutlineLine title = lines[i];
        title.text = trimText(title.text);
        auto page = takeTrailingPage(title.text);
        // Page numbers often occupy a separate text block. Prefer the closest
        // number to the right on the same baseline (within the same column).
        const bool twoColumns = rightColumnTitles[title.page] >= 3;
        const auto findPage = [&](const OutlineLine& end) -> std::optional<int> {
            const OutlineLine* closest = nullptr;
            for (const auto* number : pageNumbers[title.page]) {
                checkWork();
                const auto& other = *number;
                if ((twoColumns && end.left < end.pageWidth * 0.5 && other.left > end.pageWidth * 0.5)
                    || other.left < end.right - 2 || other.left - end.right > end.pageWidth * 0.4
                    || std::abs((other.top + other.bottom) - (end.top + end.bottom)) > end.size * 1.5)
                    continue;
                if (!closest || other.left < closest->left)
                    closest = &other;
            }
            return closest ? parsePageNumber(closest->text) : std::nullopt;
        };
        if (!page)
            page = findPage(title);
        // Walk only this column. Number blocks and the adjacent column must
        // not interrupt a wrapped title, or be absorbed into it.
        OutlineLine end = title;
        while (!page && title.text.size() < MaxGeneratedHeadingBytes) {
            const OutlineLine* next = nullptr;
            for (std::size_t j = i + 1; j < lines.size() && lines[j].page == title.page; ++j) {
                checkWork();
                const auto& candidate = lines[j];
                if (candidate.top - end.bottom > end.size * 0.8 || candidate.top - end.top > end.size * 1.4)
                    break;
                if (candidate.top <= end.top || std::abs(candidate.left - end.left) >= 5
                    || parsePageNumber(candidate.text))
                    continue;
                next = &candidate;
                break;
            }
            if (!next || !canJoinLines(end, *next) || next->top - end.top > end.size * 1.4)
                break;
            consumed.insert(next);
            std::string continuation = trimText(next->text);
            page = takeTrailingPage(continuation);
            title.text += " " + continuation;
            end = *next;
            if (!page)
                page = findPage(end);
        }
        if (title.text.size() <= MaxGeneratedHeadingBytes && !headingKey(title.text).empty())
            result.push_back({ std::move(title), page, 0 });
        if (result.size() > MaxGeneratedOutlineNodes)
            throw std::length_error("generated outline node limit exceeded");
    }
    return result;
}

std::vector<double> collectSizes(std::vector<double> sizes)
{
    std::sort(sizes.begin(), sizes.end(), std::greater<>());
    std::vector<double> distinct;
    for (double size : sizes) {
        if (distinct.empty() || distinct.back() - size > distinct.back() * 0.05)
            distinct.push_back(size);
    }
    return distinct;
}

int rankSize(double size, const std::vector<double>& sizes)
{
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (size >= sizes[i] * 0.95)
            return static_cast<int>(i);
    }
    return static_cast<int>(sizes.size());
}

struct Candidate {
    OutlineLine line;
    bool chapter = false;
    int level = 0;
};

std::vector<Candidate> findCandidates(const std::vector<OutlineLine>& lines, const std::set<int>& contentsPages)
{
    // Estimate body style across the document, excluding contents and code.
    // One code-heavy page must not reclassify its numbered instructions.
    std::map<std::pair<std::string, int>, std::size_t> styles;
    std::unordered_map<std::string, std::set<int>> margins;
    for (const auto& line : lines) {
        if (isMarginLine(line))
            margins[stripEdgeNumbers(line.text)].insert(line.page);
        if (!contentsPages.contains(line.page) && !line.monospaced && line.boldFraction < 0.5 && line.text.size() >= 25
            && !isMarginLine(line))
            styles[{ line.fontFamily, static_cast<int>(std::round(line.size * 2)) }] += line.text.size();
    }
    double bodySize = 0;
    std::string bodyFamily;
    std::size_t mostCharacters = 0;
    for (const auto& [style, count] : styles) {
        if (count > mostCharacters) {
            mostCharacters = count;
            bodyFamily = style.first;
            bodySize = static_cast<double>(style.second) / 2;
        }
    }
    // All-bold/short documents have no prose sample. Use their most frequent
    // style as a conservative baseline rather than accepting every line.
    if (bodySize == 0) {
        for (const auto& line : lines) {
            if (!line.monospaced && !contentsPages.contains(line.page))
                styles[{ line.fontFamily, static_cast<int>(std::round(line.size * 2)) }] += line.text.size();
        }
        for (const auto& [style, count] : styles) {
            if (count > mostCharacters) {
                mostCharacters = count;
                bodyFamily = style.first;
                bodySize = static_cast<double>(style.second) / 2;
            }
        }
    }
    const auto isHeading = [&](const OutlineLine& line) {
        if (contentsPages.contains(line.page) || line.monospaced || line.text.empty() || isCaption(line.text)
            || isContentsLabel(line.text))
            return false;
        const auto repeated = margins.find(stripEdgeNumbers(line.text));
        if (isMarginLine(line) && repeated != margins.end() && repeated->second.size() >= 3)
            return false;
        return line.size >= bodySize * 1.22
            || (line.boldFraction >= 0.7 && line.size >= bodySize
                && (line.fontFamily != bodyFamily || line.size >= bodySize * 1.05));
    };
    std::vector<Candidate> result;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!isHeading(lines[i]))
            continue;
        OutlineLine heading = lines[i];
        heading.text = trimText(heading.text);
        bool chapter = false;
        // A display chapter number and its title can use different sizes and
        // separate blocks. Normal page numbers are too small to qualify.
        if (parsePageNumber(heading.text) && heading.size >= bodySize * 1.8 && i + 1 < lines.size()) {
            const auto& next = lines[i + 1];
            if (isHeading(next) && !parsePageNumber(next.text) && heading.page == next.page
                && heading.fontFamily == next.fontFamily && next.size >= bodySize * 1.8
                && next.top >= heading.bottom - heading.size * 0.5 && next.top - heading.bottom < heading.size * 1.5
                && (std::abs(heading.right - next.right) < heading.size
                    || std::abs(heading.left - next.left) < heading.size)) {
                heading.size = next.size;
                appendLine(heading, next);
                chapter = true;
                ++i;
            }
        }
        while (i + 1 < lines.size() && isHeading(lines[i + 1]) && canJoinLines(heading, lines[i + 1])) {
            appendLine(heading, lines[++i]);
        }
        // Check the joined title so a standalone chapter number can still acquire its title.
        if (heading.text.size() > MaxGeneratedHeadingBytes || normalizeTitle(heading.text).empty())
            continue;
        result.push_back({ std::move(heading), chapter, 0 });
        if (result.size() > MaxGeneratedOutlineNodes)
            throw std::length_error("generated outline node limit exceeded");
    }
    return result;
}

bool applyContents(std::vector<Candidate>& candidates, std::vector<ContentsEntry>& entries)
{
    std::unordered_map<std::string, std::vector<const ContentsEntry*>> byTitle;
    std::unordered_map<std::string, std::vector<const Candidate*>> bodyTitles;
    std::vector<double> entrySizes;
    for (const auto& entry : entries) {
        byTitle[headingKey(entry.line.text)].push_back(&entry);
        entrySizes.push_back(entry.line.size);
    }
    for (const auto& candidate : candidates)
        bodyTitles[headingKey(candidate.line.text)].push_back(&candidate);
    std::map<int, int> offsets;
    int anchors = 0;
    std::size_t matchedTitles = 0;
    for (const auto& [title, values] : byTitle) {
        const auto found = bodyTitles.find(title);
        if (found == bodyTitles.end())
            continue;
        ++matchedTitles;
        if (values.size() == 1 && found->second.size() == 1 && values.front()->printedPage) {
            ++offsets[found->second.front()->line.page - *values.front()->printedPage];
            ++anchors;
        }
    }
    // An incidental "contents" label is insufficient. Require actual title
    // matches and a page mapping corroborated by independent unique headings.
    if (matchedTitles < 3 || offsets.empty())
        return false;
    const auto best = std::max_element(
        offsets.begin(), offsets.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    if (best->second < 2 || best->second * 5 < anchors * 3)
        return false;
    const int offset = best->first;
    const auto sizes = collectSizes(std::move(entrySizes));
    for (auto& entry : entries)
        entry.level = std::min(7, rankSize(entry.line.size, sizes));
    std::vector<Candidate> selected;
    bool backMatter = false;
    double chapterSize = 0;
    for (const auto& candidate : candidates) {
        if (candidate.chapter)
            chapterSize = std::max(chapterSize, candidate.line.size);
    }
    for (auto& candidate : candidates) {
        const auto found = byTitle.find(headingKey(candidate.line.text));
        if (found == byTitle.end())
            continue;
        const ContentsEntry* match = nullptr;
        int distance = 2;
        for (const auto* entry : found->second) {
            const int current = entry->printedPage ? std::abs(candidate.line.page - (*entry->printedPage + offset)) : 1;
            if (current < distance) {
                distance = current;
                match = entry;
            }
        }
        if (!match)
            continue;
        candidate.level = match->level;
        const auto key = headingKey(candidate.line.text);
        if (chapterSize > 0 && candidate.line.size >= chapterSize * 0.95
            && (key == "index" || key == "bibliography" || key == "references" || key == "glossary"))
            backMatter = true;
        if (backMatter && !candidate.chapter && candidate.line.size >= chapterSize * 0.95)
            candidate.level = 0;
        selected.push_back(candidate);
    }
    if (selected.size() < 3)
        return false;
    candidates = std::move(selected);
    return true;
}

void assignLevels(std::vector<Candidate>& candidates)
{
    std::vector<double> ordinarySizes;
    bool hasSections = false;
    bool hasChapters = false;
    for (const auto& candidate : candidates) {
        hasSections = hasSections || isSectionTitle(candidate.line.text);
        hasChapters = hasChapters || candidate.chapter;
        if (!candidate.chapter && !isSectionTitle(candidate.line.text))
            ordinarySizes.push_back(candidate.line.size);
    }
    const auto sizes = collectSizes(std::move(ordinarySizes));
    for (auto& candidate : candidates) {
        if (isSectionTitle(candidate.line.text))
            candidate.level = 0;
        else if (candidate.chapter)
            candidate.level = hasSections ? 1 : 0;
        else if (const auto number = parseHeading(candidate.line.text))
            candidate.level = static_cast<int>(number->components.size()) - 1 + (hasSections ? 1 : 0);
        else
            candidate.level = rankSize(candidate.line.size, sizes) + (hasSections ? 1 : 0) + (hasChapters ? 1 : 0);
        candidate.level = std::min(7, candidate.level);
    }
}

} // namespace

std::vector<Model::OutlineNode> buildGeneratedOutline(std::vector<OutlineLine> lines)
{
    // Reject malformed records before sorting or dividing by page dimensions.
    std::erase_if(lines, [](const OutlineLine& line) {
        return line.page < 0 || !std::isfinite(line.size) || line.size <= 0 || line.size >= 1000
            || !std::isfinite(line.pageWidth) || line.pageWidth <= 0 || !std::isfinite(line.pageHeight)
            || line.pageHeight <= 0 || !std::isfinite(line.left) || !std::isfinite(line.right)
            || !std::isfinite(line.top) || !std::isfinite(line.bottom) || line.right < line.left
            || line.bottom < line.top;
    });
    // Geometry gives a stable order even when the PDF's content stream draws
    // a heading after its paragraphs or uses separate blocks for each line.
    std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
        if (a.page != b.page)
            return a.page < b.page;
        if (a.top != b.top)
            return a.top < b.top;
        return a.left < b.left;
    });
    std::set<int> contentsPages;
    for (const auto& line : lines) {
        if (line.page < 64 && isContentsLabel(line.text))
            contentsPages.insert(line.page);
    }
    auto candidates = findCandidates(lines, contentsPages);
    auto contents = readContents(lines, contentsPages);
    if (!applyContents(candidates, contents)) {
        // An uncorroborated contents label must not suppress its page's headings.
        if (!contentsPages.empty())
            candidates = findCandidates(lines, { });
        assignLevels(candidates);
    }
    std::vector<Model::OutlineNode> result;

    struct Parent {
        int level;
        Model::OutlineNode* node;
        std::vector<unsigned> number;
    };

    std::vector<Parent> parents;
    std::set<std::pair<int, std::string>> emitted;
    for (const auto& candidate : candidates) {
        const auto& line = candidate.line;
        if (!emitted.emplace(line.page, headingKey(line.text)).second)
            continue;
        while (!parents.empty() && parents.back().level >= candidate.level)
            parents.pop_back();
        const auto number = parseHeading(line.text);
        if (number) {
            // A missing numeric parent must not attach 11.2 beneath chapter 10.
            while (!parents.empty() && !parents.back().number.empty()) {
                const auto& prefix = parents.back().number;
                if (prefix.size() < number->components.size()
                    && std::equal(prefix.begin(), prefix.end(), number->components.begin()))
                    break;
                parents.pop_back();
            }
        }
        Model::OutlineNode node;
        node.title = line.text;
        node.link.valid = true;
        node.link.viewport = line.viewport;
        if (node.link.viewport.page < 0) {
            node.link.viewport.page = line.page;
            node.link.viewport.coordinateMask = Model::Viewport::CoordinateX | Model::Viewport::CoordinateY;
            node.link.viewport.normalizedX = std::clamp(line.left / line.pageWidth, 0.0, 1.0);
            node.link.viewport.normalizedY =
                normalizeDestinationY(std::clamp(line.top / line.pageHeight, 0.0, 1.0), line.pageHeight);
        }
        auto& siblings = parents.empty() ? result : parents.back().node->children;
        siblings.push_back(std::move(node));
        parents.push_back({ candidate.level, &siblings.back(), number ? number->components : std::vector<unsigned>() });
    }
    return result;
}

} // namespace Mu::Worker::Engine
