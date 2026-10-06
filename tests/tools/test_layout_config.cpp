// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tools/cli/layout_config.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#ifdef HAVE_KDE_CONFIG
#include "generator/config/settings.hpp"
#include "mupdfngsettings.h"
#include <KConfig>
#endif

namespace Cli = Mu::Tools::Cli;
namespace Model = Mu::Model;

class TestLayoutConfig : public QObject {
    Q_OBJECT

    static void compareSettings(const Model::DocumentSettings& actual, const Model::DocumentSettings& expected)
    {
        QCOMPARE(actual.graphicsAntialiasing, expected.graphicsAntialiasing);
        QCOMPARE(actual.textAntialiasing, expected.textAntialiasing);
        QCOMPARE(actual.imageQuality, expected.imageQuality);
        QCOMPARE(actual.interpolateImages, expected.interpolateImages);
        QCOMPARE(actual.memoryCacheBytes, expected.memoryCacheBytes);
        QCOMPARE(actual.idleTrimAggressiveness, expected.idleTrimAggressiveness);
        QCOMPARE(actual.paperColorRgb, expected.paperColorRgb);
        QCOMPARE(actual.epub.fontSize, expected.epub.fontSize);
        QCOMPARE(actual.epub.fontFamily, expected.epub.fontFamily);
        QCOMPARE(actual.epub.pageSize, expected.epub.pageSize);
        QCOMPARE(actual.epub.customCssBase64, expected.epub.customCssBase64);
        QCOMPARE(actual.formJavaScriptEnabled, expected.formJavaScriptEnabled);
        QCOMPARE(actual.overprintSimulation, expected.overprintSimulation);
    }

    static bool writeFile(const QString& path, const QByteArray& contents)
    {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
    }

private slots:

    void readsSettings_data()
    {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<int>("fontSize");
        QTest::addColumn<int>("pageSize");
        QTest::newRow("defaults") << QByteArray() << 11 << int(Model::EpubPageSize::A5);
        const QStringList names { "A5", "SixByNine", "B5", "Letter" };
        const Model::EpubPageSize pages[] { Model::EpubPageSize::A5,
                                            Model::EpubPageSize::SixByNine,
                                            Model::EpubPageSize::B5,
                                            Model::EpubPageSize::Letter };
        for (int i = 0; i < 4; ++i) {
            QTest::newRow(qPrintable(names[i]))
                << ("[EPUB]\nEpubPageSize=" + names[i].toUtf8() + "\nEpubFontSize=14\n") << 14 << int(pages[i]);
            QTest::newRow(qPrintable(QStringLiteral("numeric-%1").arg(i)))
                << ("[EPUB]\nEpubPageSize=" + QByteArray::number(i) + "\n") << 11 << int(pages[i]);
        }
        QTest::newRow("clamp-low") << QByteArray("[EPUB]\nEpubFontSize=-4\n") << 10 << int(Model::EpubPageSize::A5);
        QTest::newRow("clamp-high") << QByteArray("[EPUB]\nEpubFontSize=999\n") << 20 << int(Model::EpubPageSize::A5);
        QTest::newRow("invalid") << QByteArray(
            "[EPUB]\nEpubFontSize=bad\nEpubPageSize=-1\nEpubFontFamily=unknown\nEpubCustomCss=bad!\n[Rendering]"
            "\nGraphicsAntialiasingBits=99\nTextAntialiasingBits=bad\nImageRenderingQuality=-1\n[General]\nMemoryLimit="
            "-1\nIdleTrimLevel=99\n")
                                 << 11 << int(Model::EpubPageSize::A5);
        QTest::newRow("all-settings") << QByteArray(
            "[EPUB]\nEpubFontSize=18\nEpubPageSize=letter\nEpubFontFamily=Monospace\nEpubCustomCss="
            "Ym9keSB7IGNvbG9yOiByZWQ7IH0=\n[Rendering]\nGraphicsAntialiasingBits=High\nTextAntialiasingBits="
            "Disabled\nImageRenderingQuality=Quality\nImageInterpolation=off\nOverprintSimulation=yes\n[General]"
            "\nMemoryLimit=Size128MiB\nIdleTrimLevel=Aggressive\nPdfFormJavaScriptEnabled=true\n")
                                      << 18 << int(Model::EpubPageSize::Letter);
        QTest::newRow("invalid-bool") << QByteArray("[Rendering]\nImageInterpolation=anything\nOverprintSimulation=0\n")
                                      << 11 << int(Model::EpubPageSize::A5);
        QTest::newRow("oversize-css") << ("[EPUB]\nEpubCustomCss=" + QByteArray(2000, 'a').toBase64() + "\n") << 11
                                      << int(Model::EpubPageSize::A5);
        const QStringList families { "Default", "Serif", "SansSerif", "Monospace" };
        const QStringList memory { "Size32MiB", "Size64MiB", "Size128MiB", "Size256MiB" };
        const QStringList trim { "Off", "Conservative", "Balanced", "Aggressive" };
        for (int i = 0; i < 4; ++i) {
            QTest::newRow(qPrintable(QStringLiteral("choices-%1").arg(i)))
                << ("[EPUB]\nEpubFontFamily=" + families[i].toUtf8() + "\n[General]\nMemoryLimit=" + memory[i].toUtf8()
                    + "\nIdleTrimLevel=" + trim[i].toUtf8() + "\n")
                << 11 << int(Model::EpubPageSize::A5);
        }
        const QStringList aa { "Disabled", "Minimum", "Low", "Medium", "High" };
        for (int i = 0; i < aa.size(); ++i) {
            QTest::newRow(qPrintable(QStringLiteral("aa-%1").arg(i)))
                << ("[Rendering]\nGraphicsAntialiasingBits=" + aa[i].toUtf8()
                    + "\nTextAntialiasingBits=" + QByteArray::number(i) + "\n")
                << 11 << int(Model::EpubPageSize::A5);
        }
    }

