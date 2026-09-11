// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_PLUGIN_OCR_CONSTANTS_HPP
#define MU_PLUGIN_OCR_CONSTANTS_HPP

namespace Mu::Plugin::OCR::Constant {

// Scroll-settle delay before OCR scheduling is allowed to run. Conservative by
// design: pages the user only scrolls through must not consume resources.
constexpr int DEBOUNCE_MS = 250;

// Worker OCR runs only for the settled focus page because a running worker job
// cannot be cancelled. A queued page farther than this from the focus is
// dropped instead of wasting the single worker slot.
constexpr int STALE_RADIUS = 2;

// Cap on the cache-prefetch window (focus plus immediate neighbours).
constexpr int MAX_QUEUED_PAGES = 3;

// Bounds for a page whose worker dispatch or recognition failed before it is
// reported through the failed() signal.
constexpr int MAX_ATTEMPTS = 3;
constexpr int RETRY_MS = 1000;

} // namespace Mu::Plugin::OCR::Constant
#endif // MU_PLUGIN_OCR_CONSTANTS_HPP
