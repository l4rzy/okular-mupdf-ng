// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_UTIL_REQUEST_TIMEOUT_HPP
#define MU_PLUGIN_UTIL_REQUEST_TIMEOUT_HPP

#include "shared/model/types.hpp"

namespace Mu::Plugin::Util {

/// Returns the response deadline in milliseconds for a worker request.
int requestTimeoutFor(const Model::RequestPayload& payload);

} // namespace Mu::Plugin::Util

#endif
