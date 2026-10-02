// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/layers_model.hpp"

#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <okular/core/document.h>

#include "shared/model/validation.hpp"

namespace Mu::Generator {

void resetLayerSearches(Okular::Document& document)
{
    document.cancelSearch();
    for (const int id : { PART_SEARCH_ID, PAGEVIEW_SEARCH_ID, SW_SEARCH_ID, PRESENTATION_SEARCH_ID })
        document.resetSearch(id);
}

LayersModel::LayersModel(SetLayer setLayer, QObject* parent)
    : QStandardItemModel(parent)
    , m_setLayer(std::move(setLayer))
{
}

void LayersModel::clearLayers()
{
    clear();
    m_state = { };
    m_defaults.clear();
    m_items.clear();
}

bool LayersModel::resetLayers(Model::LayersResponse response)
{
    if (!Model::isValidLayersResponse(response))
        return false;
    clearLayers();
    m_state = std::move(response);
    std::vector<QStandardItem*> parents { invisibleRootItem() };
    for (const auto& entry : m_state.entries) {
        parents.resize(static_cast<std::size_t>(entry.depth) + 1);
        auto* item = new QStandardItem(QString::fromStdString(entry.name));
        item->setEditable(false);
        item->setData(entry.id, Qt::UserRole);
        if (entry.type != Model::LayerType::Label) {
            item->setCheckable(true);
            item->setCheckState(entry.selected ? Qt::Checked : Qt::Unchecked);
            if (entry.locked)
                item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        }
        parents.back()->appendRow(item);
        parents.push_back(item);
        m_items.push_back(item);
        m_defaults.push_back(entry.selected);
    }
    return true;
}

bool LayersModel::hasSameStructure(const Model::LayersResponse& response) const
{
    if (!Model::isValidLayersResponse(response) || response.entries.size() != m_state.entries.size())
        return false;
    for (std::size_t i = 0; i < m_state.entries.size(); ++i) {
        const auto& old = m_state.entries[i];
        const auto& fresh = response.entries[i];
        if (old.id != fresh.id || old.depth != fresh.depth || old.name != fresh.name || old.type != fresh.type
            || old.locked != fresh.locked)
            return false;
    }
    return true;
}

bool LayersModel::isDefaultVisibility() const
{
    for (std::size_t i = 0; i < m_state.entries.size(); ++i) {
        if (m_state.entries[i].selected != m_defaults[i])
            return false;
    }
    return true;
}

bool LayersModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (role != Qt::CheckStateRole)
        return QStandardItemModel::setData(index, value, role);
    if (!index.isValid() || index.model() != this || m_changing
        || (value.toInt() != Qt::Checked && value.toInt() != Qt::Unchecked))
        return false;
    const auto id = index.data(Qt::UserRole).toInt();
    if (id < 0 || static_cast<std::size_t>(id) >= m_state.entries.size())
        return false;
    const auto& entry = m_state.entries[static_cast<std::size_t>(id)];
    if (entry.type == Model::LayerType::Label || entry.locked)
        return false;
    const bool selected = value.toInt() == Qt::Checked;
    if (entry.selected == selected)
        return true;
    QScopedValueRollback changing(m_changing, true);
    const auto response = m_setLayer({ m_state.generation, id, selected });
    if (!response || response->generation != m_state.generation || !hasSameStructure(*response)) {
        Q_EMIT changeFailed();
        return false;
    }
    std::vector<QModelIndex> changed;
    {
        // Update every radio peer before the first observer receives a signal.
        QSignalBlocker blocker(this);
        for (std::size_t i = 0; i < response->entries.size(); ++i) {
            if (m_state.entries[i].selected != response->entries[i].selected) {
                m_items[i]->setCheckState(response->entries[i].selected ? Qt::Checked : Qt::Unchecked);
                changed.push_back(m_items[i]->index());
            }
        }
        m_state = *response;
    }
    if (!changed.empty())
        Q_EMIT visibilityChanged();
    for (const auto& changedIndex : changed)
        Q_EMIT dataChanged(changedIndex, changedIndex, { Qt::CheckStateRole });
    return true;
}

bool LayersModel::restoreLayers(Model::LayersResponse response, const SetLayer& restoreLayer)
{
    if (!hasSameStructure(response))
        return false;
    // Deselect first, then select, so radio-group replay is order-independent.
    for (bool selected : { false, true }) {
        for (const auto& desired : m_state.entries) {
            if (desired.type == Model::LayerType::Label || desired.locked || desired.selected != selected
                || response.entries[static_cast<std::size_t>(desired.id)].selected == selected)
                continue;
            auto updated = restoreLayer({ response.generation, desired.id, selected });
            if (!updated || updated->generation != response.generation || !hasSameStructure(*updated))
                return false;
            response = std::move(*updated);
        }
    }
    for (std::size_t i = 0; i < response.entries.size(); ++i) {
        if (response.entries[i].selected != m_state.entries[i].selected)
            return false;
    }
    m_state = std::move(response);
    return true;
}

} // namespace Mu::Generator
