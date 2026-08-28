// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/document_base.hpp"

#include <unistd.h>

namespace Mu::Worker::Engine {

using namespace ::Mu::Model;

// =============================================================================
// Error Handling Helpers
// =============================================================================

bool DocumentBase::fail(std::string* error, const char* message)
{
    if (error)
        *error = message ? message : "operation not supported for this document type";
    return false;
}

bool DocumentBase::fail(std::string* error, std::string_view message)
{
    if (error)
        *error = message.empty() ? std::string("operation not supported for this document type") : std::string(message);
    return false;
}

// =============================================================================
// Default Implementations for Format-Specific Operations
// =============================================================================

bool DocumentBase::addAnnotation(int, const Annotation&, std::int32_t*, std::string* error)
{
    return fail(error, "annotations are not supported for this document type");
}

bool DocumentBase::modifyAnnotation(int, std::int32_t, const Annotation&, bool, std::string* error)
{
    return fail(error, "annotations are not supported for this document type");
}

bool DocumentBase::removeAnnotation(int, std::int32_t, std::string* error)
{
    return fail(error, "annotations are not supported for this document type");
}

bool DocumentBase::saveFd(int fd, std::string* error)
{
    if (fd >= 0)
        ::close(fd);
    return fail(error, "save is not supported for this document type");
}

bool DocumentBase::savePdfFd(int fd, const std::vector<int>&, std::string* error)
{
    if (fd >= 0)
        ::close(fd);
    return fail(error, "save is not supported for this document type");
}

bool DocumentBase::signFd(const Model::SignRequest&, CmsCallback, int outputFd, SigningResult*, std::string* error)
{
    if (outputFd >= 0)
        ::close(outputFd);
    return fail(error, "signing is not supported for this document type");
}

bool DocumentBase::updateFormField(
    int, std::int32_t, const Model::FormValue&, std::vector<FieldMutation>*, std::string* error)
{
    return fail(error, "form field editing is not supported for this document type");
}

bool DocumentBase::resetForm(int, std::int32_t, std::vector<FieldMutation>*, std::string* error)
{
    return fail(error, "form reset is not supported for this document type");
}

std::vector<Font> DocumentBase::fonts(const std::vector<int>&, std::string*) const
{
    return { };
}

std::vector<EmbeddedFile> DocumentBase::embeddedFiles(std::size_t, std::size_t, bool*, std::string*) const
{
    return { };
}

} // namespace Mu::Worker::Engine
