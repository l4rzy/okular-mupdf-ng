#include "runtime/command_service.hpp"
#include "shared/model/types.hpp"
#include "shared/model/validation.hpp"
#include "shared/protocol/ipc_debug.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include "shared/transport/compat.hpp"
#include <QTest>

class TestNativeWorkerTypes : public QObject {
    Q_OBJECT
private slots:

    void pingCompatibilityRoundTrip()
    {
        using namespace Mu;
        const Model::RequestMessage input { 5, Model::PingRequest { "0.2.3" } };
        std::string error;
        const auto requestBytes = ::Mu::IPC::ZppCodec::encode(input, &error);
        QVERIFY(requestBytes);
        Model::RequestMessage decodedRequest;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*requestBytes, &decodedRequest, &error));
        QCOMPARE(std::get<Model::PingRequest>(decodedRequest.payload).compat, std::string("0.2.3"));

        const Model::ResponseMessage response { 5,
                                                Model::PingResponse { std::string(::Mu::IPC::COMPAT), 123, { } },
                                                std::nullopt };
        const auto responseBytes = ::Mu::IPC::ZppCodec::encode(response, &error);
        QVERIFY(responseBytes);
        Model::ResponseMessage decodedResponse;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*responseBytes, &decodedResponse, &error));
        QCOMPARE(std::get<Model::PingResponse>(decodedResponse.payload).compat, std::string(::Mu::IPC::COMPAT));
    }

    void typedRequestRoundTrip()
    {
        using namespace Mu;
        Model::RequestMessage input { 42, Model::RenderRequest { 2, 640, 480, Model::RenderTile { 4, 5, 100, 120 } } };
        std::string error;
        const auto encoded = ::Mu::IPC::ZppCodec::encode(input, &error);
        QVERIFY(encoded);
        Model::RequestMessage output;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*encoded, &output, &error));
        QCOMPARE(output.id, std::uint64_t(42));
        const auto* render = std::get_if<Model::RenderRequest>(&output.payload);
        QVERIFY(render);
        QCOMPARE(render->tile->width, 100);

        const Model::RequestMessage release { 43, Model::ReleaseFrameSlotRequest { 7, 11 } };
        const auto releaseBytes = ::Mu::IPC::ZppCodec::encode(release, &error);
        QVERIFY(releaseBytes);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*releaseBytes, &output, &error));
        const auto* decodedRelease = std::get_if<Model::ReleaseFrameSlotRequest>(&output.payload);
        QVERIFY(decodedRelease);
        QCOMPARE(decodedRelease->slotId, std::uint64_t(7));
        QCOMPARE(decodedRelease->leaseId, std::uint64_t(11));

        const Model::RequestMessage cancel { 44, Model::CancelOcrJobsRequest { } };
        const auto cancelBytes = ::Mu::IPC::ZppCodec::encode(cancel, &error);
        QVERIFY(cancelBytes);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*cancelBytes, &output, &error));
        QVERIFY(std::holds_alternative<Model::CancelOcrJobsRequest>(output.payload));

        const Model::RequestMessage ocr { 45, Model::OcrPageRequest { { 17 }, 3, 225, "eng", true } };
        const auto ocrBytes = ::Mu::IPC::ZppCodec::encode(ocr, &error);
        QVERIFY(ocrBytes);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*ocrBytes, &output, &error));
        const auto* decodedOcr = std::get_if<Model::OcrPageRequest>(&output.payload);
        QVERIFY(decodedOcr);
        QCOMPARE(decodedOcr->file.transferId, std::uint64_t(17));
        QCOMPARE(decodedOcr->page, 3);
    }

    void documentSettingsRoundTrip()
    {
        using namespace Mu;
        for (const auto pageSize : { Model::EpubPageSize::B5,
                                     Model::EpubPageSize::A5,
                                     Model::EpubPageSize::SixByNine,
                                     Model::EpubPageSize::Letter }) {
            Model::DocumentSettings settings;
            settings.graphicsAntialiasing = 4;
            settings.textAntialiasing = 6;
            settings.imageQuality = 2;
            settings.interpolateImages = false;
            settings.memoryCacheBytes = 128ULL * 1024ULL * 1024ULL;
            settings.epub.fontSize = 17;
            settings.epub.pageSize = pageSize;
            settings.epub.fontFamily = static_cast<Model::EpubFontFamily>(static_cast<std::uint8_t>(pageSize));
            settings.epub.customCssBase64 = "Ym9keSB7IGNvbG9yOiByZWQ7IH0=";
            std::string error;
            const auto encoded =
                ::Mu::IPC::ZppCodec::encode(Model::RequestMessage { 45, Model::SettingsRequest { settings } }, &error);
            QVERIFY(encoded);
            Model::RequestMessage decoded;
            QVERIFY(::Mu::IPC::ZppCodec::decode(*encoded, &decoded, &error));
            const auto& output = std::get<Model::SettingsRequest>(decoded.payload).settings;
            QCOMPARE(output.graphicsAntialiasing, 4);
            QCOMPARE(output.memoryCacheBytes, 128ULL * 1024ULL * 1024ULL);
            QCOMPARE(output.epub.fontSize, 17);
            QCOMPARE(output.epub.pageSize, pageSize);
            QCOMPARE(output.epub.fontFamily, settings.epub.fontFamily);
            QCOMPARE(output.epub.customCssBase64, settings.epub.customCssBase64);
        }

        Model::DocumentSettings invalid;
        invalid.epub.fontSize = 99;
        invalid.epub.pageSize = static_cast<Model::EpubPageSize>(255);
        invalid.epub.fontFamily = static_cast<Model::EpubFontFamily>(255);
        std::string error;
        const auto encoded =
            ::Mu::IPC::ZppCodec::encode(Model::RequestMessage { 46, Model::SettingsRequest { invalid } }, &error);
        QVERIFY(encoded);
        Model::RequestMessage decoded;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*encoded, &decoded, &error));
        const auto& output = std::get<Model::SettingsRequest>(decoded.payload).settings;
        QCOMPARE(output.epub.fontSize, invalid.epub.fontSize);
        QCOMPARE(output.epub.pageSize, invalid.epub.pageSize);
        QCOMPARE(output.epub.fontFamily, invalid.epub.fontFamily);

        invalid.epub.customCssBase64 = std::string(Model::MaxEpubCustomCssBase64Bytes + 1, 'A');
        const auto oversized =
            ::Mu::IPC::ZppCodec::encode(Model::RequestMessage { 47, Model::SettingsRequest { invalid } }, &error);
        QVERIFY(oversized);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*oversized, &decoded, &error));
    }

    void typedResponseNotificationRoundTrip()
    {
        using namespace Mu;
        Model::ResponseMessage response {
            7, Model::OcrResponse { { Model::OcrStatus::Success, { { "ok", 0, 0, 1, 1, true } } } }, std::nullopt
        };
        std::string error;
        const auto bytes = ::Mu::IPC::ZppCodec::encode(response, &error);
        QVERIFY(bytes);
        Model::ResponseMessage decoded;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*bytes, &decoded, &error));
        QCOMPARE(std::get<Model::OcrResponse>(decoded.payload).result.boxes.front().text, std::string("ok"));

        const Model::ResponseMessage renderResponse {
            8, Model::RenderResponse { { 13, 2, 5, 320, 240, 1280, Model::PixelFormatRgba8888 } }, std::nullopt
        };
        const auto renderBytes = ::Mu::IPC::ZppCodec::encode(renderResponse, &error);
        QVERIFY(renderBytes);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*renderBytes, &decoded, &error));
        const auto& frame = std::get<Model::RenderResponse>(decoded.payload).frame;
        QCOMPARE(frame.transferId, std::uint64_t(13));
        QCOMPARE(frame.slotId, std::uint64_t(2));
        QCOMPARE(frame.leaseId, std::uint64_t(5));

        Model::NotificationMessage notification { Model::SignInput { 3, "nonce", "cert", { 0, 1, 255 } } };
        const auto notificationBytes = ::Mu::IPC::ZppCodec::encode(notification, &error);
        QVERIFY(notificationBytes);
        Model::NotificationMessage notificationDecoded;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*notificationBytes, &notificationDecoded, &error));
        QCOMPARE(std::get<Model::SignInput>(notificationDecoded.payload).digest[2], std::uint8_t(255));
    }

    void typedOpenResponseRoundTrip()
    {
        using namespace Mu;
        const Model::RequestMessage request {
            7, Model::OpenRequest { { 12 }, "sample.epub", { }, Model::DocumentType::Epub, { 3, 4 } }
        };
        std::string error;
        const auto requestBytes = ::Mu::IPC::ZppCodec::encode(request, &error);
        QVERIFY(requestBytes);
        Model::RequestMessage decodedRequest;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*requestBytes, &decodedRequest, &error));
        QCOMPARE(std::get<Model::OpenRequest>(decodedRequest.payload).epubAccelerator,
                 std::vector<std::uint8_t>({ 3, 4 }));

        Model::PageInfo page;
        page.number = 0;
        page.geometry = { 612, 792, 1.5, { } };
        Model::ResponseMessage response { 8, Model::OpenResponse { { page }, 0, { 1, 2 } }, std::nullopt };
        const auto bytes = ::Mu::IPC::ZppCodec::encode(response, &error);
        QVERIFY(bytes);
        Model::ResponseMessage decoded;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*bytes, &decoded, &error));
        const auto& output = std::get<Model::OpenResponse>(decoded.payload);
        QCOMPARE(output.pages.size(), size_t(1));
        QCOMPARE(output.pages.front().geometry.duration, 1.5);
        QCOMPARE(output.epubAccelerator, std::vector<std::uint8_t>({ 1, 2 }));
    }

    void pageLinksNotificationRoundTrip()
    {
        using namespace Mu;
        Model::Link link;
        link.left = 0.1;
        link.right = 0.9;
        link.target.valid = true;
        const Model::NotificationMessage notification { Model::PageLinksNotification {
            7, { { 3, { link } } }, false, { } } };
        std::string error;
        const auto bytes = ::Mu::IPC::ZppCodec::encode(notification, &error);
        QVERIFY(bytes);
        Model::NotificationMessage decoded;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*bytes, &decoded, &error));
        const auto& output = std::get<Model::PageLinksNotification>(decoded.payload);
        QCOMPARE(output.generation, std::uint64_t(7));
        QCOMPARE(output.pages.size(), size_t(1));
        QCOMPARE(output.pages.front().page, 3);
        QCOMPARE(output.pages.front().links.size(), size_t(1));
    }

    void oversizedResponseReportsFrameLimit()
    {
        using namespace Mu;
        Model::EmbeddedFile file;
        file.data.resize(Limit::MaxFrameBytes, 0x7f);
        const Model::ResponseMessage response { 9, Model::EmbeddedFilesResponse { { std::move(file) } }, std::nullopt };
        std::string error;
        ::Mu::IPC::ZppCodec::EncodeError errorCode = ::Mu::IPC::ZppCodec::EncodeError::None;
        const auto bytes = ::Mu::IPC::ZppCodec::encode(response, &error, &errorCode);
        QVERIFY(!bytes);
        QCOMPARE(errorCode, ::Mu::IPC::ZppCodec::EncodeError::FrameLimit);
        QVERIFY(error.find("message exceeds frame limit") != std::string::npos);
    }
