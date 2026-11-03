// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MU_TEST_EXPORT_PROGRESS_HPP
#define MU_TEST_EXPORT_PROGRESS_HPP

#include <QSignalSpy>

#include "shared/model/types.hpp"

namespace Mu::Test {

/// Terminal results from the export's single lifecycle signal.
inline QList<Model::ExportProgressNotification> exportCompletions(const QSignalSpy& updates)
{
    QList<Model::ExportProgressNotification> results;
    for (const auto& arguments : updates) {
        const auto progress = arguments.front().value<Model::ExportProgressNotification>();
        if (progress.done())
            results.append(progress);
    }
    return results;
}

} // namespace Mu::Test
#endif
