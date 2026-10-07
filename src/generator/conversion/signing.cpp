// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/signing.hpp"

#include "generator/config/okular_emblem.hpp"
#include "plugin/util/signing_timestamp.hpp"

namespace Mu::Generator::Conversion {

std::optional<Model::SignatureAppearance> toModelSignatureAppearance(const Okular::NewSignatureData& data,
                                                                     const Config::SignatureAppearanceOptions& options)
{
    Model::SignatureAppearance appearance;
    appearance.elements = options.simple ? Model::SignatureElementSimple : Model::SignatureElementDefault;
    const auto logo = static_cast<std::uint8_t>(Model::SignatureElement::Logo);
    if (options.emblem == Config::SignatureEmblem::Okular) {
        appearance.emblemImage = Config::encodeOkularEmblem();
        if (appearance.emblemImage.empty())
            return std::nullopt;
    }
    if (options.emblem == Config::SignatureEmblem::MuPDF)
        appearance.elements |= logo;
    else
        appearance.elements &= static_cast<std::uint8_t>(~logo);
    appearance.reason = data.reason().toStdString();
    appearance.location = data.location().toStdString();
    appearance.drawBorder = options.drawBorder;
    const auto timestamp = Plugin::Util::SigningTimestamp::current(options.useUtc);
    appearance.signingEpochSeconds = timestamp.epochSeconds;
    appearance.signingDisplayDate = timestamp.displayDate.toStdString();
    return appearance;
}

} // namespace Mu::Generator::Conversion
