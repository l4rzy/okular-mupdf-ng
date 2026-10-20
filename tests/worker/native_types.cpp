#include "engine/mupdf_helpers.hpp"
#include "engine/signer.hpp"
#include "runtime/command_service.hpp"
#include "runtime/render_budget.hpp"
#include "shared/compat.hpp"
#include "shared/model/geometry.hpp"
#include "shared/model/types.hpp"
#include "shared/model/validation.hpp"
#include "shared/protocol/ipc_debug.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include <QBuffer>
#include <QImage>
#include <QTest>
#include <cmath>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mupdf/fitz/version.h>
#include <unistd.h>

namespace {

int failStreamRead(fz_context* context, fz_stream*, std::size_t)
{
    fz_throw(context, FZ_ERROR_SYSTEM, "injected stream read failure");
}

struct FailingFitzAllocator {
    fz_alloc_context original;
    unsigned failAt;
    unsigned calls = 0;

    bool reject() { return ++calls >= failAt && failAt != 0; }

    static void* allocate(void* user, std::size_t size)
    {
        auto& state = *static_cast<FailingFitzAllocator*>(user);
        return state.reject() ? nullptr : state.original.malloc(state.original.user, size);
    }

    static void* resize(void* user, void* pointer, std::size_t size)
    {
        auto& state = *static_cast<FailingFitzAllocator*>(user);
        return state.reject() ? nullptr : state.original.realloc(state.original.user, pointer, size);
    }

    static void release(void* user, void* pointer)
    {
        auto& state = *static_cast<FailingFitzAllocator*>(user);
        state.original.free(state.original.user, pointer);
    }
};

} // namespace

class TestNativeWorkerTypes : public QObject {
    Q_OBJECT
private slots:

    void nonFiniteValidation_data()
    {
        QTest::addColumn<double>("value");
        QTest::newRow("nan") << std::numeric_limits<double>::quiet_NaN();
        QTest::newRow("positive-infinity") << std::numeric_limits<double>::infinity();
        QTest::newRow("negative-infinity") << -std::numeric_limits<double>::infinity();
    }

    void nonFiniteValidation()
    {
        QFETCH(double, value);
        QVERIFY(!Mu::Model::isValidDpi(value, 72));
        QVERIFY(!Mu::Model::isValidDpi(72, value));
        QVERIFY(!Mu::Model::isValidOcrDpi(static_cast<float>(value)));
        for (const Mu::Model::NormalizedRect& rect : { Mu::Model::NormalizedRect { value, 0, 1, 1 },
                                                       { 0, value, 1, 1 },
                                                       { 0, 0, value, 1 },
                                                       { 0, 0, 1, value } })
            QVERIFY(!Mu::Model::isValidNormalizedRect(rect));
    }

    void signingStreamErrors_data()
    {
        QTest::addColumn<bool>("readError");
        QTest::newRow("complete") << false;
        QTest::newRow("error-after-prefix") << true;
    }