#ifdef MU_DEBUG_ENABLED
    void typedDebugFormattingIncludesPayload()
    {
        using namespace Mu;
        const Model::RequestMessage request {
            12,
            Model::SignRequest { { 9 }, 2, { 0.1, 0.2, 0.8, 0.9 }, "cert", "Subject", "reason", "location", -1, { } }
        };
        const auto requestText = ::Mu::IPC::Debug::request(request);
        QVERIFY(requestText.rfind("[sign] id=12", 0) == 0);
        QVERIFY(requestText.find("sign") != std::string::npos);
        QVERIFY(requestText.find("password") == std::string::npos);
        const Model::NotificationMessage notification { Model::SignInput { 4, "nonce", "cert", { 0, 1, 255 } } };
        const auto notificationText = ::Mu::IPC::Debug::notification(notification);
        QVERIFY(notificationText.rfind("[sign-input] job=4", 0) == 0);
        QVERIFY(notificationText.find("password") == std::string::npos);
        QVERIFY(notificationText.find("0x0001ff") == std::string::npos);

        Model::TextBoxesResponse textBoxes;
        for (int i = 0; i < 100; ++i)
            textBoxes.boxes.push_back({ std::string(80, 'x'), 0, 0, 1, 1, false });
        const auto responseText = ::Mu::IPC::Debug::response({ 13, textBoxes, std::nullopt });
        QVERIFY(responseText.rfind("[text-boxes] id=13 count=100", 0) == 0);
        QVERIFY(responseText.find("details=") == std::string::npos);
        QVERIFY(responseText.find("rect=") == std::string::npos);
        QVERIFY(responseText.find("...<truncated>") == std::string::npos);

        // Colorized debug format
        const auto colorRequestText = ::Mu::IPC::Debug::request(request, true);
        QVERIFY(colorRequestText.find("\033[1;94msign\033[0m") != std::string::npos);

        const auto releaseText = ::Mu::IPC::Debug::request({ 15, Model::ReleaseFrameSlotRequest { 2, 3 } }, true);
        QVERIFY(releaseText.find("\033[32mrelease-frame-slot\033[0m") != std::string::npos);

        const auto colorResponseText = ::Mu::IPC::Debug::response({ 13, textBoxes, std::nullopt }, true);
        QVERIFY(colorResponseText.find("\033[33mtext-boxes\033[0m") != std::string::npos);

        const auto errorResponseText = ::Mu::IPC::Debug::response(
            { 14, std::monostate { }, Model::Error { Model::ErrorCode::InvalidRequest, "op", "msg" } }, true);
        QVERIFY(errorResponseText.find("\033[1;31merror\033[0m") != std::string::npos);
    }
