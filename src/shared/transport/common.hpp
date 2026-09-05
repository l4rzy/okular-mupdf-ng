// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_SHARED_TRANSPORT_COMMON_HPP
#define MU_SHARED_TRANSPORT_COMMON_HPP

/**
 * @file common.hpp
 * @brief Shared transport limits and deadline budgets for okular-mupdf-worker.
 *
 * This header is shared between the worker executable and the plugin. It has
 * no dependencies beyond the C++ standard library, so it can be included by
 * both sides without pulling in Qt, MuPDF, or Okular.
 */

namespace Mu::IPC {

inline constexpr int PROTOCOL_VERSION = 1;

namespace Timeout {

inline constexpr int ControlReadMs = 30'000;
inline constexpr int ControlWriteMs = 30'000;
inline constexpr int GenericOpMs = 5'000;
inline constexpr int HandshakeMs = 30'000;
inline constexpr int RenderMs = 15'000;
inline constexpr int OcrMs = 45'000;
inline constexpr int SignMs = 45'000;
inline constexpr int SignRoundTripMs = 30'000;
inline constexpr int FdChannelTimeoutMs = 5'000;

} // namespace Timeout

} // namespace Mu::IPC

#endif // MU_SHARED_TRANSPORT_COMMON_HPP
