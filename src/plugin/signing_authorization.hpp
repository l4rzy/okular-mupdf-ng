// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_SIGNING_AUTHORIZATION_HPP
#define MU_PLUGIN_SIGNING_AUTHORIZATION_HPP

#include "shared/model/types.hpp"

namespace Mu::Plugin {

/// Authorizes one CMS callback for the user's active signing request.
struct SigningAuthorization {
    std::uint64_t requestId = 0;
    std::string certificateNickname;
    bool consumed = false;

    bool accept(const Model::SignInput& input)
    {
        if (requestId == 0 || consumed || input.jobId != requestId || input.certificateNickname != certificateNickname)
            return false;
        consumed = true;
        return true;
    }
};

} // namespace Mu::Plugin

#endif // MU_PLUGIN_SIGNING_AUTHORIZATION_HPP
