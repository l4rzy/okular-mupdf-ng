// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_FORM_CONSTRUCTION_HPP
#define MU_GENERATOR_PROXY_FORM_CONSTRUCTION_HPP

#include <QList>
#include <QVector>
#include <vector>

#include "shared/model/types.hpp"

namespace Okular {

class FormField;
class Page;

} // namespace Okular

namespace Mu::Generator::Proxy::Form {

class Coordinator;

// Consumes model fields and registers non-owning coordinator views. The caller
// transfers the returned proxies to Okular pages before the coordinator dies.
QList<Okular::FormField*> createFormFields(std::vector<Model::FormField>& fields, Coordinator& coordinator);

// Run after every page is constructed so radio groups can span pages.
void assignRadioSiblings(const QVector<Okular::Page*>& pages);

} // namespace Mu::Generator::Proxy::Form

#endif // MU_GENERATOR_PROXY_FORM_CONSTRUCTION_HPP
