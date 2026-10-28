// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/kindle/document.hpp"

#include <array>
#include <cerrno>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "sys/sys.hpp"

extern "C" int mupdfng_kindle_convert(int input, int output);

namespace Mu::Worker::Engine {

bool KindleDocument::openFd(int fd, std::string, std::string* error)
{
    close();
    Sys::FileDescriptor input(fd);
    Sys::FileDescriptor output(static_cast<int>(::syscall(SYS_memfd_create, "kindle-epub", MFD_CLOEXEC)));
    if (!output)
        return fail(error, "could not create temporary EPUB");
    if (mupdfng_kindle_convert(input.get(), output.get()) != 0)
        return fail(error, "could not convert Kindle book (invalid, unsupported, encrypted, or too large)");
    if (::lseek(output.get(), 0, SEEK_SET) < 0)
        return fail(error, "could not rewind converted EPUB");
    Sys::FileDescriptor retained(::dup(output.get()));
    if (!retained)
        return fail(error, "could not retain converted EPUB");
    if (!EpubDocument::openFd(output.release(), "document.epub", error))
        return false;
    m_epub = std::move(retained);
    return true;
}

void KindleDocument::close() noexcept
{
    m_epub.reset();
    EpubDocument::close();
}

bool KindleDocument::exportEpubFd(int fd, std::string* error) const
{
    Sys::FileDescriptor output(fd);
    if (!isOpen() || !m_epub || !output)
        return fail(error, "Kindle document is not open");
    std::array<char, 65536> bytes;
    off_t offset = 0;
    for (;;) {
        const auto count = ::pread(m_epub.get(), bytes.data(), bytes.size(), offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            return fail(error, "could not read converted EPUB");
        if (count == 0)
            return true;
        offset += count;
        ssize_t written = 0;
        while (written < count) {
            const auto result =
                ::write(output.get(), bytes.data() + written, static_cast<std::size_t>(count - written));
            if (result < 0 && errno == EINTR)
                continue;
            if (result <= 0)
                return fail(error, "could not write converted EPUB");
            written += result;
        }
    }
}

DocumentMetadata KindleDocument::metadata(const std::vector<std::string>& keys, std::string* error) const
{
    auto result = EpubDocument::metadata(keys, error);
    if (isOpen())
        result.mimeType = Model::documentTypeToMime(Model::DocumentType::Kindle);
    return result;
}

} // namespace Mu::Worker::Engine
