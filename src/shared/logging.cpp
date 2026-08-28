// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "logging.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>

namespace Mu::Log {
namespace {

const char* name(Level level)
{
    switch (level) {
    case Level::Critical:
        return "critical";
    case Level::Warning:
        return "warning";
    case Level::Debug:
        return "debug";
    }
    return "unknown";
}

void writeTimestamp(std::ostream& out)
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const auto timer = system_clock::to_time_t(now);
    std::tm tm_info { };
    ::localtime_r(&timer, &tm_info);

    out << '[' << std::setfill('0') << std::setw(2) << tm_info.tm_hour << ':' << std::setw(2) << tm_info.tm_min << ':'
        << std::setw(2) << tm_info.tm_sec << '.' << std::setw(3) << ms.count() << "] ";
}

} // namespace

void write(Level level, std::string_view component, std::string_view message)
{
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    writeTimestamp(std::clog);
    std::clog << '[' << name(level) << "] [" << component << "] " << message << '\n';
}

} // namespace Mu::Log
