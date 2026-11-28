// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_SHARED_LOGGING_HPP
#define MU_SHARED_LOGGING_HPP

#include <paperbox/logging.hpp>

#define MU_LOG(level, component, message) MU_LOG_##level((component), (message))
#define MU_LOG_critical(component, message)                                                                            \
    ::paperbox::log::write(::paperbox::log::Level::Critical, (component), (message))
#define MU_LOG_warning(component, message)                                                                             \
    ::paperbox::log::write(::paperbox::log::Level::Warning, (component), (message))

#ifdef MU_DEBUG_ENABLED
#define MU_LOG_debug(component, message) ::paperbox::log::write(::paperbox::log::Level::Debug, (component), (message))
#else // MU_DEBUG_ENABLED
#define MU_LOG_debug(...)                                                                                              \
    do {                                                                                                               \
    } while (false)
#endif // MU_DEBUG_ENABLED

#endif // MU_SHARED_LOGGING_HPP
