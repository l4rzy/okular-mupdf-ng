// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/layers_model.hpp"

#include <KConfigGroup>
#include <KSharedConfig>
#include <QAbstractItemModelTester>
#include <QMimeDatabase>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <okular/core/document.h>
#include <okular/core/page.h>
#include <okular/core/settings_core.h>

using Mu::Generator::LayersModel;
using namespace Mu::Model;

namespace {

LayersResponse fixture()
{
    return { 1,
             { { 0, 0, "Parent", LayerType::CheckBox, true, false },
               { 1, 1, "Child", LayerType::CheckBox, false, false },
               { 2, 0, "Choices", LayerType::Label, false, false },
               { 3, 1, "A", LayerType::RadioButton, true, false },
               { 4, 1, "B", LayerType::RadioButton, false, false },
               { 5, 0, "Locked", LayerType::CheckBox, true, true } } };
}

std::optional<LayersResponse> setLayer(LayersResponse& state, const SetLayerRequest& request)
{
    if (request.generation != state.generation)
        return std::nullopt;
    if (request.selected && (request.id == 3 || request.id == 4)) {
        state.entries[3].selected = false;
        state.entries[4].selected = false;
    }
    state.entries[static_cast<std::size_t>(request.id)].selected = request.selected;
    return state;
}

} // namespace

class TestLayersModel : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_root;

