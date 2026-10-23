// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/epub/document.hpp"
#include "engine/mobi/document.hpp"
#include "engine/pdf/document.hpp"
#include "engine/theming/device.hpp"
#include "engine/theming/palette.hpp"
#include "genpdf.hpp"
#include "shared/model/theme_colors.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <unistd.h>

extern "C" {
#include <mupdf/pdf.h>
}

namespace {

bool openDocument(Mu::Worker::Engine::PdfDocument& document, const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    std::string error;
    if (!document.openFd(::dup(file.handle()), path.toStdString(), &error))
        return false;
    return true;
}

bool createShadingPdf(fz_context* ctx, const QString& path, int kind)
{
    const auto filename = QFile::encodeName(path);
    const QByteArray commands = kind ? QByteArray("/Pattern cs /P scn 0 0 64 64 re f")
                                     : QByteArray("1 g 0 0 64 64 re f q 0 0 64 24 re W n /S sh Q");
    const QByteArray patternCommands = kind == 3
        ? QByteArray("q 40 0 0 40 0 0 cm BI /W 1 /H 1 /CS /RGB /BPC 8 /F /AHx ID ff0000> EI Q")
        : kind == 1 ? QByteArray("1 g 0 0 40 40 re f 0 g 0 0 20 20 re f")
                    : QByteArray("1 g 0 0 40 40 re f q 0 0 40 20 re W n /S sh Q");
    pdf_document* volatile document = nullptr;
    pdf_obj* volatile resources = nullptr;
    pdf_obj* volatile pattern = nullptr;
    pdf_obj* volatile page = nullptr;
    fz_buffer* volatile contents = nullptr;
    bool success = false;
    fz_var(success);
    fz_try(ctx)
    {
        document = pdf_create_document(ctx);
        resources = pdf_new_dict(ctx, document, 2);
        const auto setArray = [&](const char* key, std::initializer_list<float> values) {
            auto* array = pdf_new_array(ctx, document, static_cast<int>(values.size()));
            pdf_dict_putp_drop(ctx, resources, key, array);
            for (float value : values)
                pdf_array_push_real(ctx, array, value);
        };
        pdf_dict_putp_drop(ctx, resources, "Shading/S/ShadingType", pdf_new_int(ctx, 2));
        pdf_dict_putp(ctx, resources, "Shading/S/ColorSpace", PDF_NAME(DeviceGray));
        setArray("Shading/S/Coords", { 0, 0, 64, 0 });
        pdf_dict_putp_drop(ctx, resources, "Shading/S/Function/FunctionType", pdf_new_int(ctx, 2));
        pdf_dict_putp_drop(ctx, resources, "Shading/S/Function/N", pdf_new_int(ctx, 1));
        setArray("Shading/S/Function/Domain", { 0, 1 });
        setArray("Shading/S/Function/C0", { 1 });
        setArray("Shading/S/Function/C1", { 0 });
        if (kind) {
            pattern = pdf_new_dict(ctx, document, 8);
            pdf_dict_put_int(ctx, pattern, PDF_NAME(PatternType), 1);
            pdf_dict_put_int(ctx, pattern, PDF_NAME(PaintType), 1);
            pdf_dict_put_int(ctx, pattern, PDF_NAME(TilingType), 1);
            pdf_dict_put_int(ctx, pattern, PDF_NAME(XStep), 40);
            pdf_dict_put_int(ctx, pattern, PDF_NAME(YStep), 40);
            pdf_dict_put_rect(ctx, pattern, PDF_NAME(BBox), { 0, 0, 40, 40 });
            auto* patternResources = pdf_dict_put_dict(ctx, pattern, PDF_NAME(Resources), 1);
            pdf_dict_put(ctx, patternResources, PDF_NAME(Shading), pdf_dict_get(ctx, resources, PDF_NAME(Shading)));
            contents =
                fz_new_buffer_from_copied_data(ctx,
                                               reinterpret_cast<const unsigned char*>(patternCommands.constData()),
                                               std::size_t(patternCommands.size()));
            pdf_dict_putp_drop(ctx, resources, "Pattern/P", pdf_add_stream(ctx, document, contents, pattern, 0));
            fz_drop_buffer(ctx, contents);
            contents = nullptr;
        }
        contents = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(commands.constData()), std::size_t(commands.size()));
        page = pdf_add_page(ctx, document, { 0, 0, 64, 64 }, 0, resources, contents);
        pdf_insert_page(ctx, document, -1, page);
        pdf_save_document(ctx, document, filename.constData(), &pdf_default_write_options);
        success = true;
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, contents);
        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, pattern);
        pdf_drop_obj(ctx, resources);
        pdf_drop_document(ctx, document);
    }
    fz_catch(ctx)
    {
    }
    return success;
}

bool createBlendPdf(fz_context* ctx, const QString& path, const QByteArray& blend, float alpha, bool knockout)
{
    pdf_document* volatile document = nullptr;
    pdf_obj* volatile resources = nullptr;
    pdf_obj* volatile page = nullptr;
    pdf_obj* volatile form = nullptr;
    fz_buffer* volatile contents = nullptr;
    bool success = false;
    fz_var(success);
    fz_try(ctx)
    {
        document = pdf_create_document(ctx);
        resources = pdf_new_dict(ctx, document, 1);
        pdf_dict_putp_drop(ctx, resources, "ExtGState/B/BM", pdf_new_name(ctx, blend.constData()));
        pdf_dict_putp_drop(ctx, resources, "ExtGState/B/ca", pdf_new_real(ctx, alpha));
        constexpr char commands[] = "1 g 0 0 64 64 re f q /B gs 16 16 16 16 re f Q";
        contents =
            fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(commands), sizeof(commands) - 1);
        if (knockout) {
            form = pdf_new_dict(ctx, document, 5);
            pdf_dict_put(ctx, form, PDF_NAME(Type), PDF_NAME(XObject));
            pdf_dict_put(ctx, form, PDF_NAME(Subtype), PDF_NAME(Form));
            pdf_dict_put_rect(ctx, form, PDF_NAME(BBox), { 0, 0, 64, 64 });
            pdf_dict_putp_drop(ctx, form, "Group/S", pdf_new_name(ctx, "Transparency"));
            pdf_dict_putp_drop(ctx, form, "Group/CS", pdf_new_name(ctx, "DeviceRGB"));
            pdf_dict_putp(ctx, form, "Group/K", PDF_TRUE);
            pdf_dict_putp(ctx, form, "Group/I", PDF_TRUE);
            auto* formResources = pdf_dict_put_dict(ctx, form, PDF_NAME(Resources), 1);
            pdf_dict_put(ctx, formResources, PDF_NAME(ExtGState), pdf_dict_get(ctx, resources, PDF_NAME(ExtGState)));
            pdf_dict_putp_drop(ctx, resources, "XObject/F", pdf_add_stream(ctx, document, contents, form, 0));
            fz_drop_buffer(ctx, contents);
            contents = nullptr;
            contents = fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>("/F Do"), 5);
        }
        page = pdf_add_page(ctx, document, { 0, 0, 64, 64 }, 0, resources, contents);
        pdf_insert_page(ctx, document, -1, page);
        pdf_save_document(ctx, document, QFile::encodeName(path).constData(), &pdf_default_write_options);
        success = true;
    }
    fz_always(ctx)
    {
        fz_drop_buffer(ctx, contents);
        pdf_drop_obj(ctx, form);
        pdf_drop_obj(ctx, page);
        pdf_drop_obj(ctx, resources);
        pdf_drop_document(ctx, document);
    }
    fz_catch(ctx)
    {
    }
    return success;
}

} // namespace

class TestPage : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_tempDir;
    QString m_multiPagePath;
    QString m_singlePagePath;
    QString m_textPath;