#endif
    void malformedFramesAreRejected()
    {
        using namespace Mu;
        Model::RequestMessage input { 1, Model::PingRequest { "test" } };
        std::string error;
        auto bytes = ::Mu::IPC::ZppCodec::encode(input, &error);
        QVERIFY(bytes);
        bytes->at(0) = std::byte { 0 };
        Model::RequestMessage output;
        QVERIFY(!::Mu::IPC::ZppCodec::decode(*bytes, &output, &error));
        bytes = ::Mu::IPC::ZppCodec::encode(input, &error);
        bytes->push_back(std::byte { 1 });
        QVERIFY(!::Mu::IPC::ZppCodec::decode(*bytes, &output, &error));
    }

    void protocolEnvelopeValidation()
    {
        using namespace Mu;
        std::string error;

        const auto zeroRequest =
            ::Mu::IPC::ZppCodec::encode(Model::RequestMessage { 0, Model::PingRequest { } }, &error);
        QVERIFY(zeroRequest);
        Model::RequestMessage requestOutput { 9, Model::CloseRequest { } };
        QVERIFY(!::Mu::IPC::ZppCodec::decode(*zeroRequest, &requestOutput, &error));
        QCOMPARE(requestOutput.id, std::uint64_t(9));
        QVERIFY(std::holds_alternative<Model::CloseRequest>(requestOutput.payload));

        const Model::ResponseMessage contradictory {
            10,
            Model::PingResponse { "0.6.0", 1, { } },
            Model::Error { Model::ErrorCode::Internal, "test", "error" },
        };
        const auto contradictoryBytes = ::Mu::IPC::ZppCodec::encode(contradictory, &error);
        QVERIFY(contradictoryBytes);
        Model::ResponseMessage responseOutput { 11, std::monostate { }, std::nullopt };
        QVERIFY(!::Mu::IPC::ZppCodec::decode(*contradictoryBytes, &responseOutput, &error));
        QCOMPARE(responseOutput.id, std::uint64_t(11));
        QVERIFY(std::holds_alternative<std::monostate>(responseOutput.payload));

        const Model::NotificationMessage negativePage { Model::PageLinksNotification {
            1, { { -1, { } } }, false, { } } };
        const auto negativePageBytes = ::Mu::IPC::ZppCodec::encode(negativePage, &error);
        QVERIFY(negativePageBytes);
        Model::NotificationMessage notificationOutput { Model::SignInput { 1, "nonce", "cert", { } } };
        QVERIFY(!::Mu::IPC::ZppCodec::decode(*negativePageBytes, &notificationOutput, &error));
        QVERIFY(std::holds_alternative<Model::SignInput>(notificationOutput.payload));

        const Model::NotificationMessage outOfRangePage { Model::PageLinksNotification {
            1, { { 999999, { } } }, false, { } } };
        const auto outOfRangePageBytes = ::Mu::IPC::ZppCodec::encode(outOfRangePage, &error);
        QVERIFY(outOfRangePageBytes);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*outOfRangePageBytes, &notificationOutput, &error));
    }

    void annotationCodecPreservesFiniteGeometry()
    {
        ::Mu::Model::Annotation annotation;
        annotation.x0 = -5.0;
        annotation.y0 = 2.0;
        annotation.x1 = 5.0;
        annotation.y1 = -2.0;
        annotation.extras.points.push_back({ -1.0, 2.0 });
        annotation.extras.caretSymbolP = true;
        std::string error;
        const auto encoded = ::Mu::IPC::ZppCodec::encode(
            ::Mu::Model::RequestMessage { 47, ::Mu::Model::AnnotationAddRequest { 0, annotation } }, &error);
        QVERIFY2(encoded, error.c_str());
        ::Mu::Model::RequestMessage decoded;
        QVERIFY2(::Mu::IPC::ZppCodec::decode(*encoded, &decoded, &error), error.c_str());
        const auto& decodedAnnotation = std::get<::Mu::Model::AnnotationAddRequest>(decoded.payload).annotation;
        QCOMPARE(decodedAnnotation.x0, annotation.x0);
        QCOMPARE(decodedAnnotation.y0, annotation.y0);
        QCOMPARE(decodedAnnotation.x1, annotation.x1);
        QCOMPARE(decodedAnnotation.y1, annotation.y1);
        QCOMPARE(decodedAnnotation.extras.points.front().x, annotation.extras.points.front().x);
        QVERIFY(decodedAnnotation.extras.caretSymbolP);
    }

    void commandServiceDispatchesTypedPing()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        const auto response = service.dispatch({ 9, ::Mu::Model::PingRequest { "test" } });
        QVERIFY(!response.error);
        QVERIFY(std::get_if<::Mu::Model::PingResponse>(&response.payload));
        QCOMPARE(std::get<::Mu::Model::PingResponse>(response.payload).compat, std::string(::Mu::IPC::COMPAT));

        const auto invalidRelease = service.dispatch({ 10, ::Mu::Model::ReleaseFrameSlotRequest { } });
        QVERIFY(invalidRelease.error);
        QCOMPARE(invalidRelease.error->code, ::Mu::Model::ErrorCode::InvalidRequest);

        const auto staleRelease = service.dispatch({ 11, ::Mu::Model::ReleaseFrameSlotRequest { 1, 1 } });
        QVERIFY(!staleRelease.error);
    }

    void validationHelpers()
    {
        using namespace ::Mu::Model;

        // Render Tile validation
        QVERIFY(isValidRenderTile(1000, 1000, 0, 0, 1000, 1000));
        QVERIFY(isValidRenderTile(1000, 1000, 100, 100, 200, 200));
        QVERIFY(!isValidRenderTile(1000, 1000, -1, 0, 100, 100));
        QVERIFY(!isValidRenderTile(1000, 1000, 0, -1, 100, 100));
        QVERIFY(!isValidRenderTile(1000, 1000, 0, 0, 0, 100));
        QVERIFY(!isValidRenderTile(1000, 1000, 0, 0, 100, 0));
        QVERIFY(!isValidRenderTile(1000, 1000, 900, 900, 200, 200));
        QVERIFY(!isValidRenderTile(1000, 1000, 1001, 0, 10, 10));

        // DPI validation
        QVERIFY(isValidDpi(72.0, 72.0));
        QVERIFY(isValidDpi(300.0, 300.0));
        QVERIFY(!isValidDpi(0.0, 72.0));
        QVERIFY(!isValidDpi(72.0, -1.0));
        QVERIFY(!isValidDpi(std::numeric_limits<double>::quiet_NaN(), 72.0));

        // OCR DPI validation
        QVERIFY(isValidOcrDpi(72.0f));
        QVERIFY(isValidOcrDpi(300.0f));
        QVERIFY(isValidOcrDpi(600.0f));
        QVERIFY(!isValidOcrDpi(71.9f));
        QVERIFY(!isValidOcrDpi(600.1f));
        QVERIFY(!isValidOcrDpi(std::numeric_limits<float>::quiet_NaN()));

        // Annotation validation
        Annotation validAnnot;
        validAnnot.x0 = 0.1;
        validAnnot.y0 = 0.1;
        validAnnot.x1 = 0.9;
        validAnnot.y1 = 0.9;
        validAnnot.contents = "Test annotation";
        validAnnot.author = "Author";
        validAnnot.uuid = "uuid-1234";
        std::string_view reason;
        QVERIFY(isValidAnnotation(validAnnot, &reason));

        Annotation invalidCoord = validAnnot;
        invalidCoord.x0 = -0.1;
        QVERIFY(isValidAnnotation(invalidCoord, &reason));

        Annotation invertedCoord = validAnnot;
        invertedCoord.x0 = 0.9;
        invertedCoord.x1 = 0.1;
        QVERIFY(isValidAnnotation(invertedCoord, &reason));

        Annotation invalidUtf8 = validAnnot;
        invalidUtf8.contents = "\xff\xfe";
        QVERIFY(!isValidAnnotation(invalidUtf8, &reason));

        Annotation embeddedNul = validAnnot;
        embeddedNul.author = std::string("author\0suffix", 13);
        QVERIFY(!isValidAnnotation(embeddedNul, &reason));
    }
};

int runTestWorkerNativeTypes(int argc, char** argv)
{
    TestNativeWorkerTypes test;
    return QTest::qExec(&test, argc, argv);
}

#include "native_types.moc"