    void signingStreamErrors()
    {
        QFETCH(bool, readError);
        auto* context = fz_new_context(nullptr, nullptr, 0);
        QVERIFY(context);
        auto* stream = fz_open_memory(context, reinterpret_cast<const unsigned char*>("abc"), 3);
        if (readError)
            stream->next = failStreamRead;
        unsigned callbackCount = 0;
        std::array<std::uint8_t, 32> digest { };
        auto* signer = ::Mu::Worker::Engine::createSigner("test", "Test", [&](const auto& hash, const auto&) {
            ++callbackCount;
            digest = hash;
            return ::Mu::Worker::Engine::CmsResult { ::Mu::Model::SigningResult::Success, { }, { 1, 2, 3 } };
        });
        unsigned char output[3] { };
        const int written = signer->create_digest(context, signer, stream, output, sizeof(output));
        pdf_drop_signer(context, signer);
        fz_drop_stream(context, stream);
        fz_drop_context(context);
        QCOMPARE(written, readError ? 0 : 3);
        QCOMPARE(callbackCount, readError ? 0u : 1u);
        if (!readError)
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(digest.data()), digest.size()).toHex(),
                     QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }

    void outputFileOwnership_data()
    {
        QTest::addColumn<unsigned>("failAt");
        QTest::newRow("success") << 0u;
        QTest::newRow("output-allocation-fails") << 1u;
        QTest::newRow("buffer-allocation-fails") << 2u;
    }

    void outputFileOwnership()
    {
        QFETCH(unsigned, failAt);
        auto* context = fz_new_context(nullptr, nullptr, 0);
        QVERIFY(context);
        FILE* volatile file = ::tmpfile();
        QVERIFY(file);
        const int fd = ::fileno(file);
        const auto state = std::make_unique<FailingFitzAllocator>(context->alloc, failAt);
        context->alloc = {
            state.get(), FailingFitzAllocator::allocate, FailingFitzAllocator::resize, FailingFitzAllocator::release
        };
        fz_output* volatile output = nullptr;
        volatile bool failed = false;
        fz_try(context)
        {
            output = ::Mu::Worker::Engine::takeOutputFile(context, file);
            fz_close_output(context, output);
        }
        fz_always(context)
        {
            fz_drop_output(context, output);
            context->alloc = state->original;
        }
        fz_catch(context)
        {
            failed = true;
            fz_ignore_error(context);
        }
        fz_drop_context(context);
        QVERIFY(file == nullptr);
        QVERIFY(::fcntl(fd, F_GETFD) == -1);
        QCOMPARE(static_cast<bool>(failed), failAt != 0);
    }

    void pingCompatibilityRoundTrip_data()
    {
        QTest::addColumn<bool>("systemMuPdf");
        QTest::newRow("bundled") << false;
        QTest::newRow("system") << true;
    }

    void pingCompatibilityRoundTrip()
    {
        QFETCH(bool, systemMuPdf);
        using namespace Mu;
        const Model::RequestMessage input { 5, Model::PingRequest { "0.2.3" } };
        std::string error;
        const auto requestBytes = ::Mu::IPC::ZppCodec::encode(input, &error);
        QVERIFY(requestBytes);
        Model::RequestMessage decodedRequest;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*requestBytes, &decodedRequest, &error));
        QCOMPARE(std::get<Model::PingRequest>(decodedRequest.payload).compat, std::string("0.2.3"));

        const Model::ResponseMessage response {
            5, Model::PingResponse { std::string(::Mu::IPC::COMPAT), 123, { }, "1.26.0", systemMuPdf }, std::nullopt
        };
        const auto responseBytes = ::Mu::IPC::ZppCodec::encode(response, &error);
        QVERIFY(responseBytes);
        Model::ResponseMessage decodedResponse;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*responseBytes, &decodedResponse, &error));
        QCOMPARE(std::get<Model::PingResponse>(decodedResponse.payload).compat, std::string(::Mu::IPC::COMPAT));
        QCOMPARE(std::get<Model::PingResponse>(decodedResponse.payload).engineVersion, std::string("1.26.0"));
        QCOMPARE(std::get<Model::PingResponse>(decodedResponse.payload).systemMuPdf, systemMuPdf);
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

        const Model::RequestMessage ocr { 45, Model::OcrPageRequest { { 17 }, 3, 225, "eng" } };
        const auto ocrBytes = ::Mu::IPC::ZppCodec::encode(ocr, &error);
        QVERIFY(ocrBytes);
        QVERIFY(::Mu::IPC::ZppCodec::decode(*ocrBytes, &output, &error));
        const auto* decodedOcr = std::get_if<Model::OcrPageRequest>(&output.payload);
        QVERIFY(decodedOcr);
        QCOMPARE(decodedOcr->file.transferId, std::uint64_t(17));
        QCOMPARE(decodedOcr->page, 3);
    }

    void savePdfModesRoundTrip_data()
    {
        QTest::addColumn<bool>("flatten");
        QTest::addColumn<bool>("withReferences");
        QTest::newRow("print") << false << false;
        QTest::newRow("flatten") << true << false;
        QTest::newRow("epub-export") << false << true;
    }

    void savePdfModesRoundTrip()
    {
        QFETCH(bool, flatten);
        QFETCH(bool, withReferences);
        const Mu::Model::RequestMessage request {
            48, Mu::Model::SavePdfRequest { { 19 }, { 2, 0 }, flatten, withReferences }
        };
        std::string error;
        const auto encoded = Mu::IPC::ZppCodec::encode(request, &error);
        QVERIFY2(encoded, error.c_str());
        Mu::Model::RequestMessage decoded;
        QVERIFY2(Mu::IPC::ZppCodec::decode(*encoded, &decoded, &error), error.c_str());
        const auto* result = std::get_if<Mu::Model::SavePdfRequest>(&decoded.payload);
        QVERIFY(result);
        QCOMPARE(result->file.transferId, std::uint64_t(19));
        QCOMPARE(result->pages, std::vector<std::int32_t>({ 2, 0 }));
        QCOMPARE(result->flatten, flatten);
        QCOMPARE(result->withReferences, withReferences);
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
            settings.idleTrimAggressiveness = Model::IdleTrimLevel::Aggressive;
            settings.readingTheme = Model::ReadingTheme::Dark;
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
            QCOMPARE(output.readingTheme, Model::ReadingTheme::Dark);
            QCOMPARE(output.memoryCacheBytes, 128ULL * 1024ULL * 1024ULL);
            QCOMPARE(output.idleTrimAggressiveness, Model::IdleTrimLevel::Aggressive);
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

    void oversizedResponseReportsControlMessageLimit()
    {
        using namespace Mu;
        Model::EmbeddedFile file;
        file.data.resize(Limit::MaxControlMessageBytes, 0x7f);
        const Model::ResponseMessage response { 9, Model::EmbeddedFilesResponse { { std::move(file) } }, std::nullopt };
        std::string error;
        ::Mu::IPC::ZppCodec::EncodeError errorCode = ::Mu::IPC::ZppCodec::EncodeError::None;
        const auto bytes = ::Mu::IPC::ZppCodec::encode(response, &error, &errorCode);
        QVERIFY(!bytes);
        QCOMPARE(errorCode, ::Mu::IPC::ZppCodec::EncodeError::ControlMessageLimit);
        QVERIFY(error.find("message exceeds control-message limit") != std::string::npos);
    }
#ifdef MU_DEBUG_ENABLED
    void typedDebugFormattingIncludesPayload()
    {
        using namespace Mu;
        const Model::RequestMessage request { 12,
                                              Model::SignRequest { { 9 },
                                                                   2,
                                                                   { 0.1, 0.2, 0.8, 0.9 },
                                                                   "cert",
                                                                   "Subject",
                                                                   -1,
                                                                   { .elements = Model::SignatureElementDefault,
                                                                     .reason = "reason",
                                                                     .location = "location",
                                                                     .signingEpochSeconds = 1'788'804'840,
                                                                     .signingDisplayDate = "Sep 7, 2026 13:14 CDT",
                                                                     .backgroundImage = { } } } };
        const auto requestText = ::Mu::IPC::Debug::request(request);
        QVERIFY(requestText.rfind("[sign] id=12", 0) == 0);
        QVERIFY(requestText.find("sign") != std::string::npos);
        QVERIFY(requestText.find("elements=\"0x7f\"") != std::string::npos);
        QVERIFY(requestText.find("displayDate=\"Sep 7, 2026 13:14 CDT\"") != std::string::npos);
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
            Model::PingResponse { "0.6.0", 1, { }, { } },
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

    void annotationCodecPreservesProtocolValues()
    {
        ::Mu::Model::Annotation annotation;
        annotation.subtype = ::Mu::Model::AnnotationType::Projection;
        annotation.flags = ::Mu::Model::annotationFlagValue(::Mu::Model::AnnotationFlag::Invisible)
            | ::Mu::Model::annotationFlagValue(::Mu::Model::AnnotationFlag::LockedContents);
        annotation.extras.style.intent = ::Mu::Model::AnnotationIntent::StampSnapshot;
        annotation.extras.style.firstLineEnding = ::Mu::Model::AnnotationLineEnding::Slash;
        annotation.extras.style.lastLineEnding = ::Mu::Model::AnnotationLineEnding::RClosedArrow;

        std::string error;
        const auto encoded = ::Mu::IPC::ZppCodec::encode(
            ::Mu::Model::RequestMessage { 48, ::Mu::Model::AnnotationAddRequest { 0, annotation } }, &error);
        QVERIFY2(encoded, error.c_str());
        ::Mu::Model::RequestMessage decoded;
        QVERIFY2(::Mu::IPC::ZppCodec::decode(*encoded, &decoded, &error), error.c_str());
        const auto& decodedAnnotation = std::get<::Mu::Model::AnnotationAddRequest>(decoded.payload).annotation;
        QCOMPARE(decodedAnnotation.subtype, ::Mu::Model::AnnotationType::Projection);
        QCOMPARE(decodedAnnotation.flags, annotation.flags);
        QCOMPARE(*decodedAnnotation.extras.style.intent, ::Mu::Model::AnnotationIntent::StampSnapshot);
        QCOMPARE(*decodedAnnotation.extras.style.firstLineEnding, ::Mu::Model::AnnotationLineEnding::Slash);
        QCOMPARE(*decodedAnnotation.extras.style.lastLineEnding, ::Mu::Model::AnnotationLineEnding::RClosedArrow);
    }

    void commandServiceDispatchesTypedPing()
    {
        ::Mu::Worker::Runtime::CommandService service({ });
        const auto response = service.dispatch({ 9, ::Mu::Model::PingRequest { "test" } });
        QVERIFY(!response.error);
        QVERIFY(std::get_if<::Mu::Model::PingResponse>(&response.payload));
        QCOMPARE(std::get<::Mu::Model::PingResponse>(response.payload).compat, std::string(::Mu::IPC::COMPAT));
        QCOMPARE(std::get<::Mu::Model::PingResponse>(response.payload).engineVersion, std::string(FZ_VERSION));

        const auto invalidRelease = service.dispatch({ 10, ::Mu::Model::ReleaseFrameSlotRequest { } });
        QVERIFY(invalidRelease.error);
        QCOMPARE(invalidRelease.error->code, ::Mu::Model::ErrorCode::InvalidRequest);

        const auto staleRelease = service.dispatch({ 11, ::Mu::Model::ReleaseFrameSlotRequest { 1, 1 } });
        QVERIFY(!staleRelease.error);
    }

    void commandServiceValidatesRequestsBeforeHandlers()
    {
        using namespace ::Mu;
        Worker::Runtime::CommandService service({ });

        const auto invalidRender = service.dispatch({ 12, Model::RenderRequest { 0, 0, 100, std::nullopt } });
        QVERIFY(invalidRender.error);
        QCOMPARE(invalidRender.error->code, Model::ErrorCode::InvalidRequest);
        QCOMPARE(invalidRender.error->operation, std::string("render"));

        // Geometry over the hard cap is request-level invalid before a document
        // is consulted; the render frame budget still enforces resource limits.
        const auto oversizedRender = service.dispatch(
            { 17, Model::RenderRequest { 0, ::Mu::Limit::MaxRenderDimension + 1, 100, std::nullopt } });
        QVERIFY(oversizedRender.error);
        QCOMPARE(oversizedRender.error->code, Model::ErrorCode::InvalidRequest);
        QCOMPARE(oversizedRender.error->operation, std::string("render"));

        Model::DocumentSettings invalidSettings;
        invalidSettings.epub.fontSize = 99;
        const auto invalidSettingsResponse = service.dispatch({ 13, Model::SettingsRequest { invalidSettings } });
        QVERIFY(invalidSettingsResponse.error);
        QCOMPARE(invalidSettingsResponse.error->code, Model::ErrorCode::InvalidRequest);
        QCOMPARE(invalidSettingsResponse.error->operation, std::string("settings"));

        const auto invalidForm = service.dispatch({ 14, Model::FormUpdateRequest { { }, Model::FormTextValue { } } });
        QVERIFY(invalidForm.error);
        QCOMPARE(invalidForm.error->code, Model::ErrorCode::InvalidRequest);
        QCOMPARE(invalidForm.error->operation, std::string("form_update"));

        const auto invalidButton = service.dispatch({ 18, Model::FormButtonClickRequest { } });
        QVERIFY(invalidButton.error);
        QCOMPARE(invalidButton.error->code, Model::ErrorCode::InvalidRequest);
        QCOMPARE(invalidButton.error->operation, std::string("form_button_click"));

        const auto invalidSign = service.dispatch({ 15, Model::SignRequest { } });
        QVERIFY(invalidSign.error);
        QCOMPARE(invalidSign.error->code, Model::ErrorCode::InvalidRequest);
        QCOMPARE(invalidSign.error->operation, std::string("sign"));

        const auto validSettingsResponse = service.dispatch({ 16, Model::SettingsRequest { } });
        QVERIFY(!validSettingsResponse.error);
    }

    void validationHelpers()
    {
        using namespace ::Mu::Model;

        // Request-level validation is shared by the plugin and worker. The
        // worker-side command test above verifies that these rules run before
        // document state or descriptor ownership is consulted.
        std::string_view reason;
        QVERIFY(isValidFileTransfer({ 1 }, &reason));
        QVERIFY(!isValidFileTransfer({ 0 }, &reason));

        QVERIFY(isValidRenderRequest({ 0, 640, 480, RenderTile { 10, 10, 100, 100 } }, &reason));
        QVERIFY(!isValidRenderRequest({ 0, 0, 480, std::nullopt }, &reason));
        QVERIFY(!isValidRenderRequest({ 0, 640, 480, RenderTile { 600, 10, 100, 100 } }, &reason));

        QVERIFY(isValidTextBoxesRequest({ 0, 72, 144, false }, &reason));
        QVERIFY(!isValidTextBoxesRequest({ 0, 0, 144, false }, &reason));
        QVERIFY(isValidOcrPageRequest({ { 1 }, 0, 225, "eng" }, &reason));
        QVERIFY(!isValidOcrPageRequest({ { 0 }, 0, 225, "eng" }, &reason));

        DocumentSettings invalidTheme;
        invalidTheme.readingTheme = static_cast<ReadingTheme>(255);
        QVERIFY(!isValidDocumentSettings(invalidTheme, &reason));
        invalidTheme.readingTheme = static_cast<ReadingTheme>(4);
        QVERIFY(!isValidDocumentSettings(invalidTheme, &reason));
        DocumentSettings validSettings;
        QVERIFY(isValidDocumentSettings(validSettings, &reason));
        validSettings.readingTheme = ReadingTheme::Twilight;
        QVERIFY(isValidDocumentSettings(validSettings, &reason));
        validSettings.readingTheme = ReadingTheme::Dusk;
        QVERIFY(isValidDocumentSettings(validSettings, &reason));
        validSettings.memoryCacheBytes = ::Mu::Limit::MaxDocumentMemoryCacheBytes + 1;
        QVERIFY(!isValidDocumentSettings(validSettings, &reason));

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

        // EPUB custom CSS base64 validation
        QVERIFY(isValidEpubCustomCssBase64(""));
        QVERIFY(isValidEpubCustomCssBase64("AA=="));
        QVERIFY(isValidEpubCustomCssBase64("QUJD"));
        QVERIFY(isValidEpubCustomCssBase64("QUJDRA=="));
        // Padding is optional, but '=' must sit in the last two positions only.
        QVERIFY(!isValidEpubCustomCssBase64("A="));
        QVERIFY(!isValidEpubCustomCssBase64("AB=C"));
        QVERIFY(!isValidEpubCustomCssBase64("AAAA===="));
        QVERIFY(!isValidEpubCustomCssBase64("AAA=AAAA"));
        // URL-safe alphabet and whitespace are rejected.
        QVERIFY(!isValidEpubCustomCssBase64("A-B_"));
        QVERIFY(!isValidEpubCustomCssBase64("AAA "));
        QVERIFY(!isValidEpubCustomCssBase64("\nAAA"));
        // Alignment and the stored byte cap.
        QVERIFY(!isValidEpubCustomCssBase64("AAAAA"));
        QVERIFY(!isValidEpubCustomCssBase64(std::string(Mu::Limit::MaxEpubCustomCssBase64Bytes + 1, 'A')));
        QVERIFY(isValidEpubCustomCssBase64(std::string(Mu::Limit::MaxEpubCustomCssBase64Bytes, 'A')));

        // Generic base64 syntax carries no size cap.
        QVERIFY(isValidBase64(""));
        QVERIFY(isValidBase64("QUJDRA=="));
        QVERIFY(isValidBase64(std::string(Mu::Limit::MaxEpubCustomCssBase64Bytes + 8, 'A')));
        QVERIFY(!isValidBase64("A="));
        QVERIFY(!isValidEpubCustomCssBase64(std::string(Mu::Limit::MaxEpubCustomCssBase64Bytes + 8, 'A')));

        // Annotation validation
        Annotation validAnnot;
        validAnnot.x0 = 0.1;
        validAnnot.y0 = 0.1;
        validAnnot.x1 = 0.9;
        validAnnot.y1 = 0.9;
        validAnnot.contents = "Test annotation";
        validAnnot.author = "Author";
        validAnnot.uuid = "uuid-1234";
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

        QVERIFY(isValidAnnotationAddRequest({ 0, validAnnot }, &reason));
        QVERIFY(isValidAnnotationModifyRequest({ { 0, { "annotation" }, validAnnot, true } }, &reason));
        QVERIFY(isValidAnnotationRemoveRequest({ 0, { "annotation" } }, &reason));
        QVERIFY(!isValidAnnotationRemoveRequest({ 0, { } }, &reason));

        SignRequest sign;
        sign.file.transferId = 1;
        sign.page = 0;
        sign.rectangle = { 0.1, 0.1, 0.9, 0.9 };
        sign.certificateNickname = "certificate";
        QVERIFY(isValidSignRequest(sign, &reason));
        sign.appearance.elements = 0x80;
        QVERIFY(!isValidSignRequest(sign, &reason));
    }

    void validatesStampImage_data()
    {
        QTest::addColumn<QByteArray>("png");
        QTest::addColumn<bool>("supplied");
        QTest::addColumn<bool>("stamp");
        QTest::addColumn<bool>("valid");
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(QColor(255, 0, 0, 128));
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        QTest::newRow("preserve") << QByteArray { } << false << true << true;
        QTest::newRow("native") << QByteArray { } << true << true << true;
        QTest::newRow("bitmap") << png << true << true << true;
        QTest::newRow("non-stamp-bitmap") << png << true << false << false;
        QTest::newRow("non-stamp-clear") << QByteArray { } << true << false << false;
        QTest::newRow("not-png") << QByteArray("invalid") << true << true << false;
        QTest::newRow("truncated") << png.left(32) << true << true << false;
        auto invalid = png;
        invalid[16] = 1;
        QTest::newRow("oversized-width") << invalid << true << true << false;
        invalid = png;
        invalid[20] = 1;
        QTest::newRow("oversized-height") << invalid << true << true << false;
        invalid = png;
        for (int i = 16; i < 20; ++i)
            invalid[i] = 0;
        QTest::newRow("zero-width") << invalid << true << true << false;
        invalid = png;
        invalid.resize(Mu::Limit::MaxStampImageBytes + 1);
        QTest::newRow("oversized-bytes") << invalid << true << true << false;
    }

    void validatesStampImage()
    {
        using namespace Mu;
        QFETCH(QByteArray, png);
        QFETCH(bool, supplied);
        QFETCH(bool, stamp);
        QFETCH(bool, valid);
        Model::Annotation annotation;
        annotation.subtype = stamp ? Model::AnnotationType::Stamp : Model::AnnotationType::Text;
        if (supplied)
            annotation.extras.stampImage.emplace(png.begin(), png.end());
        QCOMPARE(Model::isValidAnnotation(annotation), valid);
        for (const Model::RequestPayload& payload :
             { Model::RequestPayload(Model::AnnotationAddRequest { 0, annotation }),
               Model::RequestPayload(Model::AnnotationModifyRequest { { 0, { "stamp" }, annotation, true } }) }) {
            std::string error;
            const auto encoded = IPC::ZppCodec::encode(Model::RequestMessage { 1, payload }, &error);
            QVERIFY2(encoded, error.c_str());
            Model::RequestMessage decoded;
            QVERIFY2(IPC::ZppCodec::decode(*encoded, &decoded, &error), error.c_str());
            const auto& restored = std::holds_alternative<Model::AnnotationAddRequest>(decoded.payload)
                ? std::get<Model::AnnotationAddRequest>(decoded.payload).annotation
                : std::get<Model::AnnotationModifyRequest>(decoded.payload).mutation.annotation;
            QCOMPARE(restored.extras.stampImage, annotation.extras.stampImage);
            QCOMPARE(Model::isValidAnnotation(restored), valid);
        }
    }

    void validatesSignatureEmblem_data()
    {
        using namespace Mu;
        QTest::addColumn<QByteArray>("png");
        QTest::addColumn<bool>("nativeLogo");
        QTest::addColumn<bool>("valid");
        QImage image(2, 2, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        QTest::newRow("none") << QByteArray { } << false << true;
        QTest::newRow("native") << QByteArray { } << true << true;
        QTest::newRow("custom") << png << false << true;
        QTest::newRow("both") << png << true << false;
        QTest::newRow("not-png") << QByteArray("not a PNG") << false << false;
        QTest::newRow("truncated-header") << png.left(32) << false << false;
        auto invalid = png;
        invalid[12] = 'X';
        QTest::newRow("wrong-header") << invalid << false << false;
        invalid = png;
        for (int i = 16; i < 20; ++i)
            invalid[i] = 0;
        QTest::newRow("zero-width") << invalid << false << false;
        invalid = png;
        invalid[18] = 4;
        invalid[19] = 1;
        QTest::newRow("oversized-width") << invalid << false << false;
        invalid = png;
        invalid[22] = 4;
        invalid[23] = 1;
        QTest::newRow("oversized-height") << invalid << false << false;
        invalid = png;
        invalid.resize(Limit::MaxSignatureEmblemBytes + 1);
        QTest::newRow("oversized-bytes") << invalid << false << false;
    }

    void validatesSignatureEmblem()
    {
        using namespace Mu;
        QFETCH(QByteArray, png);
        QFETCH(bool, nativeLogo);
        QFETCH(bool, valid);
        Model::SignRequest request;
        request.file.transferId = 1;
        request.page = 0;
        request.rectangle = { 0.1, 0.1, 0.9, 0.9 };
        request.appearance.elements = nativeLogo ? Model::SignatureElementDefault : Model::SignatureElementSimple;
        request.appearance.emblemImage.assign(png.begin(), png.end());
        QCOMPARE(Model::isValidSignRequest(request), valid);
        if (valid) {
            const Model::RequestMessage message { 1, request };
            std::string error;
            const auto bytes = IPC::ZppCodec::encode(message, &error);
            QVERIFY2(bytes, error.c_str());
            Model::RequestMessage decoded;
            QVERIFY2(IPC::ZppCodec::decode(*bytes, &decoded, &error), error.c_str());
            const auto& appearance = std::get<Model::SignRequest>(decoded.payload).appearance;
            QCOMPARE(appearance.elements, request.appearance.elements);
            QCOMPARE(appearance.emblemImage, request.appearance.emblemImage);
        }
    }

    void renderRequestsFitSharedFrameBudget()
    {
        using namespace ::Mu;

        const Model::RenderRequest withinBudget { 1, 8'000, 4'000, std::nullopt };
        const auto unchanged = Worker::Runtime::fitRenderRequestToFrameBudget(withinBudget);
        QCOMPARE(unchanged.request.width, withinBudget.width);
        QCOMPARE(unchanged.request.height, withinBudget.height);
        QCOMPARE(unchanged.frameDataBytes, std::uint64_t(8'000) * 4'000 * 4U);

        const Model::RenderRequest fullPage { 2, 16'384, 16'384, std::nullopt };
        const auto fullFallback = Worker::Runtime::fitRenderRequestToFrameBudget(fullPage);
        QVERIFY(fullFallback.frameWidth < static_cast<std::uint32_t>(fullPage.width));
        QVERIFY(fullFallback.frameWidth > 0);
        QVERIFY(fullFallback.frameHeight > 0);
        QCOMPARE(fullFallback.request.width, static_cast<std::int32_t>(fullFallback.frameWidth));
        QCOMPARE(fullFallback.request.height, static_cast<std::int32_t>(fullFallback.frameHeight));
        QVERIFY(sizeof(IPC::FrameBufferHeader) + fullFallback.frameDataBytes <= Limit::MaxSharedFrameBytes);

        const Model::RenderRequest tiled {
            3, 2'000'000, 1'000'000, Model::RenderTile { 500'000, 250'000, 500'000, 500'000 }
        };
        const auto tileFallback = Worker::Runtime::fitRenderRequestToFrameBudget(tiled);
        QVERIFY(tileFallback.frameWidth < static_cast<std::uint32_t>(tiled.tile->width));
        QVERIFY(tileFallback.request.tile);
        const auto& scaledTile = *tileFallback.request.tile;
        QVERIFY(Model::isValidRenderTile(tileFallback.request.width,
                                         tileFallback.request.height,
                                         scaledTile.x,
                                         scaledTile.y,
                                         scaledTile.width,
                                         scaledTile.height));
        QCOMPARE(scaledTile.width, static_cast<std::int32_t>(tileFallback.frameWidth));
        QCOMPARE(scaledTile.height, static_cast<std::int32_t>(tileFallback.frameHeight));
        QVERIFY(sizeof(IPC::FrameBufferHeader) + tileFallback.frameDataBytes <= Limit::MaxSharedFrameBytes);
        const auto tileXError = static_cast<std::int64_t>(tiled.tile->x) * tileFallback.request.width
            - static_cast<std::int64_t>(scaledTile.x) * tiled.width;
        const auto tileYError = static_cast<std::int64_t>(tiled.tile->y) * tileFallback.request.height
            - static_cast<std::int64_t>(scaledTile.y) * tiled.height;
        QVERIFY(tileXError >= -tiled.width && tileXError <= tiled.width);
        QVERIFY(tileYError >= -tiled.height && tileYError <= tiled.height);

        const Model::RenderRequest extremeTile {
            4,
            Limit::MaxTiledRenderDimension,
            Limit::MaxTiledRenderDimension,
            Model::RenderTile { 0, 0, Limit::MaxTiledRenderDimension, Limit::MaxTiledRenderDimension }
        };
        const auto extremeFallback = Worker::Runtime::fitRenderRequestToFrameBudget(extremeTile);
        QVERIFY(extremeFallback.frameWidth < static_cast<std::uint32_t>(extremeTile.tile->width));
        QVERIFY(sizeof(IPC::FrameBufferHeader) + extremeFallback.frameDataBytes <= Limit::MaxSharedFrameBytes);
    }

    void sharedEditableAnnotationWhitelistIsStable()
    {
        using ::Mu::Model::AnnotationType;
        using ::Mu::Model::isEditableAnnotationType;

        struct Case {
            AnnotationType type;
            bool editable;
        };

        static constexpr Case cases[] = {
            { AnnotationType::Text, true },      { AnnotationType::FreeText, true },
            { AnnotationType::Line, true },      { AnnotationType::Square, true },
            { AnnotationType::Circle, true },    { AnnotationType::Polygon, true },
            { AnnotationType::PolyLine, true },  { AnnotationType::Highlight, true },
            { AnnotationType::Underline, true }, { AnnotationType::Squiggly, true },
            { AnnotationType::StrikeOut, true }, { AnnotationType::Stamp, true },
            { AnnotationType::Caret, true },     { AnnotationType::Ink, true },
            { AnnotationType::Link, false },     { AnnotationType::Redact, false },
            { AnnotationType::Popup, false },    { AnnotationType::FileAttachment, false },
            { AnnotationType::Widget, false },   { AnnotationType::Unknown, false },
        };
        for (const Case& value : cases)
            QCOMPARE(isEditableAnnotationType(value.type), value.editable);
    }

    void sharedNormalizedUserSpaceConversionsAreInverses()
    {
        using ::Mu::Model::normalizedToUserSpace;
        using ::Mu::Model::Point;
        using ::Mu::Model::userSpaceToNormalized;
        const auto near = [](double a, double b) {
            return std::abs(a - b) < 1e-9;
        };

        // Normalized (0.1, 0.2) on a 200x100 page flips Y to 80.
        const Point user = normalizedToUserSpace({ 0.1, 0.2 }, 200, 100);
        QVERIFY(near(user.x, 20.0));
        QVERIFY(near(user.y, 80.0));

        const Point round = userSpaceToNormalized(user, 200, 100);
        QVERIFY(near(round.x, 0.1));
        QVERIFY(near(round.y, 0.2));

        // Out-of-range user-space points clamp into the normalized frame.
        const Point clamped = userSpaceToNormalized({ -50, 200 }, 200, 100);
        QVERIFY(near(clamped.x, 0.0));
        QVERIFY(near(clamped.y, 0.0));

        // A degenerate page extent yields zero rather than an infinity.
        const Point degenerate = userSpaceToNormalized({ 10, 10 }, 0, 0);
        QVERIFY(near(degenerate.x, 0.0));
        QVERIFY(near(degenerate.y, 0.0));
    }
};

int runTestWorkerNativeTypes(int argc, char** argv)
{
    TestNativeWorkerTypes test;
    return QTest::qExec(&test, argc, argv);
}

#include "native_types.moc"