private slots:

    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        qputenv("XDG_CONFIG_HOME", m_root.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", m_root.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", m_root.filePath("cache").toUtf8());
        // Container runners may lack sandbox features required by Strict mode.
        // This suite needs the real fixture and its native text, not the gate page.
        const auto config = KSharedConfig::openConfig(QStringLiteral("okular-mupdf-ngrc"));
        KConfigGroup general(config, QStringLiteral("General"));
        general.writeEntry("SandboxEnforcement", "Relaxed");
        KConfigGroup ocr(config, QStringLiteral("OCR"));
        ocr.writeEntry("OcrTriggerMode", "Never");
        config->sync();
        // Select this build's backend and resolve its worker through PATH,
        // including Release builds without an embedded build-tree worker path.
        QCoreApplication::setLibraryPaths({ QStringLiteral(TEST_PLUGIN_ROOT) });
        qputenv("PATH", QByteArray(TEST_WORKER_DIR) + ':' + qgetenv("PATH"));
        Okular::SettingsCore::instance(QStringLiteral("mupdfng-layer-test"));
    }

    void clearsLayerSearchHighlights_data()
    {
        QTest::addColumn<int>("searchId");
        QTest::newRow("find bar") << PART_SEARCH_ID;
        QTest::newRow("page view") << PAGEVIEW_SEARCH_ID;
        QTest::newRow("sidebar") << SW_SEARCH_ID;
        QTest::newRow("presentation") << PRESENTATION_SEARCH_ID;
    }

    void clearsLayerSearchHighlights()
    {
        QFETCH(int, searchId);
        Okular::Document document(nullptr);
        const QString path = QStringLiteral(TEST_LAYER_PDF);
        QCOMPARE(document.openDocument(path, QUrl::fromLocalFile(path), QMimeDatabase().mimeTypeForFile(path)),
                 Okular::Document::OpenSuccess);
        QVERIFY(document.metaData(QStringLiteral("GeneratorExtraDescription"), { })
                    .toString()
                    .contains(QStringLiteral("MuPDF")));
        QVERIFY(document.layersModel());
        QSignalSpy finished(&document, &Okular::Document::searchFinished);
        document.searchText(
            searchId, QStringLiteral("RED"), true, Qt::CaseSensitive, Okular::Document::AllDocument, false, Qt::yellow);
        QTRY_VERIFY(!finished.isEmpty());
        QCOMPARE(finished.last().at(1).value<Okular::Document::SearchStatus>(), Okular::Document::MatchFound);
        QVERIFY(document.page(0)->hasHighlights(searchId));
        Mu::Generator::resetLayerSearches(document);
        QVERIFY(!document.page(0)->hasHighlights(searchId));
        document.closeDocument();
    }

    void hierarchyAndFlags()
    {
        LayersModel model([](const SetLayerRequest&) { return std::optional<LayersResponse> { }; });
        QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);
        QVERIFY(model.resetLayers(fixture()));
        QCOMPARE(model.rowCount(), 3);
        QCOMPARE(model.rowCount(model.index(0, 0)), 1);
        QCOMPARE(model.rowCount(model.index(1, 0)), 2);
        QCOMPARE(model.index(0, 0, model.index(0, 0)).data().toString(), QStringLiteral("Child"));
        QVERIFY(model.index(0, 0).flags().testFlag(Qt::ItemIsUserCheckable));
        QVERIFY(!model.index(1, 0).flags().testFlag(Qt::ItemIsUserCheckable));
        QVERIFY(!model.index(2, 0).flags().testFlag(Qt::ItemIsUserCheckable));
        QCOMPARE(model.index(2, 0).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        QVERIFY(model.isDefaultVisibility());
        model.clearLayers();
        QCOMPARE(model.rowCount(), 0);
    }

    void rejectsChanges_data()
    {
        QTest::addColumn<int>("row");
        QTest::addColumn<int>("state");
        QTest::newRow("label") << 1 << int(Qt::Checked);
        QTest::newRow("locked") << 2 << int(Qt::Unchecked);
        QTest::newRow("partial") << 0 << int(Qt::PartiallyChecked);
        QTest::newRow("invalid index") << 8 << int(Qt::Checked);
    }

    void rejectsChanges()
    {
        QFETCH(int, row);
        QFETCH(int, state);
        int calls = 0;
        LayersModel model([&](const SetLayerRequest&) {
            ++calls;
            return std::optional<LayersResponse> { };
        });
        QVERIFY(model.resetLayers(fixture()));
        QVERIFY(!model.setData(model.index(row, 0), state, Qt::CheckStateRole));
        QCOMPARE(calls, 0);
        QVERIFY(model.isDefaultVisibility());
    }

    void radioUpdatesArePublishedTogether()
    {
        auto backend = fixture();
        LayersModel model([&](const SetLayerRequest& request) { return setLayer(backend, request); });
        QVERIFY(model.resetLayers(backend));
        const auto parent = model.index(1, 0);
        const auto a = model.index(0, 0, parent);
        const auto b = model.index(1, 0, parent);
        QSignalSpy visibility(&model, &LayersModel::visibilityChanged);
        connect(&model, &QAbstractItemModel::dataChanged, this, [&] {
            QCOMPARE(a.data(Qt::CheckStateRole).toInt(), int(Qt::Unchecked));
            QCOMPARE(b.data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
        });
        QVERIFY(model.setData(b, Qt::Checked, Qt::CheckStateRole));
        QCOMPARE(visibility.count(), 1);
        QVERIFY(!model.isDefaultVisibility());
    }

    void failedChangePreservesState()
    {
        LayersModel model([](const SetLayerRequest&) { return std::optional<LayersResponse> { }; });
        QVERIFY(model.resetLayers(fixture()));
        QSignalSpy failed(&model, &LayersModel::changeFailed);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        QVERIFY(!model.setData(model.index(0, 0), Qt::Unchecked, Qt::CheckStateRole));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(changed.count(), 0);
        QVERIFY(model.isDefaultVisibility());
        QCOMPARE(model.index(0, 0).data(Qt::CheckStateRole).toInt(), int(Qt::Checked));
    }

    void recoveryReplaysSelection()
    {
        auto backend = fixture();
        LayersModel model([&](const SetLayerRequest& request) { return setLayer(backend, request); });
        QVERIFY(model.resetLayers(backend));
        QVERIFY(model.setData(model.index(1, 0, model.index(1, 0)), Qt::Checked, Qt::CheckStateRole));
        backend = fixture();
        backend.generation = 2;
        const auto restore = [&](const SetLayerRequest& request) {
            return setLayer(backend, request);
        };
        QVERIFY(model.restoreLayers(backend, restore));
        QVERIFY(!backend.entries[3].selected);
        QVERIFY(backend.entries[4].selected);
        auto different = backend;
        different.entries[0].name = "Different document";
        QVERIFY(!model.restoreLayers(different, restore));
        QVERIFY(model.setData(model.index(0, 0), Qt::Unchecked, Qt::CheckStateRole));
        QVERIFY(!backend.entries[0].selected);
    }

    void rejectsMalformedHierarchy()
    {
        LayersModel model([](const SetLayerRequest&) { return std::optional<LayersResponse> { }; });
        auto response = fixture();
        response.entries[1].depth = 3;
        QVERIFY(!model.resetLayers(response));
        QCOMPARE(model.rowCount(), 0);
    }
};

QTEST_MAIN(TestLayersModel)
#include "test_layers_model.moc"
