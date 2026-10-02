// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_LAYERS_MODEL_HPP
#define MU_GENERATOR_LAYERS_MODEL_HPP

#include <QStandardItemModel>

#include <functional>
#include <optional>

#include "shared/model/types.hpp"

namespace Okular {

class Document;

}

namespace Mu::Generator {

/// Clear searches owned by Okular when their layer visibility becomes obsolete.
void resetLayerSearches(Okular::Document& document);

/// Qt adapter for MuPDF's default optional-content configuration UI.
class LayersModel final : public QStandardItemModel {
    Q_OBJECT

public:
    using SetLayer = std::function<std::optional<Model::LayersResponse>(const Model::SetLayerRequest&)>;

    explicit LayersModel(SetLayer setLayer, QObject* parent = nullptr);
    bool resetLayers(Model::LayersResponse response);
    void clearLayers();
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    /// Replay session visibility after the caller verifies the source identity.
    bool restoreLayers(Model::LayersResponse response, const SetLayer& restoreLayer);
    [[nodiscard]] bool isDefaultVisibility() const;

signals:
    /// Emitted before dataChanged triggers Okular's pixmap/form refresh.
    void visibilityChanged();
    void changeFailed();

private:
    [[nodiscard]] bool hasSameStructure(const Model::LayersResponse& response) const;

    SetLayer m_setLayer;
    Model::LayersResponse m_state;
    std::vector<bool> m_defaults;
    std::vector<QStandardItem*> m_items;
    bool m_changing = false;
};

} // namespace Mu::Generator

#endif
