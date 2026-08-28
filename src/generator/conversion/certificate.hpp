// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MUPDF_GENERATOR_CONVERSION_CERTIFICATE_HPP
#define MUPDF_GENERATOR_CONVERSION_CERTIFICATE_HPP

#include <okular/core/signatureutils.h>

#include "shared/model/types.hpp"

namespace Mu::Generator::Conversion {

Okular::CertificateInfo toOkularCertificateInfo(const Model::Certificate& certificate);

} // namespace Mu::Generator::Conversion

#endif
