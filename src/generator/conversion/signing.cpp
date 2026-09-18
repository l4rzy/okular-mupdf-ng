// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/signing.hpp"

#include "plugin/util/signature_image.hpp"
#include "plugin/util/signing_timestamp.hpp"

namespace Mu::Generator::Conversion {

Model::SignatureAppearance toModelSignatureAppearance(const Okular::NewSignatureData& data,
                                                      const Config::SignatureAppearanceOptions& options)
{
    Model::SignatureAppearance appearance;
    appearance.elements = options.simple ? Model::SignatureElementSimple : Model::SignatureElementDefault;
    appearance.reason = data.reason().toStdString();
    appearance.drawBorder = options.drawBorder;
    if (!options.simple)
        appearance.location = data.location().toStdString();
    const auto timestamp = Plugin::Util::SigningTimestamp::current(options.useUtc);
    appearance.signingEpochSeconds = timestamp.epochSeconds;
    appearance.signingDisplayDate = timestamp.displayDate.toStdString();
    return appearance;
}

} // namespace Mu::Generator::Conversion
