// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/kindle/document.hpp"

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
    return EpubDocument::openFd(output.release(), "document.epub", error);
}

DocumentMetadata KindleDocument::metadata(const std::vector<std::string>& keys, std::string* error) const
{
    auto result = EpubDocument::metadata(keys, error);
    if (isOpen())
        result.mimeType = Model::documentTypeToMime(Model::DocumentType::Kindle);
    return result;
}

} // namespace Mu::Worker::Engine
