// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MUPDF_SHARED_LOGGING_HPP
#define MUPDF_SHARED_LOGGING_HPP

#include <string_view>

namespace Mu::Log {

enum class Level {
    Critical,
    Warning,
    Debug,
};

void write(Level level, std::string_view component, std::string_view message);

} // namespace Mu::Log

#define MU_LOG(level, component, message) MU_LOG_##level((component), (message))
#define MU_LOG_critical(component, message) ::Mu::Log::write(::Mu::Log::Level::Critical, (component), (message))
#define MU_LOG_warning(component, message) ::Mu::Log::write(::Mu::Log::Level::Warning, (component), (message))

#ifdef MU_DEBUG_ENABLED
#define MU_LOG_debug(component, message) ::Mu::Log::write(::Mu::Log::Level::Debug, (component), (message))
#else
#define MU_LOG_debug(...)                                                                                              \
    do {                                                                                                               \
    } while (false)
#endif

#endif // MUPDF_SHARED_LOGGING_HPP
