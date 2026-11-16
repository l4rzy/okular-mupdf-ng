// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "generator/okular_compat.hpp"
#include <okular/core/document.h>

namespace Mu::Test {

inline qsizetype synopsisCount(const Okular::DocumentSynopsis& synopsis)
{
#if MU_OKULAR_STRUCTURED_SYNOPSIS
    return synopsis.children().size();
#else // MU_OKULAR_STRUCTURED_SYNOPSIS
    return synopsis.childNodes().size();
#endif // MU_OKULAR_STRUCTURED_SYNOPSIS
}

inline QString firstSynopsisTitle(const Okular::DocumentSynopsis& synopsis)
{
#if MU_OKULAR_STRUCTURED_SYNOPSIS
    const auto children = synopsis.children();
    return children.isEmpty() ? QString() : children.front().title();
#else // MU_OKULAR_STRUCTURED_SYNOPSIS
    return synopsis.documentElement().tagName();
#endif // MU_OKULAR_STRUCTURED_SYNOPSIS
}

} // namespace Mu::Test