    void readsSettings()
    {
        QFETCH(QByteArray, contents);
        QFETCH(int, fontSize);
        QFETCH(int, pageSize);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("config");
        QVERIFY(writeFile(path, contents));
        const auto actual = Cli::readLayoutConfig({ path });
        QCOMPARE(actual.epub.fontSize, fontSize);
        QCOMPARE(int(actual.epub.pageSize), pageSize);
#ifdef HAVE_KDE_CONFIG
        // Read through the actual generated singleton, not a duplicate mapping.
        MuPDFNGSettings::self()->setConfig(std::make_unique<KConfig>(path, KConfig::SimpleConfig));
        Mu::Generator::Config::reloadSettings();
        compareSettings(actual, Mu::Generator::Config::readWorkerSettings().documentSettings(0xFFFFFF));
#endif
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), contents);
    }

    void readsQuotedIniValues()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("config");
        const QByteArray contents("[EPUB]\nEpubPageSize=\"Letter\"\nEpubCustomCss=\"YWJj\"\n");
        QVERIFY(writeFile(path, contents));
        const auto actual = Cli::readLayoutConfig({ path });
        QCOMPARE(actual.epub.pageSize, Model::EpubPageSize::Letter);
        QCOMPARE(actual.epub.customCssBase64, std::string("YWJj"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), contents);
    }

    void readsWorkerSettings()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("config");
        QVERIFY(writeFile(path,
                          "[Rendering]\nGraphicsAntialiasingBits=Disabled\nTextAntialiasingBits=Low\n"
                          "ImageRenderingQuality=Quality\nImageInterpolation=false\nOverprintSimulation=true\n"
                          "[General]\nMemoryLimit=Size256MiB\nIdleTrimLevel=Off\nPdfFormJavaScriptEnabled=true\n"
                          "[EPUB]\nEpubFontFamily=Monospace\n"));
        Model::DocumentSettings expected;
        expected.graphicsAntialiasing = 0;
        expected.textAntialiasing = 4;
        expected.imageQuality = 2;
        expected.interpolateImages = false;
        expected.overprintSimulation = true;
        expected.memoryCacheBytes = 256LL * 1024 * 1024;
        expected.idleTrimAggressiveness = Model::IdleTrimLevel::Off;
        expected.epub.pageSize = Model::EpubPageSize::A5;
        expected.epub.fontFamily = Model::EpubFontFamily::Monospace;
#ifdef MU_WORKER_ENABLE_FORM_JAVASCRIPT
        expected.formJavaScriptEnabled = true;
