// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/mobi/document.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <unistd.h>

#include "sys/sys.hpp"

namespace Mu::Worker::Engine {

namespace {

bool readHeader(int fd, std::span<unsigned char> bytes, off_t offset)
{
    while (!bytes.empty()) {
        const ssize_t count = ::pread(fd, bytes.data(), bytes.size(), offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        bytes = bytes.subspan(static_cast<std::size_t>(count));
        offset += count;
    }
    return true;
}

std::uint32_t readBigEndian32(const unsigned char* bytes) noexcept
{
    return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8)
        | std::uint32_t(bytes[3]);
}

} // namespace

bool MobiDocument::openFd(int fd, std::string, std::string* error)
{
    close();
    Sys::FileDescriptor input(fd);
    // Inspect only bounded headers before handing the source to MuPDF. Its
    // legacy reader skips the encryption field, so reject DRM explicitly.
    std::array<unsigned char, 94> database { };
    if (!readHeader(fd, database, 0) || std::memcmp(database.data() + 60, "BOOKMOBI", 8) != 0)
        return fail(error, "invalid legacy MOBI header");
    const unsigned recordCount = (unsigned(database[76]) << 8) | database[77];
    const auto firstRecord = readBigEndian32(database.data() + 78);
    const auto secondRecord = readBigEndian32(database.data() + 86);
    if (recordCount < 2 || firstRecord < 78U + recordCount * 8U || secondRecord < firstRecord
        || secondRecord - firstRecord < 40)
        return fail(error, "invalid MOBI record table");

    std::array<unsigned char, 40> header { };
    if (!readHeader(fd, header, static_cast<off_t>(firstRecord)) || std::memcmp(header.data() + 16, "MOBI", 4) != 0)
        return fail(error, "invalid legacy MOBI record header");
    const unsigned compression = (unsigned(header[0]) << 8) | header[1];
    if (compression != 1 && compression != 2)
        return fail(error, "unsupported MOBI compression; only uncompressed and PalmDOC are supported");
    if (header[12] != 0 || header[13] != 0)
        return fail(error, "encrypted MOBI documents are not supported");
    if (readBigEndian32(header.data() + 36) > 7)
        return fail(error, "only legacy MOBI documents are supported");

    // Use a format hint independent of the filename (including memory opens).
    return EpubDocument::openFd(input.release(), "document.mobi", error);
}

DocumentMetadata MobiDocument::metadata(const std::vector<std::string>& keys, std::string* error) const
{
    auto result = EpubDocument::metadata(keys, error);
    if (isOpen())
        result.mimeType = Model::documentTypeToMime(Model::DocumentType::Mobi);
    return result;
}

} // namespace Mu::Worker::Engine
