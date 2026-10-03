// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QTest>
#include <array>

#include "shared/model/validation.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/protocol/zpp_codec.hpp"

class TestLayerResponse : public QObject {
    Q_OBJECT

private slots:

    void acceptsValidLayers()
    {
        const Mu::Model::LayersResponse layers { 7,
                                                 { { 0, 0, "Layer", Mu::Model::LayerType::CheckBox, true, false } } };
        QVERIFY(Mu::Model::isValidLayersResponse(layers));
        QVERIFY(Mu::Model::isValidLayersResponse({ 7, { } }));
    }

    void malformedLayers_data()
    {
        QTest::addColumn<int>("scenario");
        const std::array names { "zero-generation", "invalid-id", "negative-depth", "missing-parent", "unknown-type",
                                 "embedded-nul",    "name-limit", "entry-limit",    "text-limit" };
        for (std::size_t i = 0; i < names.size(); ++i)
            QTest::newRow(names[i]) << static_cast<int>(i);
    }

    void malformedLayers()
    {
        using namespace ::Mu;
        QFETCH(int, scenario);
        Model::LayersResponse layers { 1, { { 0, 0, "Layer", Model::LayerType::CheckBox, true, false } } };
        auto& entry = layers.entries[0];
        switch (scenario) {
        case 0:
            layers.generation = 0;
            break;
        case 1:
            entry.id = 1;
            break;
        case 2:
            entry.depth = -1;
            break;
        case 3:
            entry.depth = 1;
            break;
        case 4:
            entry.type = static_cast<Model::LayerType>(255);
            break;
        case 5:
            entry.name = std::string("a\0b", 3);
            break;
        case 6:
            entry.name.assign(Limit::MaxLayerNameBytes + 1, 'x');
            break;
        case 7:
            layers.entries.resize(Limit::MaxLayerEntries + 1, entry);
            for (std::size_t i = 0; i < layers.entries.size(); ++i)
                layers.entries[i].id = static_cast<std::int32_t>(i);
            break;
        case 8:
            entry.name.assign(Limit::MaxLayerNameBytes, 'x');
            layers.entries.resize(Limit::MaxLayerTextBytes / Limit::MaxLayerNameBytes + 1, entry);
            for (std::size_t i = 0; i < layers.entries.size(); ++i)
                layers.entries[i].id = static_cast<std::int32_t>(i);
            break;
        }
        std::string error;
        const auto bytes = IPC::ZppCodec::encode(Model::ResponseMessage { 1, layers, std::nullopt }, &error);
        QVERIFY2(bytes.has_value(), error.c_str());
        Model::ResponseMessage decoded;
        QVERIFY2(IPC::ZppCodec::decode(*bytes, &decoded, &error), error.c_str());
        const auto* response = std::get_if<Model::LayersResponse>(&decoded.payload);
        QVERIFY(response);
        QVERIFY(!Model::isValidLayersResponse(*response));
    }
};

QTEST_GUILESS_MAIN(TestLayerResponse)
#include "test_layer_response.moc"
