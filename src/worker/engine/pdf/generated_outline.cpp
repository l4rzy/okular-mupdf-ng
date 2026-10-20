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

extern "C" {
#include <mupdf/fitz/string-util.h>
#include <mupdf/ucdn.h>
}

namespace Mu::Worker::Engine {

namespace {

// Text recognition bounds.
constexpr unsigned MaxHeadingNumberComponent = 9999;
constexpr std::size_t MaxHeadingNumberDepth = 8;
constexpr int MaxPrintedPageNumber = 100'000;
constexpr std::size_t MaxRomanPageMarkerBytes = 8;

// Geometry tolerances: page ratios, font-size multiples (em), and PDF points.
constexpr double HeaderBoundaryRatio = 0.09;
constexpr double FooterBoundaryRatio = 0.93;
constexpr double FontSizeToleranceRatio = 0.05;
constexpr double MinMatchingFontSizeRatio = 0.95;
constexpr double BoldFractionTolerance = 0.3;
constexpr double MaxLineOverlapEm = 0.5;
constexpr double MaxLineGapEm = 0.8;
constexpr double LeftAlignmentTolerancePoints = 5;
constexpr double RightAlignmentToleranceEm = 0.35;
// Compare sums of left/right edges, avoiding division to obtain each center.
constexpr double DoubledCenterAlignmentTolerancePoints = 10;

// Printed contents extraction and page-number association.
constexpr std::size_t MaxContentsComparisons = 2'000'000;
constexpr double ColumnBoundaryRatio = 0.5;
constexpr std::size_t MinColumnTitleBytes = 9;
constexpr int MinRightColumnTitles = 3;
constexpr double MaxPageNumberOverlapPoints = 2;
constexpr double MaxPageNumberGapRatio = 0.4;
// The comparison uses top + bottom, twice each text box's vertical center.
constexpr double DoubledPageNumberCenterToleranceEm = 1.5;
constexpr double MaxWrappedLineTopGapEm = 1.4;
constexpr int ContentsSearchPageLimit = 64;

// Body-style estimation and heading selection.
constexpr double FontSizeBucketsPerPoint = 2;
constexpr double MaxBodyBoldFraction = 0.5;
constexpr std::size_t MinBodySampleBytes = 25;
constexpr std::size_t MinRepeatedMarginPages = 3;
constexpr double MinHeadingSizeRatio = 1.22;
constexpr double MinHeadingBoldFraction = 0.7;
constexpr double MinBoldHeadingSizeRatio = 1.05;
constexpr double MinChapterSizeRatio = 1.8;
constexpr double MaxChapterTitleTopGapEm = 1.5;
constexpr double MaxFontSizePoints = 1000;
constexpr int MaxOutlineLevel = 7;

// Evidence required before printed contents can override typography.
constexpr std::size_t MinMatchedContentsTitles = 3;
constexpr int MinPageOffsetAnchors = 2;
constexpr int PageOffsetAgreementNumerator = 3;
constexpr int PageOffsetAgreementDenominator = 5;
constexpr int MaxContentsPageDistance = 1;
constexpr int UnspecifiedContentsPageDistance = 1;
constexpr std::size_t MinSelectedContentsHeadings = 3;

// =============================================================================
// Heading Text & Number Recognition
// =============================================================================

bool hasTitleLetters(std::string_view text)
{
    while (!text.empty()) {
        int codepoint = 0;
        const int length = fz_chartorunen(&codepoint, text.data(), text.size());
        switch (ucdn_get_general_category(static_cast<std::uint32_t>(codepoint))) {
        case UCDN_GENERAL_CATEGORY_LL:
        case UCDN_GENERAL_CATEGORY_LM:
        case UCDN_GENERAL_CATEGORY_LO:
        case UCDN_GENERAL_CATEGORY_LT:
        case UCDN_GENERAL_CATEGORY_LU:
            return true;
        default:
            break;
        }
        text.remove_prefix(static_cast<std::size_t>(length));
    }
    return false;
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
        if (error != std::errc() || number > MaxHeadingNumberComponent
            || result.components.size() >= MaxHeadingNumberDepth)
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
    if (error != std::errc() || end != trimmed.data() + trimmed.size() || number <= 0 || number > MaxPrintedPageNumber)
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
            || (!word.empty() && word.size() <= MaxRomanPageMarkerBytes
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

// =============================================================================
// Line Geometry & Joining
// =============================================================================

bool isMarginLine(const OutlineLine& line)
{
    return line.top < line.pageHeight * HeaderBoundaryRatio || line.bottom > line.pageHeight * FooterBoundaryRatio;
}

bool hasMatchingStyle(const OutlineLine& a, const OutlineLine& b)
{
    return a.fontFamily == b.fontFamily
        && std::abs(a.size - b.size) <= std::max(a.size, b.size) * FontSizeToleranceRatio
        && std::abs(a.boldFraction - b.boldFraction) < BoldFractionTolerance;
}

bool canJoinLines(const OutlineLine& a, const OutlineLine& b)
{
    if (a.page != b.page || !hasMatchingStyle(a, b) || b.top < a.bottom - a.size * MaxLineOverlapEm
        || b.top - a.bottom > a.size * MaxLineGapEm)
        return false;
    // Left, right, and centered display titles can all wrap across blocks.
    return std::abs(a.left - b.left) < LeftAlignmentTolerancePoints
        || std::abs(a.right - b.right) < a.size * RightAlignmentToleranceEm
        || std::abs((a.left + a.right) - (b.left + b.right)) < DoubledCenterAlignmentTolerancePoints;
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

// =============================================================================
// Table of Contents Extraction
// =============================================================================

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
        if (++comparisons > MaxContentsComparisons)
            throw std::length_error("generated outline contents work limit exceeded");
    };
    std::map<int, std::vector<const OutlineLine*>> pageNumbers;
    std::map<int, int> rightColumnTitles;
    for (const auto& line : lines) {
        if (!contentsPages.contains(line.page))
            continue;
        if (parsePageNumber(line.text))
            pageNumbers[line.page].push_back(&line);
        else if (line.left > line.pageWidth * ColumnBoundaryRatio && line.text.size() >= MinColumnTitleBytes
                 && !isMarginLine(line))
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
        const bool twoColumns = rightColumnTitles[title.page] >= MinRightColumnTitles;
        const auto findPage = [&](const OutlineLine& end) -> std::optional<int> {
            const OutlineLine* closest = nullptr;
            for (const auto* number : pageNumbers[title.page]) {
                checkWork();
                const auto& other = *number;
                if ((twoColumns && end.left < end.pageWidth * ColumnBoundaryRatio
                     && other.left > end.pageWidth * ColumnBoundaryRatio)
                    || other.left < end.right - MaxPageNumberOverlapPoints
                    || other.left - end.right > end.pageWidth * MaxPageNumberGapRatio
                    || std::abs((other.top + other.bottom) - (end.top + end.bottom))
                        > end.size * DoubledPageNumberCenterToleranceEm)
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
                if (candidate.top - end.bottom > end.size * MaxLineGapEm
                    || candidate.top - end.top > end.size * MaxWrappedLineTopGapEm)
                    break;
                if (candidate.top <= end.top || std::abs(candidate.left - end.left) >= LeftAlignmentTolerancePoints
                    || parsePageNumber(candidate.text))
                    continue;
                next = &candidate;
                break;
            }
            if (!next || !canJoinLines(end, *next) || next->top - end.top > end.size * MaxWrappedLineTopGapEm)
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

// =============================================================================
// Heading Candidate Detection
// =============================================================================

std::vector<double> collectSizes(std::vector<double> sizes)
{
    std::sort(sizes.begin(), sizes.end(), std::greater<>());
    std::vector<double> distinct;
    for (double size : sizes) {
        if (distinct.empty() || distinct.back() - size > distinct.back() * FontSizeToleranceRatio)
            distinct.push_back(size);
    }
    return distinct;
}

int rankSize(double size, const std::vector<double>& sizes)
{
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        if (size >= sizes[i] * MinMatchingFontSizeRatio)
            return static_cast<int>(i);
    }
    return static_cast<int>(sizes.size());
}

struct Candidate {
    OutlineLine line;
    bool chapter = false;
    int level = 0;
};

struct BodyStyle {
    std::string fontFamily;
    double fontSize = 0;
};

std::unordered_set<std::string> findRepeatedMargins(const std::vector<OutlineLine>& lines)
{
    std::unordered_map<std::string, std::set<int>> pagesByTitle;
    for (const auto& line : lines) {
        if (isMarginLine(line))
            pagesByTitle[stripEdgeNumbers(line.text)].insert(line.page);
    }
    std::unordered_set<std::string> repeated;
    for (const auto& [title, pages] : pagesByTitle) {
        if (pages.size() >= MinRepeatedMarginPages)
            repeated.insert(title);
    }
    return repeated;
}

BodyStyle estimateBodyStyle(const std::vector<OutlineLine>& lines, const std::set<int>& contentsPages)
{
    // Estimate body style across the document, excluding contents and code.
    // One code-heavy page must not reclassify its numbered instructions.
    std::map<std::pair<std::string, int>, std::size_t> styles;
    for (const auto& line : lines) {
        if (!contentsPages.contains(line.page) && !line.monospaced && line.boldFraction < MaxBodyBoldFraction
            && line.text.size() >= MinBodySampleBytes && !isMarginLine(line))
            styles[{ line.fontFamily, static_cast<int>(std::round(line.size * FontSizeBucketsPerPoint)) }] +=
                line.text.size();
    }
    BodyStyle body;
    std::size_t mostCharacters = 0;
    const auto selectDominantStyle = [&] {
        // Map order preserves the existing tie-break between equally common styles.
        for (const auto& [style, count] : styles) {
            if (count > mostCharacters) {
                mostCharacters = count;
                body.fontFamily = style.first;
                body.fontSize = static_cast<double>(style.second) / FontSizeBucketsPerPoint;
            }
        }
    };
    selectDominantStyle();
    // All-bold/short documents have no prose sample. Use their most frequent
    // style as a conservative baseline rather than accepting every line.
    if (body.fontSize == 0) {
        for (const auto& line : lines) {
            if (!line.monospaced && !contentsPages.contains(line.page))
                styles[{ line.fontFamily, static_cast<int>(std::round(line.size * FontSizeBucketsPerPoint)) }] +=
                    line.text.size();
        }
        selectDominantStyle();
    }
    return body;
}

std::vector<Candidate> findHeadingCandidates(const std::vector<OutlineLine>& lines,
                                             const std::set<int>& contentsPages,
                                             const BodyStyle& body,
                                             const std::unordered_set<std::string>& repeatedMargins)
{
    const auto isHeading = [&](const OutlineLine& line) {
        if (contentsPages.contains(line.page) || line.monospaced || line.text.empty() || isCaption(line.text)
            || isContentsLabel(line.text))
            return false;
        if (isMarginLine(line) && repeatedMargins.contains(stripEdgeNumbers(line.text)))
            return false;
        return line.size >= body.fontSize * MinHeadingSizeRatio
            || (line.boldFraction >= MinHeadingBoldFraction && line.size >= body.fontSize
                && (line.fontFamily != body.fontFamily || line.size >= body.fontSize * MinBoldHeadingSizeRatio));
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
        if (parsePageNumber(heading.text) && heading.size >= body.fontSize * MinChapterSizeRatio
            && i + 1 < lines.size()) {
            const auto& next = lines[i + 1];
            if (isHeading(next) && !parsePageNumber(next.text) && heading.page == next.page
                && heading.fontFamily == next.fontFamily && next.size >= body.fontSize * MinChapterSizeRatio
                && next.top >= heading.bottom - heading.size * MaxLineOverlapEm
                && next.top - heading.bottom < heading.size * MaxChapterTitleTopGapEm
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
        // Require letters after joining so table values are rejected while a
        // standalone chapter number can still acquire its title.
        if (heading.text.size() > MaxGeneratedHeadingBytes || normalizeTitle(heading.text).empty()
            || !hasTitleLetters(heading.text))
            continue;
        result.push_back({ std::move(heading), chapter, 0 });
        if (result.size() > MaxGeneratedOutlineNodes)
            throw std::length_error("generated outline node limit exceeded");
    }
    return result;
}

// =============================================================================
// Contents Matching & Outline Levels
// =============================================================================

bool hasReliablePageOffset(int agreeingAnchors, int totalAnchors)
{
    // Require at least two independent anchors and 60% agreement. Keep the
    // comparison in integers so acceptance at the boundary stays exact.
    return agreeingAnchors >= MinPageOffsetAnchors
        && agreeingAnchors * PageOffsetAgreementDenominator >= totalAnchors * PageOffsetAgreementNumerator;
}

// Entries are borrowed; the source vector stays alive and stable through matching.
using ContentsByTitle = std::unordered_map<std::string, std::vector<const ContentsEntry*>>;

ContentsByTitle indexContentsEntries(const std::vector<ContentsEntry>& entries)
{
    ContentsByTitle byTitle;
    for (const auto& entry : entries)
        byTitle[headingKey(entry.line.text)].push_back(&entry);
    return byTitle;
}

struct ContentsPageMapping {
    int pageOffset = 0;
    int agreeingAnchors = 0;
    int totalAnchors = 0;
    std::size_t matchedTitles = 0;
};

ContentsPageMapping inferContentsPageMapping(const std::vector<Candidate>& candidates,
                                             const ContentsByTitle& contentsByTitle)
{
    std::unordered_map<std::string, std::vector<const Candidate*>> bodyTitles;
    for (const auto& candidate : candidates)
        bodyTitles[headingKey(candidate.line.text)].push_back(&candidate);
    std::map<int, int> offsets;
    ContentsPageMapping mapping;
    for (const auto& [title, values] : contentsByTitle) {
        const auto found = bodyTitles.find(title);
        if (found == bodyTitles.end())
            continue;
        ++mapping.matchedTitles;
        if (values.size() == 1 && found->second.size() == 1 && values.front()->printedPage) {
            ++offsets[found->second.front()->line.page - *values.front()->printedPage];
            ++mapping.totalAnchors;
        }
    }
    if (!offsets.empty()) {
        const auto best = std::max_element(
            offsets.begin(), offsets.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
        mapping.pageOffset = best->first;
        mapping.agreeingAnchors = best->second;
    }
    return mapping;
}

bool hasReliableContentsMapping(const ContentsPageMapping& mapping)
{
    // An incidental "contents" label is insufficient. Require actual title
    // matches and a page mapping corroborated by independent unique headings.
    return mapping.matchedTitles >= MinMatchedContentsTitles
        && hasReliablePageOffset(mapping.agreeingAnchors, mapping.totalAnchors);
}

void assignContentsLevels(std::vector<ContentsEntry>& entries)
{
    std::vector<double> entrySizes;
    for (const auto& entry : entries)
        entrySizes.push_back(entry.line.size);
    const auto sizes = collectSizes(std::move(entrySizes));
    for (auto& entry : entries)
        entry.level = std::min(MaxOutlineLevel, rankSize(entry.line.size, sizes));
}

std::vector<Candidate>
matchContentsHeadings(const std::vector<Candidate>& candidates, const ContentsByTitle& contentsByTitle, int pageOffset)
{
    std::vector<Candidate> selected;
    bool backMatter = false;
    double chapterSize = 0;
    for (const auto& candidate : candidates) {
        if (candidate.chapter)
            chapterSize = std::max(chapterSize, candidate.line.size);
    }
    for (const auto& candidate : candidates) {
        const auto found = contentsByTitle.find(headingKey(candidate.line.text));
        if (found == contentsByTitle.end())
            continue;
        const ContentsEntry* match = nullptr;
        int distance = MaxContentsPageDistance + 1;
        for (const auto* entry : found->second) {
            const int current = entry->printedPage ? std::abs(candidate.line.page - (*entry->printedPage + pageOffset))
                                                   : UnspecifiedContentsPageDistance;
            if (current < distance) {
                distance = current;
                match = entry;
            }
        }
        if (!match)
            continue;
        Candidate selectedCandidate = candidate;
        selectedCandidate.level = match->level;
        const auto key = headingKey(candidate.line.text);
        if (chapterSize > 0 && candidate.line.size >= chapterSize * MinMatchingFontSizeRatio
            && (key == "index" || key == "bibliography" || key == "references" || key == "glossary"))
            backMatter = true;
        if (backMatter && !candidate.chapter && candidate.line.size >= chapterSize * MinMatchingFontSizeRatio)
            selectedCandidate.level = 0;
        selected.push_back(std::move(selectedCandidate));
    }
    return selected;
}

void assignTypographyLevels(std::vector<Candidate>& candidates)
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
        candidate.level = std::min(MaxOutlineLevel, candidate.level);
    }
}

void prepareOutlineLines(std::vector<OutlineLine>& lines)
{
    // Reject malformed records before sorting or dividing by page dimensions.
    std::erase_if(lines, [](const OutlineLine& line) {
        return line.page < 0 || !std::isfinite(line.size) || line.size <= 0 || line.size >= MaxFontSizePoints
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
}

std::set<int> findContentsPages(const std::vector<OutlineLine>& lines)
{
    std::set<int> pages;
    for (const auto& line : lines) {
        if (line.page < ContentsSearchPageLimit && isContentsLabel(line.text))
            pages.insert(line.page);
    }
    return pages;
}

std::vector<Model::OutlineNode> assembleOutline(const std::vector<Candidate>& candidates)
{
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

} // namespace

// =============================================================================
// Generated Outline Pipeline
// =============================================================================

std::vector<Model::OutlineNode> buildGeneratedOutline(std::vector<OutlineLine> lines)
{
    prepareOutlineLines(lines);
    const auto contentsPages = findContentsPages(lines);
    const auto repeatedMargins = findRepeatedMargins(lines);
    const auto body = estimateBodyStyle(lines, contentsPages);
    auto candidates = findHeadingCandidates(lines, contentsPages, body, repeatedMargins);

    auto contents = readContents(lines, contentsPages);
    const auto contentsByTitle = indexContentsEntries(contents);
    const auto mapping = inferContentsPageMapping(candidates, contentsByTitle);
    if (hasReliableContentsMapping(mapping)) {
        assignContentsLevels(contents);
        const auto selected = matchContentsHeadings(candidates, contentsByTitle, mapping.pageOffset);
        if (selected.size() >= MinSelectedContentsHeadings)
            return assembleOutline(selected);
    }

    // An uncorroborated contents label must not suppress its page's headings
    // or bias body-style estimation. Reconsider those pages as normal text.
    if (!contentsPages.empty()) {
        const std::set<int> noContentsPages;
        const auto fallbackBody = estimateBodyStyle(lines, noContentsPages);
        candidates = findHeadingCandidates(lines, noContentsPages, fallbackBody, repeatedMargins);
    }
    assignTypographyLevels(candidates);
    return assembleOutline(candidates);
}

} // namespace Mu::Worker::Engine
