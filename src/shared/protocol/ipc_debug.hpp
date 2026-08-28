// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_SHARED_PROTOCOL_IPC_DEBUG_HPP
#define MUPDF_SHARED_PROTOCOL_IPC_DEBUG_HPP

#ifdef MU_DEBUG_ENABLED

#include <sstream>
#include <string>

#include "shared/model/types.hpp"

namespace Mu::IPC::Debug {

namespace Detail {

void page(std::ostringstream& out, const Model::PageInfo& value);

}

std::string request(const Model::RequestMessage& message, bool colorize = false);
std::string response(const Model::ResponseMessage& message, bool colorize = false);
std::string notification(const Model::NotificationMessage& message, bool colorize = false);

} // namespace Mu::IPC::Debug

#endif // MU_DEBUG_ENABLED

#endif // MUPDF_SHARED_PROTOCOL_IPC_DEBUG_HPP