#endif
        compareSettings(Cli::readLayoutConfig({ path }), expected);
    }

    void readsMissingFiles()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("missing/config");
        compareSettings(Cli::readLayoutConfig({ path }), Cli::readLayoutConfig(QStringList { }));
        QVERIFY(!QFile::exists(path));
        QVERIFY(!QDir(directory.filePath("missing")).exists());
    }

    void readsLayeredSettings_data()
    {
        QTest::addColumn<QByteArray>("base");
        QTest::addColumn<QByteArray>("override");
        QTest::addColumn<int>("expectedSize");
        QTest::newRow("override") << QByteArray("[EPUB]\nEpubFontSize=13\nEpubFontFamily=Serif\n")
                                  << QByteArray("[EPUB]\nEpubFontSize=18\n") << 18;
        QTest::newRow("inherited") << QByteArray("[EPUB]\nEpubFontSize=13\nEpubFontFamily=Serif\n")
                                   << QByteArray("[EPUB]\nEpubPageSize=Letter\n") << 13;
        QTest::newRow("empty-override") << QByteArray("[EPUB]\nEpubFontSize=13\n") << QByteArray() << 13;
    }

    void readsLayeredSettings()
    {
        QFETCH(QByteArray, base);
        QFETCH(QByteArray, override);
        QFETCH(int, expectedSize);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto first = directory.filePath("base"), second = directory.filePath("override");
        QVERIFY(writeFile(first, base));
        QVERIFY(writeFile(second, override));
        const auto actual = Cli::readLayoutConfig({ first, second });
        QCOMPARE(actual.epub.fontSize, expectedSize);
#ifdef HAVE_KDE_CONFIG
        auto config = std::make_unique<KConfig>(second, KConfig::NoGlobals);
        config->addConfigSources({ first });
        MuPDFNGSettings::self()->setConfig(std::move(config));
        Mu::Generator::Config::reloadSettings();
        compareSettings(actual, Mu::Generator::Config::readWorkerSettings().documentSettings(0xFFFFFF));
#endif
    }

    void xdgPrecedence()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto user = directory.filePath("user"), system = directory.filePath("system");
        QVERIFY(QDir().mkpath(user));
        QVERIFY(QDir().mkpath(system));
        QVERIFY(writeFile(system + "/kdeglobals", "[EPUB]\nEpubFontSize=13\nEpubFontFamily=Monospace\n"));
        QVERIFY(writeFile(system + "/okular-mupdf-ngrc", "[EPUB]\nEpubFontSize=14\nEpubPageSize=Letter\n"));
        QVERIFY(writeFile(user + "/kdeglobals", "[EPUB]\nEpubFontSize=15\n"));
        QVERIFY(writeFile(user + "/okular-mupdf-ngrc",
                          "[EPUB]\nEpubPageSize=A5\nEpubCustomCss=Ym9keSB7IGNvbG9yOiByZWQ7IH0=\n"));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_CONFIG_HOME", user);
        environment.insert("XDG_CONFIG_DIRS", system);
        environment.insert("MUPDFNG_TEST_CONFIG_PROBE", "1");
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(), { "readsXdgConfig" });
        QVERIFY(child.waitForFinished());
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QVERIFY2(child.exitCode() == 0, child.readAllStandardOutput().constData());
    }

    void readsXdgConfig()
    {
        if (!qEnvironmentVariableIsSet("MUPDFNG_TEST_CONFIG_PROBE"))
            QSKIP("Invoked in a child with isolated XDG paths");
        const auto actual = Cli::readLayoutConfig();
        QCOMPARE(actual.epub.fontSize, 15);
        QCOMPARE(actual.epub.pageSize, Model::EpubPageSize::A5);
        QCOMPARE(actual.epub.fontFamily, Model::EpubFontFamily::Monospace);
        QCOMPARE(actual.epub.customCssBase64, std::string("Ym9keSB7IGNvbG9yOiByZWQ7IH0="));
#ifdef HAVE_KDE_CONFIG
        Mu::Generator::Config::reloadSettings();
        compareSettings(actual, Mu::Generator::Config::readWorkerSettings().documentSettings(0xFFFFFF));
#endif
    }
};

QTEST_GUILESS_MAIN(TestLayoutConfig)
#include "test_layout_config.moc"
