// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/caching/pdf_toc_cache.hpp"
#include "plugin/caching/cache_file.hpp"
#include "plugin/caching/epub_cache.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <sys/stat.h>
#include <unistd.h>

namespace Mu::Plugin::Caching::PDF {
namespace {

bool validateNodes(const std::vector<Model::OutlineNode>& nodes, int pages, std::size_t depth, std::size_t& count)
{
    if (depth > 8 || nodes.size() > 5000 - count)
        return false;
    count += nodes.size();
    for (const auto& node : nodes) {
        const auto& viewport = node.link.viewport;
        if (node.title.empty() || node.title.size() > 160 || !node.link.valid || node.link.external
            || !node.link.uri.empty() || viewport.page < 0 || viewport.page >= pages
            || viewport.coordinateMask != (Model::Viewport::CoordinateX | Model::Viewport::CoordinateY)
            || !std::isfinite(viewport.normalizedX) || !std::isfinite(viewport.normalizedY) || viewport.normalizedX < 0
            || viewport.normalizedX > 1 || viewport.normalizedY < 0 || viewport.normalizedY > 1
            || !validateNodes(node.children, pages, depth + 1, count))
            return false;
    }
    return true;
}

} // namespace

QString tocCachePath(int sourceFd)
{
    struct stat before { }, after { };
    if (::fstat(sourceFd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size < 0)
        return { };
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("pdf-generated-toc-v7"));
    hash.addData(QByteArrayView("\0", 1));
    std::array<char, 65536> buffer;
    off_t offset = 0;
    while (offset < before.st_size) {
        const auto remaining = static_cast<std::size_t>(std::min<off_t>(before.st_size - offset, buffer.size()));
        const auto bytes = ::pread(sourceFd, buffer.data(), remaining, offset);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0)
            return { };
        hash.addData(QByteArrayView(buffer.data(), bytes));
        offset += bytes;
    }
    if (::fstat(sourceFd, &after) != 0 || before.st_size != after.st_size
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return { };
    return directory(QStringLiteral("pdf_toc")) + QLatin1Char('/') + QString::fromLatin1(hash.result().toHex())
        + QStringLiteral(".bin");
}

std::optional<std::vector<Model::OutlineNode>> loadToc(const QString& cachePath, int pageCount)
{
    if (cachePath.isEmpty() || pageCount < 0)
        return std::nullopt;
    // Reuse the bounded, versioned outline container used by EPUB. PDF entries
    // have their own content-addressed namespace and never contain accelerators.
    auto entry = EPUB::Cache::loadAt(cachePath);
    if (!entry)
        return std::nullopt;
    if (!entry->outline || entry->accelerator) {
        QFile::remove(cachePath);
        return std::nullopt;
    }
    std::size_t count = 0;
    if (!validateNodes(*entry->outline, pageCount, 0, count)) {
        QFile::remove(cachePath);
        return std::nullopt;
    }
    return std::move(entry->outline);
}

bool saveToc(const QString& cachePath, int pageCount, const std::vector<Model::OutlineNode>& nodes)
{
    std::size_t count = 0;
    return !cachePath.isEmpty() && pageCount >= 0 && validateNodes(nodes, pageCount, 0, count)
        && EPUB::Cache::saveOutlineAt(cachePath, nodes);
}

} // namespace Mu::Plugin::Caching::PDF
