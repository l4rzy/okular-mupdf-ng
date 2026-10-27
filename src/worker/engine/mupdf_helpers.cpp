// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/mupdf_helpers.hpp"

namespace Mu::Worker::Engine {

bool lookupMetadataValue(
    fz_context* context, fz_document* document, const char* key, std::string& value, bool& found, std::string* error)
{
    char* volatile buffer = nullptr;
    volatile int required = 0;
    // `found` refers to caller storage, so longjmp cannot invalidate it; no
    // fz_var/volatile is needed to preserve it across the exception boundary.
    found = false;

    fz_try(context)
    {
        // MuPDF reports the required buffer size first. Keep the allocation inside
        // its exception boundary, then copy the result into an owning std::string
        // only after all fallible C API calls have completed.
        required = fz_lookup_metadata(context, document, key, nullptr, 0);
        if (required > 1 && required <= 1024 * 1024) {
            buffer = static_cast<char*>(fz_malloc(context, static_cast<std::size_t>(required)));
            if (fz_lookup_metadata(context, document, key, buffer, static_cast<std::size_t>(required)) >= required)
                found = true;
        }
    }
    fz_always(context)
    {
        // The fz allocator owns this temporary buffer; free it on every path that
        // does not transfer its contents to the C++ string below.
        if (!found)
            fz_free(context, buffer);
    }
    fz_catch(context)
    {
        if (error)
            *error = fz_caught_message(context);
        return false;
    }

    if (!found)
        return true;

    // `required` includes MuPDF's terminating NUL. The C++ value stores only the
    // metadata bytes, while the buffer remains valid until it is explicitly freed.
    try {
        value.assign(buffer, static_cast<std::size_t>(required - 1));
    } catch (...) {
        fz_free(context, buffer);
        throw;
    }
    fz_free(context, buffer);
    return true;
}

} // namespace Mu::Worker::Engine