private slots:

    void plansThemeRendering()
    {
        using Mu::Model::DocumentTheme;
        using Mu::Worker::Engine::ThemePageInfo;
        using Mu::Worker::Engine::ThemeRenderPlan;
        using namespace Mu::Model::ThemeColor;

        const struct {
            DocumentTheme theme;
            ThemePageInfo page;
            ThemeRenderPlan expected;
        } cases[] {
            { DocumentTheme::Default, { }, { false, false, 0x123456 } },
            { DocumentTheme::Twilight, { }, { false, false, DefaultBackground } },
            { DocumentTheme::Dusk, { }, { true, true, DuskBackground } },
            { DocumentTheme::Dusk, { true, false }, { true, false, DefaultBackground } },
            { DocumentTheme::Dark, { }, { true, true, DarkBackground } },
            { DocumentTheme::Dark, { false, true }, { false, true, DefaultBackground } },
            { DocumentTheme::Dark, { true, true }, { true, false, DefaultBackground } },
        };

        for (const auto& test : cases) {
            Mu::Model::DocumentSettings settings;
            settings.documentTheme = test.theme;
            settings.paperColorRgb = 0x123456;
            const auto plan = Mu::Worker::Engine::planThemeRendering(settings, test.page);
            QCOMPARE(plan.useDevice, test.expected.useDevice);
            QCOMPARE(plan.recolorVectors, test.expected.recolorVectors);
            QCOMPARE(plan.paperColor, test.expected.paperColor);
        }
    }

    void dimsImageRows_data()
    {
        QTest::addColumn<int>("width");
        QTest::newRow("empty") << 0;
        QTest::newRow("single-pixel") << 1;
        QTest::newRow("short-tail") << 3;
        QTest::newRow("vector-sized") << 16;
        QTest::newRow("all-byte-values-with-tail") << 257;
    }

    void dimsImageRows()
    {
        QFETCH(int, width);
        QByteArray pixels(width * 4 + 2, char(0xAB));
        for (int i = 0; i < width * 4; ++i)
            pixels[i + 1] = static_cast<char>((i / 4 + (i % 4) * 67) % 256);
        const auto original = pixels;
        // Start unaligned and guard both ends, including the scalar tail.
        Mu::Worker::Engine::dimImageRow(reinterpret_cast<unsigned char*>(pixels.data() + 1), width);
        QCOMPARE(pixels.front(), char(0xAB));
        QCOMPARE(pixels.back(), char(0xAB));
        for (int i = 0; i < width * 4; ++i) {
            const auto value = static_cast<unsigned char>(original[i + 1]);
            const auto expected = i % 4 == 3 ? value : (unsigned(value) * 4 + 2) / 5;
            QCOMPARE(static_cast<unsigned char>(pixels[i + 1]), expected);
        }
    }

    void themesImages_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QByteArray>("rgba");
        QTest::addColumn<bool>("softMask");
        QTest::addColumn<bool>("preserved");
        QTest::newRow("dusk-opaque") << 2 << QByteArray::fromHex("ff0000ff") << false << false;
        QTest::newRow("dusk-transparent") << 2 << QByteArray::fromHex("80000080") << false << false;
        QTest::newRow("dusk-luminosity-mask") << 2 << QByteArray::fromHex("ff0000ff") << true << false;
        QTest::newRow("dark-dark-image") << 3 << QByteArray::fromHex("101010ff") << false << true;
        QTest::newRow("dark-transparent-dark-image") << 3 << QByteArray::fromHex("08080880") << false << true;
        QTest::newRow("dark-light-image") << 3 << QByteArray::fromHex("ffffffff") << false << false;
        QTest::newRow("dark-transparent-light-image") << 3 << QByteArray::fromHex("80808080") << false << false;
        QTest::newRow("dark-transparent-padding")
            << 3 << QByteArray::fromHex("000000000000000000000000ffffffff") << false << false;
        QTest::newRow("dark-fully-transparent") << 3 << QByteArray::fromHex("00000000") << false << false;
        QTest::newRow("dark-medium-image") << 3 << QByteArray::fromHex("808080ff") << false << true;
        QTest::newRow("dark-transparent-medium-image") << 3 << QByteArray::fromHex("40404080") << false << true;
        QTest::newRow("dark-below-threshold") << 3 << QByteArray::fromHex("9f9f9fff") << false << true;
        QTest::newRow("dark-at-threshold") << 3 << QByteArray::fromHex("a0a0a0ff") << false << false;
        QTest::newRow("dark-luminosity-mask") << 3 << QByteArray::fromHex("ffffffff") << true << false;
    }

    void themesImages()
    {
        QFETCH(int, theme);
        QFETCH(QByteArray, rgba);
        QFETCH(bool, softMask);
        QFETCH(bool, preserved);
        using namespace Mu::Worker::Engine;
        PdfDocument owner;
        auto* ctx = owner.context();
        auto* source = fz_new_pixmap(ctx, fz_device_rgb(ctx), int(rgba.size() / 4), 1, nullptr, 1);
        std::copy(rgba.begin(), rgba.end(), fz_pixmap_samples(ctx, source));
        auto* image = fz_new_image_from_pixmap(ctx, source, nullptr);
        auto* path = fz_new_path(ctx);
        fz_rectto(ctx, path, 0, 0, 4, 4);
        std::array<QByteArray, 2> pixels;
        for (const bool themed : { false, true }) {
            auto* output = fz_new_pixmap(ctx, fz_device_rgb(ctx), 4, 4, nullptr, 1);
            fz_clear_pixmap(ctx, output);
            auto* target = fz_new_draw_device(ctx, fz_identity, output);
            auto* device = themed
                ? newThemeDevice(
                      ctx, target, fz_identity, { 0, 0, 4, 4 }, nullptr, static_cast<Mu::Model::DocumentTheme>(theme))
                : fz_keep_device(ctx, target);
            if (softMask) {
                const float black = 0, grey = 0.5f;
                fz_begin_mask(ctx, device, { 0, 0, 4, 4 }, 1, fz_device_gray(ctx), &black, fz_default_color_params);
                fz_fill_path(ctx, device, path, 0, fz_identity, fz_device_gray(ctx), &grey, 1, fz_default_color_params);
                fz_end_mask(ctx, device);
            }
            fz_fill_image(ctx, device, image, fz_scale(4, 4), 1, fz_default_color_params);
            if (softMask)
                fz_pop_clip(ctx, device);
            fz_close_device(ctx, device);
            pixels[themed] = QByteArray(reinterpret_cast<const char*>(fz_pixmap_samples(ctx, output)) + 3 * 4, 4);
            fz_drop_device(ctx, device);
            fz_drop_device(ctx, target);
            fz_drop_pixmap(ctx, output);
        }
        const auto sourcePixel = QByteArray(reinterpret_cast<const char*>(fz_pixmap_samples(ctx, source)), rgba.size());
        fz_drop_path(ctx, path);
        fz_drop_image(ctx, image);
        fz_drop_pixmap(ctx, source);

        auto expected = pixels[0];
        if (theme == int(Mu::Model::DocumentTheme::Dusk) || preserved) {
            dimImageRow(reinterpret_cast<unsigned char*>(expected.data()), 1);
        } else {
            auto* pixmap = fz_new_pixmap_with_data(
                ctx, fz_device_rgb(ctx), 1, 1, nullptr, 1, 4, reinterpret_cast<unsigned char*>(expected.data()));
            applyDocumentTheme(ctx, pixmap, Mu::Model::DocumentTheme::Dark, nullptr);
            fz_drop_pixmap(ctx, pixmap);
        }
        for (int channel = 0; channel < 3; ++channel)
            QVERIFY(std::abs(int(static_cast<unsigned char>(pixels[1][channel]))
                             - int(static_cast<unsigned char>(expected[channel])))
                    <= 1);
        QCOMPARE(pixels[1][3], pixels[0][3]);
        QCOMPARE(sourcePixel, rgba);
    }

    void themesClippedImages_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<bool>("rotated");
        QTest::addColumn<bool>("tiled");
        for (int theme : { 2, 3 })
            for (bool rotated : { false, true })
                for (bool tiled : { false, true })
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(theme).arg(rotated).arg(tiled)))
                        << theme << rotated << tiled;
    }

    void themesClippedImages()
    {
        QFETCH(int, theme);
        QFETCH(bool, rotated);
        QFETCH(bool, tiled);
        using namespace Mu::Worker::Engine;
        PdfDocument owner;
        auto* ctx = owner.context();
        auto* source = fz_new_pixmap(ctx, fz_device_rgb(ctx), 2, 2, nullptr, 1);
        const std::array<unsigned char, 4> shades { 48, 80, 112, 120 };
        for (std::size_t i = 0; i < shades.size(); ++i) {
            auto* pixel = fz_pixmap_samples(ctx, source) + i * 4;
            std::fill_n(pixel, 3, shades[i]);
            pixel[3] = 128;
        }
        auto* image = fz_new_image_from_pixmap(ctx, source, nullptr);
        auto* clip = fz_new_path(ctx);
        fz_rectto(ctx, clip, 4.25f, 4.25f, 11.75f, 11.75f);
        const auto transform = tiled ? fz_scale(2, 2) : fz_identity;
        const fz_matrix position = rotated ? fz_matrix { 0, 16, -16, 0, 16, 0 } : fz_scale(16, 16);
        const int size = tiled ? 32 : 16;
        const auto documentTheme = static_cast<Mu::Model::DocumentTheme>(theme);
        std::array<QByteArray, 2> pixels;
        int imageWidth = 0, imageHeight = 0;
        for (bool themed : { false, true }) {
            auto* output = fz_new_pixmap(ctx, fz_device_rgb(ctx), size, size, nullptr, 1);
            fz_clear_pixmap(ctx, output);
            auto* draw = fz_new_draw_device(ctx, transform, output);

            struct ImageProbe {
                fz_device super;
                int* width;
                int* height;
            };

            auto* probe =
                reinterpret_cast<ImageProbe*>(fz_new_passthrough_device_of_size(ctx, draw, sizeof(ImageProbe)));
            probe->width = &imageWidth;
            probe->height = &imageHeight;
            probe->super.fill_image = [](fz_context* context,
                                         fz_device* device,
                                         fz_image* sourceImage,
                                         fz_matrix ctm,
                                         float alpha,
                                         fz_color_params params) {
                auto* state = reinterpret_cast<ImageProbe*>(device);
                *state->width = sourceImage->w;
                *state->height = sourceImage->h;
                fz_fill_image(context, device->passthrough, sourceImage, ctm, alpha, params);
            };
            auto* device = themed
                ? newThemeDevice(ctx, &probe->super, transform, { 0, 0, size, size }, nullptr, documentTheme)
                : fz_keep_device(ctx, &probe->super);
            fz_clip_path(ctx, device, clip, 0, fz_identity, fz_infinite_rect);
            fz_fill_image(ctx, device, image, position, 1, fz_default_color_params);
            fz_pop_clip(ctx, device);
            fz_close_device(ctx, device);
            if (!themed) {
                if (theme == int(Mu::Model::DocumentTheme::Dusk))
                    dimImageRow(fz_pixmap_samples(ctx, output), size * size);
                else
                    applyDocumentTheme(ctx, output, documentTheme, nullptr);
            }
            pixels[themed] = QByteArray(reinterpret_cast<const char*>(fz_pixmap_samples(ctx, output)), size * size * 4);
            fz_drop_device(ctx, device);
            fz_drop_device(ctx, &probe->super);
            fz_drop_device(ctx, draw);
            fz_drop_pixmap(ctx, output);
        }
        fz_drop_path(ctx, clip);
        fz_drop_image(ctx, image);
        fz_drop_pixmap(ctx, source);
        QCOMPARE(imageWidth, size / 2);
        QCOMPARE(imageHeight, size / 2);
        for (qsizetype i = 0; i < pixels[0].size(); ++i)
            QVERIFY(
                std::abs(int(static_cast<unsigned char>(pixels[0][i])) - int(static_cast<unsigned char>(pixels[1][i])))
                <= 1);
    }

    void themeStencilAndCancellation_data()
    {
        QTest::addColumn<int>("theme");
        QTest::newRow("dusk") << 2;
        QTest::newRow("dark") << 3;
    }

    void themeStencilAndCancellation()
    {
        QFETCH(int, theme);
        using namespace Mu::Worker::Engine;
        PdfDocument owner;
        auto* ctx = owner.context();
        auto* pixmap = fz_new_pixmap(ctx, fz_device_rgb(ctx), 8, 8, nullptr, 1);
        fz_clear_pixmap_with_value(ctx, pixmap, 255);
        auto* stencil = fz_new_pixmap(ctx, nullptr, 2, 2, nullptr, 1);
        fz_clear_pixmap_with_value(ctx, stencil, 255);
        auto* image = fz_new_image_from_pixmap(ctx, stencil, nullptr);
        auto* target = fz_new_draw_device(ctx, fz_identity, pixmap);
        fz_cookie cookie { };
        auto* device = newThemeDevice(
            ctx, target, fz_identity, { 0, 0, 8, 8 }, &cookie, static_cast<Mu::Model::DocumentTheme>(theme));
        const float black = 0;
        fz_fill_image_mask(ctx, device, image, fz_scale(8, 8), fz_device_gray(ctx), &black, 1, fz_default_color_params);
        const auto pixel = QByteArray(reinterpret_cast<const char*>(fz_pixmap_samples(ctx, pixmap)), 4);
        cookie.abort = 1;
        int error = 0;
        fz_var(error);
        fz_try(ctx)
        {
            fz_fill_image_mask(
                ctx, device, image, fz_scale(8, 8), fz_device_gray(ctx), &black, 1, fz_default_color_params);
        }
        fz_catch(ctx)
        {
            error = fz_caught(ctx);
        }
        fz_drop_device(ctx, device);
        fz_drop_device(ctx, target);
        fz_drop_image(ctx, image);
        fz_drop_pixmap(ctx, stencil);
        fz_drop_pixmap(ctx, pixmap);
        QCOMPARE(pixel, QByteArray::fromHex("dadce0ff"));
        QCOMPARE(error, FZ_ERROR_ABORT);
    }

    void darkDocumentImages_data()
    {
        QTest::addColumn<bool>("preserved");
        QTest::addColumn<bool>("darkPage");
        QTest::newRow("mostly-dark-image") << true << false;
        QTest::newRow("mostly-light-image") << false << false;
        QTest::newRow("mostly-dark-page") << false << true;
    }

    void darkDocumentImages()
    {
        QFETCH(bool, preserved);
        QFETCH(bool, darkPage);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("dark-images.pdf");
        QByteArray contents = darkPage ? "0 g " : "1 g ";
        contents += "0 0 612 792 re f q 200 0 0 100 100 400 cm "
                    "BI /W 4 /H 1 /CS /RGB /BPC 8 /F /AHx ID ";
        contents += preserved ? "101010101010101010ffffff" : "ffffffffffffffffff101010";
        contents += "> EI Q 0 g BT /F1 80 Tf 110 470 Td (TEXT) Tj ET";
        createTextPDF(document.context(), path, contents.constData());
        QVERIFY(openDocument(document, path));
        constexpr int width = 153, height = 198, stride = width * 4;
        std::vector<std::uint8_t> original(height * stride), dark(height * stride);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, original.data(), stride));
        auto settings = document.settings();
        settings.documentTheme = Mu::Model::DocumentTheme::Dark;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, dark.data(), stride));
        QCOMPARE(dark[0], std::uint8_t(darkPage ? 218 : 32));
        for (const int x : { 30, 70 }) {
            const auto offset = std::size_t(85 * stride + x * 4);
            if (preserved) {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    QVERIFY(std::abs(int(dark[offset + channel]) - (int(original[offset + channel]) * 4 + 2) / 5) <= 1);
                QCOMPARE(dark[offset + 3], original[offset + 3]);
            } else
                QVERIFY(std::abs(int(dark[offset]) - int(original[offset])) > 100);
        }
        const std::array<std::uint8_t, 4> foreground { 218, 220, 224, 255 };
        if (!darkPage)
            QVERIFY(std::search(dark.begin(),
                                dark.end(),
                                foreground.begin(),
                                foreground.end(),
                                [](int actual, int expected) { return std::abs(actual - expected) <= 1; })
                    != dark.end());
        // This tile sees only the minority-colored part of the image.
        constexpr DocumentBase::RenderTile tile { 64, 80, 8, 8 };
        std::vector<std::uint8_t> tiled(tile.width * tile.height * 4);
        QVERIFY(document.renderToBuffer({ 0, width, height, tile }, tiled.data(), tile.width * 4));
        for (int y = 0; y < tile.height; ++y)
            for (int x = 0; x < tile.width * 4; ++x)
                QVERIFY(std::abs(int(tiled[std::size_t(y * tile.width * 4 + x)])
                                 - int(dark[std::size_t((y + tile.y) * stride + tile.x * 4 + x)]))
                        <= 2);
        settings.documentTheme = Mu::Model::DocumentTheme::Default;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, dark.data(), stride));
        QCOMPARE(dark, original);
    }

    void duskDocumentImages_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("image-with-text-overlay") << 0;
        QTest::newRow("scanned-page") << 1;
        QTest::newRow("mostly-dark-page") << 2;
    }

    void duskDocumentImages()
    {
        QFETCH(int, kind);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("dusk-images.pdf");
        if (kind == 1) {
            createScannedPDF(document.context(), m_textPath, path);
        } else {
            QByteArray contents = kind == 2 ? QByteArray("0 g 0 0 612 792 re f ") : QByteArray("1 g 0 0 612 792 re f ");
            contents += "q 200 0 0 200 100 400 cm BI /W 2 /H 2 /CS /RGB /BPC 8 /F /AHx ID "
                        "ff000000ff000000ffffffff> EI Q 0 g BT /F1 20 Tf 110 470 Td (IMAGE) Tj ET";
            createTextPDF(document.context(), path, contents.constData());
        }
        QVERIFY(openDocument(document, path));
        constexpr int width = 612, height = 792, stride = width * 4;
        std::vector<std::uint8_t> original(height * stride), dusk(height * stride);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, original.data(), stride));
        constexpr DocumentBase::RenderTile tile { 100, 290, 200, 100 };
        std::vector<std::uint8_t> nativeTile(tile.width * tile.height * 4);
        if (!kind)
            QVERIFY(document.renderToBuffer({ 0, width, height, tile }, nativeTile.data(), tile.width * 4));
        auto settings = document.settings();
        settings.documentTheme = Mu::Model::DocumentTheme::Dusk;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, dusk.data(), stride));
        if (kind) {
            // Scans dim as images. Dark-page vector marks stay unchanged;
            // this fixture's black vectors are unaffected by multiplication.
            for (std::size_t i = 0; i < original.size(); ++i) {
                if (i % 4 == 3)
                    QCOMPARE(dusk[i], original[i]);
                else
                    QVERIFY(std::abs(int(dusk[i]) - (int(original[i]) * 4 + 2) / 5) <= 2);
            }
            return;
        }
        QCOMPARE(dusk[0], std::uint8_t(48));
        QCOMPARE(dusk[1], std::uint8_t(49));
        QCOMPARE(dusk[2], std::uint8_t(52));
        const std::array<std::uint8_t, 4> foreground { 218, 220, 224, 255 };
        QVERIFY(std::search(dusk.begin(), dusk.end(), foreground.begin(), foreground.end()) != dusk.end());
        const auto verifyDimmedPixel = [](const auto& themed, const auto& native, std::size_t offset) {
            for (std::size_t channel = 0; channel < 3; ++channel)
                QVERIFY(std::abs(int(themed[offset + channel]) - (int(native[offset + channel]) * 4 + 2) / 5) <= 2);
            QCOMPARE(themed[offset + 3], native[offset + 3]);
        };
        // Sample the image away from text and interpolation edges.
        verifyDimmedPixel(dusk, original, (220 * width + 120) * 4);
        std::vector<std::uint8_t> tiled(tile.width * tile.height * 4);
        QVERIFY(document.renderToBuffer({ 0, width, height, tile }, tiled.data(), tile.width * 4));
        // Compare matching native tile requests: image sampling can differ
        // between full-page rendering and tiles.
        verifyDimmedPixel(tiled, nativeTile, (70 * tile.width + 20) * 4);
        settings.documentTheme = Mu::Model::DocumentTheme::Default;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, tile }, tiled.data(), tile.width * 4));
        QCOMPARE(tiled, nativeTile);
    }

    void clipsThemeShading_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<int>("scale");
        for (int theme : { 2, 3 })
            for (int scale : { 1, 2 })
                QTest::newRow(qPrintable(QStringLiteral("%1-%2").arg(theme).arg(scale))) << theme << scale;
    }

    void clipsThemeShading()
    {
        QFETCH(int, theme);
        QFETCH(int, scale);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("clipped-shading.pdf");
        QVERIFY(createShadingPdf(document.context(), path, 0));
        QVERIFY(openDocument(document, path));
        auto* ctx = document.context();
        auto* page = fz_load_page(ctx, document.document(), 0);
        QSize rasterSize;

        struct RasterProbe {
            fz_device super;
            QSize* size;
        };

        auto* probe = reinterpret_cast<RasterProbe*>(fz_new_device_of_size(ctx, sizeof(RasterProbe)));
        probe->size = &rasterSize;
        probe->super.fill_image =
            [](fz_context*, fz_device* device, fz_image* image, fz_matrix, float, fz_color_params) {
                *reinterpret_cast<RasterProbe*>(device)->size = QSize(image->w, image->h);
            };
        auto* device = newThemeDevice(ctx,
                                      &probe->super,
                                      fz_scale(float(scale), float(scale)),
                                      { 0, 0, 64 * scale, 64 * scale },
                                      nullptr,
                                      static_cast<Mu::Model::DocumentTheme>(theme));
        fz_run_page(ctx, page, device, fz_identity, nullptr);
        fz_close_device(ctx, device);
        fz_drop_device(ctx, device);
        fz_drop_device(ctx, &probe->super);
        fz_drop_page(ctx, page);
        // The gradient spans the page, but its clip only exposes a 24-point strip.
        QCOMPARE(rasterSize, QSize(64 * scale, 24 * scale));
    }

    void themeShading_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<int>("kind");
        const std::array names { "axial-gradient", "tiling-pattern", "gradient-in-pattern", "image-in-pattern" };
        for (const int theme : { 2, 3 })
            for (int kind = 0; kind < int(names.size()); ++kind) {
                const auto name = QByteArray(theme == 2 ? "dusk-" : "dark-") + names[std::size_t(kind)];
                QTest::newRow(name.constData()) << theme << kind;
            }
    }

    void themeShading()
    {
        QFETCH(int, theme);
        QFETCH(int, kind);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("theme-shading.pdf");
        QVERIFY(createShadingPdf(document.context(), path, kind));
        QVERIFY(openDocument(document, path));
        constexpr int width = 128, stride = width * 4;
        std::vector<std::uint8_t> original(width * stride), themed(width * stride);
        QVERIFY(document.renderToBuffer({ 0, width, width, std::nullopt }, original.data(), stride));
        auto settings = document.settings();
        settings.documentTheme = static_cast<Mu::Model::DocumentTheme>(theme);
        document.setSettings(settings);
        std::string error;
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, themed.data(), stride, &error),
                 error.c_str());
        QVERIFY(themed != original);
        if (kind == 3) {
            QCOMPARE(themed[0], std::uint8_t(204));
            QCOMPARE(themed[1], std::uint8_t(0));
            QCOMPARE(themed[2], std::uint8_t(0));
        }
        if (kind == 1) {
            QCOMPARE(themed[(100 * width + 10) * 4], std::uint8_t(218));
            QCOMPARE(themed[(100 * width + 70) * 4], std::uint8_t(theme == 2 ? 48 : 32));
        } else if (kind != 3) {
            // Both the direct and patterned gradient must retain contrast.
            QVERIFY(int(themed[(100 * width + 70) * 4]) - int(themed[(100 * width + 10) * 4]) > 60);
        }
        constexpr DocumentBase::RenderTile tile { 13, 87, 31, 25 };
        constexpr int tileStride = tile.width * 4 + 8;
        std::vector<std::uint8_t> tiled(tile.height * tileStride, 0xAB);
        QVERIFY2(document.renderToBuffer({ 0, width, width, tile }, tiled.data(), tileStride, &error), error.c_str());
        for (int y = 0; y < tile.height; ++y) {
            for (int x = 0; x < tile.width * 4; ++x)
                QVERIFY(std::abs(int(tiled[std::size_t(y * tileStride + x)])
                                 - int(themed[std::size_t((y + tile.y) * stride + tile.x * 4 + x)]))
                        <= 2);
            for (int x = tile.width * 4; x < tileStride; ++x)
                QCOMPARE(tiled[std::size_t(y * tileStride + x)], std::uint8_t(0xAB));
        }
        settings.documentTheme = Mu::Model::DocumentTheme::Default;
        document.setSettings(settings);
        std::vector<std::uint8_t> restored(width * stride);
        QVERIFY(document.renderToBuffer({ 0, width, width, std::nullopt }, restored.data(), stride));
        QCOMPARE(restored, original);
    }

    void detectsDarkPages_data()
    {
        QTest::addColumn<QByteArray>("rgba");
        QTest::addColumn<bool>("dark");
        QTest::newRow("white") << QByteArray::fromHex("ffffffffffffffffffffffffffffffff") << false;
        QTest::newRow("black") << QByteArray::fromHex("000000ff000000ff000000ff000000ff") << true;
        QTest::newRow("half") << QByteArray::fromHex("000000ff000000ffffffffffffffffff") << false;
        QTest::newRow("dark-majority") << QByteArray::fromHex("000000ff000000ff000000ffffffffff") << true;
        QTest::newRow("light-majority") << QByteArray::fromHex("000000ffffffffffffffffffffffffff") << false;
        QTest::newRow("below-mid-grey") << QByteArray::fromHex("7f7f7fff7f7f7fff7f7f7fff7f7f7fff") << true;
        QTest::newRow("mid-grey") << QByteArray::fromHex("808080ff808080ff808080ff808080ff") << false;
        QTest::newRow("red") << QByteArray::fromHex("ff0000ffff0000ffff0000ffff0000ff") << true;
        QTest::newRow("green") << QByteArray::fromHex("00ff00ff00ff00ff00ff00ff00ff00ff") << false;
    }

    void detectsDarkPages()
    {
        QFETCH(QByteArray, rgba);
        QFETCH(bool, dark);
        Mu::Worker::Engine::PdfDocument owner;
        // Cover short rows, vectorized rows with a scalar tail, and padding.
        for (const int width : { 2, 68 }) {
            const int stride = width * 4 + 8;
            QByteArray pixels(2 * stride, '\xab');
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < width; ++x)
                    std::copy_n(rgba.constData() + ((y * width + x) % 4) * 4, 4, pixels.data() + y * stride + x * 4);
            const auto original = pixels;
            auto* pixmap = fz_new_pixmap_with_data(owner.context(),
                                                   fz_device_rgb(owner.context()),
                                                   width,
                                                   2,
                                                   nullptr,
                                                   1,
                                                   stride,
                                                   reinterpret_cast<unsigned char*>(pixels.data()));
            const bool result = Mu::Worker::Engine::isMostlyDarkPixmap(owner.context(), pixmap);
            fz_drop_pixmap(owner.context(), pixmap);
            QCOMPARE(result, dark);
            QCOMPARE(pixels, original);
        }
    }

    void preservesDarkPages_data()
    {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<bool>("preserved");
        QTest::newRow("black") << QByteArray("0 g 0 0 612 792 re f") << true;
        QTest::newRow("mostly-black") << QByteArray("0 g 0 0 612 500 re f") << true;
        QTest::newRow("mostly-white") << QByteArray("0 g 0 0 612 200 re f") << false;
        QTest::newRow("white") << QByteArray("1 g 0 0 612 792 re f") << false;
    }

    void preservesDarkPages()
    {
        QFETCH(QByteArray, contents);
        QFETCH(bool, preserved);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const QString path = m_tempDir.filePath("dark-page.pdf");
        createTextPDF(document.context(), path, contents.constData());
        QVERIFY(openDocument(document, path));
        constexpr int width = 153, height = 198, stride = width * 4;
        std::vector<std::uint8_t> original(height * stride), themed(height * stride);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, original.data(), stride));
        auto settings = document.settings();
        settings.documentTheme = Mu::Model::DocumentTheme::Dusk;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, themed.data(), stride));
        QCOMPARE(themed == original, preserved);
        // The top tile can be white even when the page is predominantly black.
        const DocumentBase::RenderTile tile { 10, 10, 50, 30 };
        std::vector<std::uint8_t> tiled(tile.width * tile.height * 4);
        QVERIFY(document.renderToBuffer({ 0, width, height, tile }, tiled.data(), tile.width * 4));
        for (int y = 0; y < tile.height; ++y)
            QVERIFY(std::equal(tiled.begin() + y * tile.width * 4,
                               tiled.begin() + (y + 1) * tile.width * 4,
                               themed.begin() + ((y + tile.y) * width + tile.x) * 4));
    }

    void themesImageHeavyPages_data()
    {
        QTest::addColumn<int>("theme");
        QTest::newRow("dusk") << int(Mu::Model::DocumentTheme::Dusk);
        QTest::newRow("dark") << int(Mu::Model::DocumentTheme::Dark);
    }

    void themesImageHeavyPages()
    {
        QFETCH(int, theme);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("image-heavy-page.pdf");
        createTextPDF(document.context(),
                      path,
                      "1 g 0 0 612 792 re f "
                      "q 500 0 0 600 56 140 cm BI /W 1 /H 1 /BPC 8 /CS /G /F /AHx ID 50> EI Q");
        QVERIFY(openDocument(document, path));
        auto settings = document.settings();
        settings.documentTheme = static_cast<Mu::Model::DocumentTheme>(theme);
        document.setSettings(settings);
        constexpr int width = 153, height = 198, stride = width * 4;
        std::vector<std::uint8_t> pixels(height * stride);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, pixels.data(), stride));
        const auto paper =
            Mu::Model::ThemeColor::splitRgb(Mu::Model::ThemeColor::getBackground(settings.documentTheme));
        for (std::size_t channel = 0; channel < paper.size(); ++channel)
            QCOMPARE(pixels[channel], static_cast<std::uint8_t>(paper[channel]));
        // The dark image is dimmed to 80%; it must not suppress theming of the paper.
        QCOMPARE(pixels[80 * stride + 70 * 4], std::uint8_t(64));
    }

    void themesHighlights_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<std::uint32_t>("color");
        QTest::newRow("dusk") << 2 << std::uint32_t(0xFFFFFF00);
        QTest::newRow("dark") << 3 << std::uint32_t(0xFFFFFF00);
        QTest::newRow("dark-translucent") << 3 << std::uint32_t(0x80FFFF00);
    }

    void themesHighlights()
    {
        QFETCH(int, theme);
        QFETCH(std::uint32_t, color);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("theme-highlight.pdf");
        createTextPDF(document.context(), path, "BT /F1 40 Tf 100 400 Td (TEST) Tj ET");
        QVERIFY(openDocument(document, path));
        Mu::Model::Annotation annotation;
        annotation.subtype = Mu::Model::AnnotationType::Highlight;
        annotation.x0 = .1;
        annotation.y0 = .4;
        annotation.x1 = .9;
        annotation.y1 = .6;
        annotation.color = color;
        annotation.extras.quads.push_back({ { .1, .4 }, { .9, .4 }, { .9, .6 }, { .1, .6 } });
        std::int32_t object = -1;
        QVERIFY(document.addAnnotation(0, annotation, &object));
        constexpr int width = 153, height = 198, stride = width * 4;
        std::vector<std::uint8_t> original(height * stride), themed(height * stride);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, original.data(), stride));
        auto settings = document.settings();
        settings.documentTheme = static_cast<Mu::Model::DocumentTheme>(theme);
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, themed.data(), stride));
        // Highlighted paper stays dark, while the text remains light.
        const auto background = std::size_t(85 * stride + 20 * 4);
        for (std::size_t channel = 0; channel < 3; ++channel)
            QVERIFY(themed[background + channel] < 70);
        int brightest = 0;
        for (int y = 85; y < 105; ++y)
            for (int x = 25; x < 65; ++x)
                brightest = std::max(brightest, int(themed[std::size_t(y * stride + x * 4)]));
        QVERIFY(brightest > 150);
        constexpr DocumentBase::RenderTile tile { 16, 80, 56, 32 };
        std::vector<std::uint8_t> tiled(tile.width * tile.height * 4);
        QVERIFY(document.renderToBuffer({ 0, width, height, tile }, tiled.data(), tile.width * 4));
        for (int y = 0; y < tile.height; ++y)
            for (int x = 0; x < tile.width * 4; ++x)
                QVERIFY(std::abs(int(tiled[std::size_t(y * tile.width * 4 + x)])
                                 - int(themed[std::size_t((y + tile.y) * stride + tile.x * 4 + x)]))
                        <= 2);
        settings.documentTheme = Mu::Model::DocumentTheme::Default;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, themed.data(), stride));
        QCOMPARE(themed, original);
    }

    void keepsAnnotationBlendFallback_data()
    {
        QTest::addColumn<int>("type");
        QTest::addColumn<QByteArray>("blend");
        QTest::newRow("square-multiply") << int(PDF_ANNOT_SQUARE) << QByteArray("Multiply");
        QTest::newRow("square-difference") << int(PDF_ANNOT_SQUARE) << QByteArray("Difference");
        QTest::newRow("highlight-difference") << int(PDF_ANNOT_HIGHLIGHT) << QByteArray("Difference");
    }

    void keepsAnnotationBlendFallback()
    {
        QFETCH(int, type);
        QFETCH(QByteArray, blend);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("annotation-blend.pdf");
        createTextPDF(document.context(), path, "");
        QVERIFY(openDocument(document, path));
        auto* ctx = document.context();
        auto* pdf = pdf_document_from_fz_document(ctx, document.document());
        auto* page = pdf_load_page(ctx, pdf, 0);
        auto* annot = pdf_create_annot(ctx, page, static_cast<enum pdf_annot_type>(type));
        if (type == PDF_ANNOT_HIGHLIGHT) {
            const auto quad = fz_quad_from_rect({ 20, 20, 60, 60 });
            pdf_set_annot_quad_points(ctx, annot, 1, &quad);
        } else
            pdf_set_annot_rect(ctx, annot, { 20, 20, 60, 60 });
        auto* resources = pdf_new_dict(ctx, pdf, 1);
        pdf_dict_putp_drop(ctx, resources, "ExtGState/B/BM", pdf_new_name(ctx, blend.constData()));
        constexpr char commands[] = "/B gs 1 1 0 rg 0 0 40 40 re f";
        auto* contents =
            fz_new_buffer_from_copied_data(ctx, reinterpret_cast<const unsigned char*>(commands), sizeof(commands) - 1);
        pdf_set_annot_appearance(ctx, annot, "N", nullptr, fz_identity, { 0, 0, 40, 40 }, resources, contents);
        fz_drop_buffer(ctx, contents);
        pdf_drop_obj(ctx, resources);
        const auto info = inspectThemePage(
            ctx, &page->super, fz_bound_page(ctx, &page->super), Mu::Model::DocumentTheme::Dark, nullptr);
        pdf_drop_annot(ctx, annot);
        fz_drop_page(ctx, &page->super);
        QVERIFY(info.needsCompositing);
    }

    void preservesDarkBlendModes_data()
    {
        QTest::addColumn<QByteArray>("blend");
        QTest::addColumn<float>("alpha");
        QTest::addColumn<bool>("knockout");
        QTest::newRow("difference") << QByteArray("Difference") << 1.0f << false;
        QTest::newRow("multiply") << QByteArray("Multiply") << 1.0f << false;
        QTest::newRow("translucent-difference") << QByteArray("Difference") << 0.5f << false;
        QTest::newRow("translucent-multiply") << QByteArray("Multiply") << 0.5f << false;
        QTest::newRow("knockout") << QByteArray("Normal") << 1.0f << true;
    }

    void preservesDarkBlendModes()
    {
        QFETCH(QByteArray, blend);
        QFETCH(float, alpha);
        QFETCH(bool, knockout);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("theme-blends.pdf");
        QVERIFY(createBlendPdf(document.context(), path, blend, alpha, knockout));
        QVERIFY(openDocument(document, path));
        auto* page = fz_load_page(document.context(), document.document(), 0);
        const auto info = inspectThemePage(
            document.context(), page, fz_bound_page(document.context(), page), Mu::Model::DocumentTheme::Dark, nullptr);
        fz_drop_page(document.context(), page);
        QVERIFY(info.needsCompositing);
        QVERIFY(!info.mostlyDark);
        constexpr int width = 64, stride = width * 4;
        std::vector<std::uint8_t> expected(width * stride), actual(width * stride);
        QVERIFY(document.renderToBuffer({ 0, width, width, std::nullopt }, expected.data(), stride));
        auto* pixmap = fz_new_pixmap_with_data(
            document.context(), fz_device_rgb(document.context()), width, width, nullptr, 1, stride, expected.data());
        applyDocumentTheme(document.context(), pixmap, Mu::Model::DocumentTheme::Dark, nullptr);
        fz_drop_pixmap(document.context(), pixmap);
        auto settings = document.settings();
        settings.documentTheme = Mu::Model::DocumentTheme::Dark;
        document.setSettings(settings);
        QVERIFY(document.renderToBuffer({ 0, width, width, std::nullopt }, actual.data(), stride));
        QCOMPARE(actual, expected);
        constexpr DocumentBase::RenderTile tile { 13, 29, 25, 25 };
        std::vector<std::uint8_t> tiled(tile.width * tile.height * 4);
        QVERIFY(document.renderToBuffer({ 0, width, width, tile }, tiled.data(), tile.width * 4));
        for (int y = 0; y < tile.height; ++y)
            QVERIFY(std::equal(tiled.data() + y * tile.width * 4,
                               tiled.data() + (y + 1) * tile.width * 4,
                               expected.data() + (y + tile.y) * stride + tile.x * 4));
    }

    void brightensThemeColors_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QByteArray>("color");
        for (const auto theme : { Mu::Model::DocumentTheme::Dusk, Mu::Model::DocumentTheme::Dark }) {
            const auto name = theme == Mu::Model::DocumentTheme::Dusk ? QByteArray("dusk-") : QByteArray("dark-");
            QTest::newRow((name + "blue").constData()) << int(theme) << QByteArray("0 0 1");
            QTest::newRow((name + "purple").constData()) << int(theme) << QByteArray("0.5 0 0.5");
        }
    }

    void brightensThemeColors()
    {
        QFETCH(int, theme);
        QFETCH(QByteArray, color);
        using namespace Mu::Worker::Engine;
        PdfDocument document;
        const auto path = m_tempDir.filePath("theme-colors.pdf");
        const auto contents = "1 g 0 0 612 792 re f " + color + " rg 100 400 100 100 re f";
        createTextPDF(document.context(), path, contents.constData());
        QVERIFY(openDocument(document, path));
        auto settings = document.settings();
        settings.documentTheme = static_cast<Mu::Model::DocumentTheme>(theme);
        document.setSettings(settings);
        constexpr int width = 153, height = 198, stride = width * 4;
        std::vector<std::uint8_t> pixels(height * stride);
        QVERIFY(document.renderToBuffer({ 0, width, height, std::nullopt }, pixels.data(), stride));
        const auto* pixel = pixels.data() + 85 * stride + 35 * 4;
        QVERIFY((54 * pixel[0] + 183 * pixel[1] + 19 * pixel[2]) / 256 >= 139);
        QVERIFY(pixel[2] > pixel[1]);
        QCOMPARE(pixel[3], std::uint8_t(255));
    }

    void preservesDarkThemeEdges()
    {
        using namespace Mu::Worker::Engine;
        PdfDocument owner;
        QByteArray pixels(256 * 4, '\0');
        for (int level = 0; level < 256; ++level) {
            pixels[level * 4] = pixels[level * 4 + 1] = static_cast<char>(level);
            pixels[level * 4 + 2] = pixels[level * 4 + 3] = char(255);
        }
        auto* pixmap = fz_new_pixmap_with_data(owner.context(),
                                               fz_device_rgb(owner.context()),
                                               256,
                                               1,
                                               nullptr,
                                               1,
                                               256 * 4,
                                               reinterpret_cast<unsigned char*>(pixels.data()));
        applyDocumentTheme(owner.context(), pixmap, Mu::Model::DocumentTheme::Dark, nullptr);
        fz_drop_pixmap(owner.context(), pixmap);
        // Blue text coverage must blend smoothly into the paper, without a threshold jump.
        for (int level = 0; level < 256; ++level) {
            for (int channel = 0; channel < 3; ++channel) {
                const int ink = static_cast<unsigned char>(pixels[channel]);
                const int paper = static_cast<unsigned char>(pixels[255 * 4 + channel]);
                const int expected = (ink * (255 - level) + paper * level) / 255;
                QVERIFY(std::abs(int(static_cast<unsigned char>(pixels[level * 4 + channel])) - expected) <= 2);
            }
            QCOMPARE(pixels[level * 4 + 3], char(255));
        }
    }

    void documentThemePalette_data()
    {
        QTest::addColumn<int>("theme");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("default") << 0 << QByteArray::fromHex("ffffffff000000ffff0000ff");
        QTest::newRow("twilight") << 1 << QByteArray::fromHex("d0cab8ff000000ffd00000ff");
        QTest::newRow("dark") << 3 << QByteArray::fromHex("202124ffdadce0ffda9295ff");
    }

    void documentThemePalette()
    {
        QFETCH(int, theme);
        QFETCH(QByteArray, expected);
        Mu::Worker::Engine::PdfDocument owner;
        auto pixels = QByteArray::fromHex("ffffffff000000ffff0000ffabababab");
        auto* pixmap = fz_new_pixmap_with_data(owner.context(),
                                               fz_device_rgb(owner.context()),
                                               3,
                                               1,
                                               nullptr,
                                               1,
                                               16,
                                               reinterpret_cast<unsigned char*>(pixels.data()));
        Mu::Worker::Engine::applyDocumentTheme(
            owner.context(), pixmap, static_cast<Mu::Model::DocumentTheme>(theme), nullptr);
        int cancellation = 0;
        if (theme != 0) {
            fz_cookie cookie { };
            cookie.abort = 1;
            fz_try(owner.context())
            {
                Mu::Worker::Engine::applyDocumentTheme(
                    owner.context(), pixmap, static_cast<Mu::Model::DocumentTheme>(theme), &cookie);
            }
            fz_catch(owner.context())
            {
                cancellation = fz_caught(owner.context());
            }
        }
        fz_drop_pixmap(owner.context(), pixmap);
        QCOMPARE(cancellation, theme == 0 ? 0 : FZ_ERROR_ABORT);
        QCOMPARE(pixels.first(12), expected);
        QCOMPARE(pixels.last(4), QByteArray::fromHex("abababab"));
    }

    void documentThemesAcrossFormats_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("pdf") << 0;
        QTest::newRow("epub") << 1;
        QTest::newRow("mobi") << 2;
    }

    void documentThemesAcrossFormats()
    {
        QFETCH(int, format);
        using namespace Mu::Worker::Engine;
        std::unique_ptr<DocumentBase> document;
        QString path;
        if (format == 0) {
            document = std::make_unique<PdfDocument>();
            path = m_textPath;
        } else if (format == 1) {
            document = std::make_unique<EpubDocument>();
            path = QStringLiteral(TEST_EPUB_DIR "/sample.epub");
        } else {
            document = std::make_unique<MobiDocument>();
            path = QStringLiteral(TEST_MOBI_DIR "/legacy-uncompressed.mobi");
        }
        QFile source(path);
        QVERIFY(source.open(QIODevice::ReadOnly));
        std::string error;
        QVERIFY2(document->openFd(::dup(source.handle()), path.toStdString(), &error), error.c_str());
        constexpr int width = 160, height = 200, stride = width * 4;
        std::vector<std::uint8_t> original(height * stride);
        QVERIFY2(document->renderToBuffer({ 0, width, height, std::nullopt }, original.data(), stride, &error),
                 error.c_str());
        // Printing uses savePdfFd rather than the themed viewing renderer.
        const auto renderPrintOutput = [&](std::vector<std::uint8_t>& pixels) {
            QTemporaryFile output;
            if (!output.open() || !document->savePdfFd(::dup(output.handle()), { 0 }, &error) || !output.seek(0))
                return false;
            PdfDocument printed;
            if (!printed.openFd(::dup(output.handle()), "print.pdf", &error))
                return false;
            pixels.resize(height * stride);
            return printed.renderToBuffer({ 0, width, height, std::nullopt }, pixels.data(), stride, &error);
        };
        std::vector<std::uint8_t> originalPrint;
        QVERIFY2(renderPrintOutput(originalPrint), error.c_str());
        const int count = document->pageCount();
        for (const auto theme :
             { Mu::Model::DocumentTheme::Dark, Mu::Model::DocumentTheme::Twilight, Mu::Model::DocumentTheme::Dusk }) {
            auto settings = document->settings();
            settings.documentTheme = theme;
            document->setSettings(settings);
            QCOMPARE(document->pageCount(), count);
            std::vector<std::uint8_t> themed(height * stride);
            QVERIFY2(document->renderToBuffer({ 0, width, height, std::nullopt }, themed.data(), stride, &error),
                     error.c_str());
            QVERIFY(themed != original);
            std::vector<std::uint8_t> themedPrint;
            QVERIFY2(renderPrintOutput(themedPrint), error.c_str());
            QCOMPARE(themedPrint, originalPrint);
            constexpr int paddedStride = stride + 8;
            std::vector<std::uint8_t> padded(height * paddedStride, 0xAB);
            QVERIFY2(document->renderToBuffer({ 0, width, height, std::nullopt }, padded.data(), paddedStride, &error),
                     error.c_str());
            for (int y = 0; y < height; ++y) {
                QVERIFY(std::equal(
                    themed.begin() + y * stride, themed.begin() + (y + 1) * stride, padded.begin() + y * paddedStride));
                for (int x = stride; x < paddedStride; ++x)
                    QCOMPARE(padded[static_cast<std::size_t>(y * paddedStride + x)], std::uint8_t(0xAB));
            }
            const DocumentBase::RenderTile tile { 31, 37, 65, 89 };
            std::vector<std::uint8_t> tiled(static_cast<std::size_t>(tile.width * tile.height * 4));
            QVERIFY2(document->renderToBuffer({ 0, width, height, tile }, tiled.data(), tile.width * 4, &error),
                     error.c_str());
            for (int y = 0; y < tile.height; ++y)
                for (int x = 0; x < tile.width * 4; ++x)
                    QVERIFY(std::abs(int(tiled[static_cast<std::size_t>(y * tile.width * 4 + x)])
                                     - int(themed[static_cast<std::size_t>((y + tile.y) * stride + tile.x * 4 + x)]))
                            <= 2);
        }
        auto settings = document->settings();
        settings.documentTheme = Mu::Model::DocumentTheme::Default;
        document->setSettings(settings);
        std::vector<std::uint8_t> restored(height * stride);
        QVERIFY2(document->renderToBuffer({ 0, width, height, std::nullopt }, restored.data(), stride, &error),
                 error.c_str());
        QCOMPARE(restored, original);
    }

    void testDehyphenatedCharacterFiltering_data()
    {
        QTest::addColumn<int>("lineFlags");
        QTest::addColumn<int>("codepoint");
        QTest::addColumn<bool>("hasNext");
        QTest::addColumn<bool>("hasNextLine");
        QTest::addColumn<bool>("expectedSkip");

        QTest::newRow("ordinary-hyphen") << 0 << static_cast<int>('-') << false << false << false;
        QTest::newRow("joined-line-hyphen")
            << static_cast<int>(FZ_STEXT_LINE_FLAGS_JOINED) << static_cast<int>('-') << false << true << true;
        QTest::newRow("joined-line-nonterminal-hyphen")
            << static_cast<int>(FZ_STEXT_LINE_FLAGS_JOINED) << static_cast<int>('-') << true << true << false;
        QTest::newRow("terminal-line-hyphen")
            << static_cast<int>(FZ_STEXT_LINE_FLAGS_JOINED) << static_cast<int>('-') << false << false << false;
        QTest::newRow("soft-hyphen") << 0 << 0x00AD << false << false << true;
    }

    void testDehyphenatedCharacterFiltering()
    {
        QFETCH(int, lineFlags);
        QFETCH(int, codepoint);
        QFETCH(bool, hasNext);
        QFETCH(bool, hasNextLine);
        QFETCH(bool, expectedSkip);

        fz_stext_line line { };
        line.flags = static_cast<decltype(line.flags)>(lineFlags);
        fz_stext_char next { };
        fz_stext_line nextLine { };
        nextLine.first_char = &next;
        line.next = hasNextLine ? &nextLine : nullptr;
        fz_stext_char character { };
        character.c = codepoint;
        character.next = hasNext ? &next : nullptr;

        QCOMPARE(Mu::Worker::Engine::shouldSkipDehyphenatedChar(&line, &character), expectedSkip);
        const bool expectedJoin = hasNextLine && (lineFlags & FZ_STEXT_LINE_FLAGS_JOINED) != 0;
        QCOMPARE(Mu::Worker::Engine::isDehyphenatedLine(&line), expectedJoin);
        nextLine.first_char = nullptr;
        QVERIFY(!Mu::Worker::Engine::isDehyphenatedLine(&line));
    }

    void testDehyphenatedDocumentText_data()
    {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("expectedText");
        QTest::newRow("terminal-hyphen") << QByteArray("BT /F1 12 Tf 72 700 Td (command -) Tj ET")
                                         << QStringLiteral("command -\n");
        QTest::newRow("wrapped-word") << QByteArray("BT /F1 12 Tf 72 700 Td (inter-) Tj 0 -18 Td (national) Tj ET")
                                      << QStringLiteral("international\n");
        QTest::newRow("wrapped-word-terminal-hyphen")
            << QByteArray("BT /F1 12 Tf 72 700 Td (inter-) Tj 0 -18 Td (national-) Tj ET")
            << QStringLiteral("international-\n");
    }

    void testDehyphenatedDocumentText()
    {
        QFETCH(QByteArray, contents);
        QFETCH(QString, expectedText);
        const QString path = m_tempDir.filePath(QStringLiteral("dehyphenation.pdf"));
        fz_context* context = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
        QVERIFY(context);
        createTextPDF(context, path, contents.constData());
        fz_drop_context(context);

        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, path));
        std::string error;
        const auto boxes = document.textBoxes(0, 72, 72, 1000, true, &error);
        QVERIFY2(error.empty(), error.c_str());
        QString text;
        for (const auto& box : boxes) {
            text += QString::fromStdString(box.text);
            if (box.endOfLine)
                text += QLatin1Char('\n');
        }
        QCOMPARE(text, expectedText);
    }

    void initTestCase()
    {
        QVERIFY(m_tempDir.isValid());
        m_multiPagePath = m_tempDir.filePath("multi.pdf");
        m_singlePagePath = m_tempDir.filePath("single.pdf");
        m_textPath = m_tempDir.filePath("text.pdf");

        fz_context* context = fz_new_context(nullptr, nullptr, 64 * 1024 * 1024);
        QVERIFY(context);
        fz_register_document_handlers(context);
        createMultiPagePDF(context, m_multiPagePath, 35);
        createMultiPagePDF(context, m_singlePagePath, 1);
        createTextPDF(context, m_textPath);
        fz_drop_context(context);
    }

    void testPageGeometry()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));
        const auto geometry = document.pageGeometry(0);
        QCOMPARE(geometry.widthPoints, 1.0);
        QCOMPARE(geometry.heightPoints, 1.0);
        QCOMPARE(QString::fromStdString(geometry.label), QStringLiteral("1"));
    }

    void testTextPDFGeometry()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        const auto geometry = document.pageGeometry(0);
        QCOMPARE(geometry.widthPoints, 612.0);
        QCOMPARE(geometry.heightPoints, 792.0);
        QCOMPARE(QString::fromStdString(geometry.label), QStringLiteral("1"));
    }

    void testRender()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));
        std::string error;
        std::vector<std::uint8_t> buffer(400 * 600 * 4);
        QVERIFY2(document.renderToBuffer({ 0, 400, 600, std::nullopt }, buffer.data(), 400 * 4, &error), error.c_str());
        // The fixture is a blank page: every RGBA byte must be opaque white.
        QVERIFY(std::all_of(buffer.begin(), buffer.end(), [](auto byte) { return byte == 255; }));

        std::vector<std::uint8_t> tileBuffer(200 * 300 * 4);
        QVERIFY2(
            document.renderToBuffer({ 0, 400, 600, Mu::Worker::Engine::DocumentBase::RenderTile { 0, 0, 200, 300 } },
                                    tileBuffer.data(),
                                    200 * 4,
                                    &error),
            error.c_str());
        QVERIFY(std::all_of(tileBuffer.begin(), tileBuffer.end(), [](auto byte) { return byte == 255; }));
    }

    void layersPreserveSiblingDepths_data()
    {
        QTest::addColumn<int>("extraDepth");
        QTest::newRow("direct children") << 0;
        QTest::newRow("one unnamed group") << 1;
        QTest::newRow("two unnamed groups") << 2;
    }

    void layersPreserveSiblingDepths()
    {
        QFETCH(int, extraDepth);
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf")));
        fz_context* context = document.context();
        pdf_document* pdf = pdf_specifics(context, document.document());
        pdf_obj* defaults = pdf_dict_getp(context, pdf_trailer(context, pdf), "Root/OCProperties/D");
        pdf_obj* order = pdf_dict_get(context, defaults, PDF_NAME(Order));
        for (int i = 0; i < extraDepth; ++i) {
            pdf_obj* wrapper = pdf_new_array(context, pdf, 1);
            pdf_array_push(context, wrapper, pdf_array_get(context, order, 3));
            pdf_array_put(context, order, 3, wrapper);
            pdf_drop_obj(context, wrapper);
        }
        // Rebuild MuPDF's UI after changing /Order.
        pdf_select_layer_config(context, pdf, -1);
        std::string error;
        const auto entries = document.layers(&error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(entries.size(), std::size_t(9));
        QCOMPARE(entries[2].depth, 0);
        QCOMPARE(entries[3].depth, 1);
        QCOMPARE(entries[4].depth, 1);
        QCOMPARE(entries[5].depth, 0);
        QCOMPARE(entries[6].depth, 1);
        QCOMPARE(entries[7].depth, 1);
    }

    void layersRespectVisibilityAndUiRules()
    {
        using namespace Mu::Model;
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, QStringLiteral(TEST_SIGNATURE_PDF_DIR "/layers.pdf")));
        std::string error;
        auto entries = document.layers(&error);
        QVERIFY2(error.empty(), error.c_str());
        QCOMPARE(entries.size(), std::size_t(9));
        const auto findId = [&](const std::string& name) {
            const auto it = std::find_if(entries.begin(), entries.end(), [&](const auto& e) { return e.name == name; });
            return it == entries.end() ? -1 : it->id;
        };
        const int red = findId("Red (initially on)");
        const int blue = findId("Blue (initially off)");
        const int parent = findId("Nested parent (initially on)");
        const int childB = findId("Child B (initially off)");
        const int variantA = findId("Variant A");
        const int variantB = findId("Variant B");
        const int locked = findId("Locked reference (always on)");
        const int label = findId("Exclusive choice");
        QVERIFY(red >= 0 && blue >= 0 && parent >= 0 && childB >= 0);
        QVERIFY(variantA >= 0 && variantB >= 0 && locked >= 0 && label >= 0);
        QCOMPARE(entries[static_cast<std::size_t>(childB)].depth, 1);
        QCOMPARE(entries[static_cast<std::size_t>(variantB)].type, LayerType::RadioButton);
        QVERIFY(entries[static_cast<std::size_t>(locked)].locked);
        const auto render = [&] {
            QImage image(612, 792, QImage::Format_RGBA8888);
            if (!document.renderToBuffer({ 0, 612, 792, std::nullopt },
                                         image.bits(),
                                         static_cast<std::size_t>(image.bytesPerLine()),
                                         &error))
                return QImage { };
            return image;
        };
        const auto original = render();
        QVERIFY(!original.isNull());
        QVERIFY(document.isPageCached(0));
        const auto text = [&] {
            std::string value;
            for (const auto& box : document.textBoxes(0, 72, 72, 10000))
                value += box.text;
            return value;
        };
        QVERIFY(text().find("BLUE") == std::string::npos);
        QVERIFY2(document.setLayer(blue, true, &error), error.c_str());
        QVERIFY(!document.isPageCached(0));
        const auto withBlue = render();
        QVERIFY(withBlue.pixelColor(340, 210).blue() > withBlue.pixelColor(340, 210).red());
        QVERIFY(text().find("BLUE") != std::string::npos);
        QVERIFY(document.setLayer(childB, true, &error));
        QVERIFY(document.setLayer(parent, false, &error));
        const auto withoutParent = render();
        QCOMPARE(withoutParent.pixelColor(75, 350), original.pixelColor(340, 350));
        QVERIFY(document.setLayer(variantB, true, &error));
        entries = document.layers();
        QVERIFY(!entries[static_cast<std::size_t>(variantA)].selected);
        QVERIFY(entries[static_cast<std::size_t>(variantB)].selected);
        for (int id : { -1, 999, locked, label }) {
            error.clear();
            QVERIFY(!document.setLayer(id, false, &error));
            QVERIFY(!error.empty());
        }
        QVERIFY(document.setLayer(blue, false));
        QVERIFY(document.setLayer(childB, false));
        QVERIFY(document.setLayer(parent, true));
        QVERIFY(document.setLayer(variantA, true));
        QCOMPARE(render(), original);

        // Tiled rendering must use the same visibility state as a full page.
        QVERIFY(document.setLayer(red, false));
        const auto full = render();
        QImage tile(240, 112, QImage::Format_RGBA8888);
        QVERIFY(
            document.renderToBuffer({ 0, 612, 792, Mu::Worker::Engine::DocumentBase::RenderTile { 48, 128, 240, 112 } },
                                    tile.bits(),
                                    static_cast<std::size_t>(tile.bytesPerLine())));
        QCOMPARE(tile, full.copy(48, 128, 240, 112));

        // Viewing state must not change saved defaults.
        const QString saved = m_tempDir.filePath(QStringLiteral("layers-saved.pdf"));
        QFile output(saved);
        QVERIFY(output.open(QIODevice::ReadWrite));
        QVERIFY(document.saveFd(::dup(output.handle())));
        output.close();
        Mu::Worker::Engine::PdfDocument reopened;
        QVERIFY(openDocument(reopened, saved));
        QVERIFY(reopened.layers()[static_cast<std::size_t>(red)].selected);
        document.close();
        QVERIFY(openDocument(document, m_textPath));
        QVERIFY(document.layers().empty());
    }

    void testOverprintSimulation_data()
    {
        QTest::addColumn<bool>("spot");
        QTest::newRow("process colors") << false;
        QTest::newRow("spot color") << true;
    }

    void testOverprintSimulation()
    {
        QFETCH(bool, spot);
        // Cyan beneath an overprinting yellow rectangle: the overlap should
        // retain cyan when simulated, rather than knock it out.
        const QByteArray content = spot
            ? QByteArray("1 0 0 0 k 10 10 60 60 re f /OP gs /Spot cs 1 scn 40 40 50 50 re f")
            : QByteArray("1 0 0 0 k 10 10 60 60 re f /OP gs 0 0 1 0 k 40 40 50 50 re f");
        const QList<QByteArray> objects {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << "
            "/ExtGState << /OP << /Type /ExtGState /OP true /op true /OPM 1 >> >> "
                + (spot ? QByteArray("/ColorSpace << /Spot [/Separation /Yellow /DeviceCMYK "
                                     "<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0 1 0] /N 1 >>] >> ")
                        : QByteArray())
                + ">> /Contents 4 0 R >>",
            "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream"
        };
        QByteArray pdf("%PDF-1.7\n");
        QList<qsizetype> offsets;
        for (qsizetype i = 0; i < objects.size(); ++i) {
            offsets.append(pdf.size());
            pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const qsizetype xref = pdf.size();
        pdf += "xref\n0 5\n0000000000 65535 f \n";
        for (qsizetype offset : offsets)
            pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        pdf += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        const QString path = m_tempDir.filePath(spot ? "spot-overprint.pdf" : "process-overprint.pdf");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(pdf), pdf.size());
        file.close();

        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, path));
        ::Mu::Model::DocumentSettings settings;
        settings.paperColorRgb = 0xF0E0D0;
        document.setSettings(settings);
        constexpr int width = 100;
        constexpr int stride = width * 4;
        std::vector<std::uint8_t> disabled(width * stride);
        std::string error;
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, disabled.data(), stride, &error),
                 error.c_str());
        settings.overprintSimulation = true;
        document.setSettings(settings);
        std::vector<std::uint8_t> enabled(width * stride);
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, enabled.data(), stride, &error),
                 error.c_str());
        const auto overlap = static_cast<std::size_t>((50 * width + 50) * 4);
        QVERIFY(enabled[overlap] + 30 < disabled[overlap]);
        QCOMPARE(enabled[overlap + 3], std::uint8_t(255));
        // Paper stays opaque; CMYK simulation can round-trip RGB slightly.
        QVERIFY(std::abs(int(enabled[0]) - 0xF0) <= 5);
        QVERIFY(std::abs(int(enabled[1]) - 0xE0) <= 5);
        QVERIFY(std::abs(int(enabled[2]) - 0xD0) <= 5);
        QCOMPARE(enabled[3], std::uint8_t(255));
        constexpr int paddedStride = stride + 12;
        std::vector<std::uint8_t> padded(width * paddedStride, 0xAB);
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, padded.data(), paddedStride, &error),
                 error.c_str());
        for (int y = 0; y < width; ++y) {
            for (int x = 0; x < stride; ++x)
                QCOMPARE(padded[static_cast<std::size_t>(y * paddedStride + x)],
                         enabled[static_cast<std::size_t>(y * stride + x)]);
            for (int x = stride; x < paddedStride; ++x)
                QCOMPARE(padded[static_cast<std::size_t>(y * paddedStride + x)], std::uint8_t(0xAB));
        }
        constexpr int tileSize = 40;
        std::vector<std::uint8_t> tile(tileSize * tileSize * 4);
        QVERIFY2(document.renderToBuffer(
                     { 0, width, width, Mu::Worker::Engine::DocumentBase::RenderTile { 30, 30, tileSize, tileSize } },
                     tile.data(),
                     tileSize * 4,
                     &error),
                 error.c_str());
        for (int y = 0; y < tileSize; ++y)
            for (int x = 0; x < tileSize * 4; ++x)
                QCOMPARE(tile[static_cast<std::size_t>(y * tileSize * 4 + x)],
                         enabled[static_cast<std::size_t>((y + 30) * stride + 30 * 4 + x)]);
        settings.overprintSimulation = false;
        document.setSettings(settings);
        std::vector<std::uint8_t> restored(width * stride);
        QVERIFY2(document.renderToBuffer({ 0, width, width, std::nullopt }, restored.data(), stride, &error),
                 error.c_str());
        QVERIFY(restored == disabled);
    }

    void testRenderRejectsInvalidDimensionsAndIsDeterministic()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        std::vector<std::uint8_t> buffer(400 * 400 * 4);
        for (const auto& request : { Mu::Worker::Engine::DocumentBase::RenderRequest { 0, 0, 100, std::nullopt },
                                     Mu::Worker::Engine::DocumentBase::RenderRequest { 0, -1, 100, std::nullopt },
                                     Mu::Worker::Engine::DocumentBase::RenderRequest { 0, 100, 0, std::nullopt } }) {
            std::string error;
            QVERIFY(!document.renderToBuffer(request, buffer.data(), 400 * 4, &error));
            QCOMPARE(error, std::string("render dimensions are invalid"));
        }
        std::vector<std::uint8_t> first(120 * 160 * 4);
        std::vector<std::uint8_t> second(120 * 160 * 4);
        QVERIFY(document.renderToBuffer({ 0, 120, 160, std::nullopt }, first.data(), 120 * 4));
        QVERIFY(document.renderToBuffer({ 0, 120, 160, std::nullopt }, second.data(), 120 * 4));
        QCOMPARE(first, second);
    }

    void testPageCacheReuseAndEviction()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));
        QVERIFY(!document.isPageCached(0));

        std::vector<std::uint8_t> buffer(40 * 60 * 4);
        std::string error;
        const auto render = [&](int page) {
            return document.renderToBuffer({ page, 40, 60, std::nullopt }, buffer.data(), 40 * 4, &error);
        };

        QVERIFY2(render(0), error.c_str());
        QVERIFY(document.isPageCached(0));

        // A second render of the same page reuses the cached handle.
        QVERIFY2(render(0), error.c_str());
        QVERIFY(document.isPageCached(0));

        // The cache holds three pages: earlier entries survive new loads.
        QVERIFY2(render(1), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY(document.isPageCached(1));
        QVERIFY2(render(2), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY(document.isPageCached(1));
        QVERIFY(document.isPageCached(2));

        // Touching page 0 makes it most recently used, so loading page 3
        // evicts page 1, the least recently used entry.
        QVERIFY2(render(0), error.c_str());
        QVERIFY2(render(3), error.c_str());
        QVERIFY(document.isPageCached(0));
        QVERIFY(document.isPageCached(2));
        QVERIFY(document.isPageCached(3));
        QVERIFY(!document.isPageCached(1));

        document.close();
        QVERIFY(!document.isPageCached(0));
        QVERIFY(!document.isPageCached(2));
        QVERIFY(!document.isPageCached(3));
    }

    void testAnnotationWriteKeepsCachedRenderCurrent()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));

        const auto render = [&document](std::vector<std::uint8_t>& pixels) {
            std::string error;
            pixels.resize(120 * 160 * 4);
            return document.renderToBuffer({ 0, 120, 160, std::nullopt }, pixels.data(), 120 * 4, &error);
        };

        std::vector<std::uint8_t> before;
        QVERIFY(render(before));
        QVERIFY(document.isPageCached(0));

        ::Mu::Model::Annotation annotation;
        annotation.subtype = ::Mu::Model::AnnotationType::Highlight;
        annotation.uuid = "cache-invalidation";
        annotation.x0 = .1;
        annotation.y0 = .2;
        annotation.x1 = .5;
        annotation.y1 = .3;
        annotation.color = 0xffff0000U;
        annotation.extras.quads.push_back({ { .1, .2 }, { .5, .2 }, { .5, .3 }, { .1, .3 } });
        std::int32_t object = -1;
        std::string error;
        QVERIFY2(document.addAnnotation(0, annotation, &object, &error), error.c_str());
        QVERIFY(object > 0);
        // The annotation is created on the cached page handle, so it stays valid.
        QVERIFY(document.isPageCached(0));

        std::vector<std::uint8_t> after;
        QVERIFY(render(after));
        QVERIFY(before != after);

        QVERIFY2(document.removeAnnotation(0, object, &error), error.c_str());
        QVERIFY(document.isPageCached(0));
        std::vector<std::uint8_t> restored;
        QVERIFY(render(restored));
        QCOMPARE(restored, before);
    }

    void testSaveClearsPageCache()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        std::vector<std::uint8_t> buffer(40 * 60 * 4);
        std::string error;
        for (int page = 0; page < 3; ++page) {
            QVERIFY2(document.renderToBuffer({ page, 40, 60, std::nullopt }, buffer.data(), 40 * 4, &error),
                     error.c_str());
            QVERIFY(document.isPageCached(page));
        }

        const QString saved = m_tempDir.filePath("cache-save.pdf");
        QFile savedFile(saved);
        QVERIFY(savedFile.open(QIODevice::WriteOnly));
        QVERIFY2(document.saveFd(savedFile.handle(), &error), error.c_str());
        savedFile.close();
        for (int page = 0; page < 3; ++page)
            QVERIFY(!document.isPageCached(page));
    }

    void testTextBoxesAndLinks()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_textPath));
        std::string error;
        const auto boxes = document.textBoxes(0, 72, 72, 10000, /*skipAnnots=*/false, &error);
        QVERIFY2(!boxes.empty(), error.c_str());
        QString extracted;
        for (const auto& box : boxes)
            extracted.append(QString::fromUtf8(box.text));
        QVERIFY(extracted.contains(QStringLiteral("Hello World")));

        // skipAnnots=true must return no more boxes than the full run
        const auto boxesContentOnly = document.textBoxes(0, 72, 72, 10000, /*skipAnnots=*/true, &error);
        QVERIFY2(error.empty(), error.c_str());
        QVERIFY(boxesContentOnly.size() <= boxes.size());

        QVERIFY(document.extractLinks(0, &error).empty());
        QVERIFY(document.extractAnnotations(0, &error).empty());
    }

    void resolveLinkTable_data()
    {
        QTest::addColumn<QString>("uri");
        QTest::addColumn<bool>("multiPage");
        QTest::addColumn<bool>("expectValid");
        QTest::addColumn<int>("expectPage");
        QTest::addColumn<int>("expectMask");
        QTest::addColumn<double>("expectX");
        QTest::addColumn<double>("expectY");
        QTest::addColumn<bool>("expectExternal");

        constexpr double skip = std::numeric_limits<double>::quiet_NaN();
        constexpr int coordinateX = Mu::Model::Viewport::CoordinateX;
        constexpr int coordinateY = Mu::Model::Viewport::CoordinateY;
        QTest::newRow("xyz") << QStringLiteral("#page=2&zoom=nan,0.25,0.75") << true << true << 1 << 3 << 0.25 << 0.75
                             << false;
        QTest::newRow("fitH") << QStringLiteral("#page=3&view=FitH,0.5") << true << true << 2 << coordinateY << 0.0
                              << 0.5 << false;
        QTest::newRow("fitV") << QStringLiteral("#page=4&view=FitV,0.5") << true << true << 3 << coordinateX << 0.5
                              << 0.0 << false;
        QTest::newRow("fitR") << QStringLiteral("#page=5&viewrect=0.25,0.1,0.5,0.5") << true << true << 4 << 3 << 0.25
                              << 0.1 << false;
        QTest::newRow("fitBH") << QStringLiteral("#page=6&view=FitBH,0.25") << true << true << 5 << coordinateY << 0.0
                               << 0.25 << false;
        QTest::newRow("fitBV") << QStringLiteral("#page=7&view=FitBV,0.25") << true << true << 6 << coordinateX << 0.25
                               << 0.0 << false;
        QTest::newRow("explicitTopLeft") << QStringLiteral("#page=1&zoom=nan,69.04297,78.73584") << false << true << 0
                                         << 3 << 69.04297 / 612.0 << (78.73584 - 12.0) / 792.0 << false;
        QTest::newRow("external") << QStringLiteral("https://example.com/document.pdf") << true << true << -1 << 0
                                  << skip << skip << true;
        QTest::newRow("pageOutOfRange") << QStringLiteral("#page=999&view=Fit") << true << false << -1 << 0 << skip
                                        << skip << false;
        QTest::newRow("missingNamedDest")
            << QStringLiteral("#nameddest=missing") << true << false << -1 << 0 << skip << skip << false;
    }

    void resolveLinkTable()
    {
        QFETCH(QString, uri);
        QFETCH(bool, multiPage);
        QFETCH(bool, expectValid);
        QFETCH(int, expectPage);
        QFETCH(int, expectMask);
        QFETCH(double, expectX);
        QFETCH(double, expectY);
        QFETCH(bool, expectExternal);

        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, multiPage ? m_multiPagePath : m_textPath));

        const auto link = document.resolveLink(uri.toStdString());
        QCOMPARE(link.valid, expectValid);
        QCOMPARE(link.external, expectExternal);
        if (!expectValid)
            return;
        QCOMPARE(link.viewport.page, expectPage);
        QCOMPARE(link.viewport.coordinateMask, static_cast<std::uint8_t>(expectMask));
        if (expectExternal) {
            QCOMPARE(link.uri, std::string("https://example.com/document.pdf"));
            return;
        }
        if (!std::isnan(expectX))
            QVERIFY(std::abs(link.viewport.normalizedX - expectX) < 0.0001);
        if (!std::isnan(expectY))
            QVERIFY(std::abs(link.viewport.normalizedY - expectY) < 0.0001);
    }

    void testTransformedPageAndMalformedPageObject()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        fz_context* context = document.context();
        pdf_document* pdfDocument = pdf_specifics(context, document.document());
        QVERIFY(pdfDocument);
        pdf_page* page = pdf_load_page(context, pdfDocument, 1);
        QVERIFY(page);
        fz_try(context)
        {
            pdf_drop_page(context, page);
            page = nullptr;
            pdf_obj* pageObject = pdf_lookup_page_obj(context, pdfDocument, 1);
            pdf_dict_put_rect(context, pageObject, PDF_NAME(MediaBox), { 10, 20, 510, 620 });
            pdf_dict_put_rect(context, pageObject, PDF_NAME(CropBox), { 110, 120, 410, 520 });
            pdf_dict_put_int(context, pageObject, PDF_NAME(Rotate), 90);
        }
        fz_always(context)
        {
            if (page)
                pdf_drop_page(context, page);
        }
        fz_catch(context)
        {
            QFAIL(fz_caught_message(context));
        }

        const auto transformed = document.resolveLink("#page=2&zoom=nan,260,320");
        QVERIFY(transformed.valid);
        QCOMPARE(transformed.viewport.page, 1);
        QCOMPARE(transformed.viewport.coordinateMask, uint8_t(3));
        QVERIFY(std::abs(transformed.viewport.normalizedX - 0.65) < 0.0001);
        QVERIFY(std::abs(transformed.viewport.normalizedY - (1.0 - 12.0 / 300.0)) < 0.0001);

        pdf_obj* malformedPage = pdf_lookup_page_obj(context, pdfDocument, 2);
        QVERIFY(malformedPage);
        pdf_dict_puts_drop(context, malformedPage, "MediaBox", pdf_new_string(context, "malformed", 9));
        const auto malformed = document.resolveLink("#page=3&zoom=nan,0.5,0.5");
        QVERIFY(malformed.valid);
        QCOMPARE(malformed.viewport.page, 2);
        QCOMPARE(malformed.viewport.coordinateMask, uint8_t(3));
        QVERIFY(std::isfinite(malformed.viewport.normalizedX));
        QVERIFY(std::isfinite(malformed.viewport.normalizedY));

        std::string error;
        const auto malformedDetails = document.pageDetails(2, &error);
        QVERIFY(error.empty());
        QVERIFY(std::isfinite(malformedDetails.geometry.widthPoints));
        QVERIFY(std::isfinite(malformedDetails.geometry.heightPoints));
        error.clear();
        const auto unavailable = document.pageDetails(document.pageCount(), &error);
        QVERIFY(unavailable.annotations.empty());
        QVERIFY(!error.empty());
    }

    void testLinkInvalidatedAfterReopen()
    {
        Mu::Worker::Engine::PdfDocument document;
        QVERIFY(openDocument(document, m_multiPagePath));

        const std::string uri = "#page=2&view=Fit";
        QVERIFY(document.resolveLink(uri).valid);

        document.close();
        QVERIFY(openDocument(document, m_singlePagePath));
        QVERIFY(!document.resolveLink(uri).valid);
    }
};

int runTestWorkerPage(int argc, char** argv)
{
    TestPage test;
    return QTest::qExec(&test, argc, argv);
}

#include "page.moc"
