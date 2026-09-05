// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <memory>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "plugin/util/temp_dir.hpp"
#include "shared/compat.hpp"
#include "shared/model/validation.hpp"
#include "shared/protocol/ipc_debug.hpp"
#include "shared/protocol/limits.hpp"
#include "shared/protocol/zpp_codec.hpp"
#include "shared/transport/ctrl_channel.hpp"
#include "shared/transport/fd_channel.hpp"
#include "shared/transport/frame_buffer.hpp"

class TestTransport : public QObject {
    Q_OBJECT

private slots:

    void frameLayoutAndOverflowBoundaries()
    {
        QCOMPARE(sizeof(::Mu::IPC::FrameBufferHeader), size_t(64));
        QCOMPARE(uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + 4, uint64_t(68));
        QVERIFY(uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + uint64_t(4096) * (4096 * 4) > 64);
        QCOMPARE(uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + uint64_t(UINT32_MAX) * UINT32_MAX,
                 uint64_t(64) + uint64_t(UINT32_MAX) * UINT32_MAX);
        ::Mu::IPC::FrameBufferHeader header {
            ::Mu::IPC::FRAME_SHM_MAGIC, ::Mu::IPC::FRAME_SHM_VERSION, 7, 2, 3, 8, 1, { }
        };
        QCOMPARE(header.magic, ::Mu::IPC::FRAME_SHM_MAGIC);
        QCOMPARE(static_cast<const char*>(::Mu::IPC::framePixelData(&header)) - reinterpret_cast<const char*>(&header),
                 ptrdiff_t(64));
        QVERIFY(::Mu::IPC::validateFrameHeader(
            &header, 2, 3, 8, 1, 7, uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + uint64_t(3) * 8));
        QVERIFY(!::Mu::IPC::validateFrameHeader(
            &header, 2, 3, 8, 1, 8, uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + uint64_t(3) * 8));
        QVERIFY(!::Mu::IPC::validateFrameHeader(
            &header, 3, 3, 8, 1, 7, uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + uint64_t(3) * 8));
        QVERIFY(!::Mu::IPC::validateFrameHeader(
            &header, 2, 3, 3, 1, 7, uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) + uint64_t(3) * 3));
        const std::uint64_t maxDataBytes = ::Mu::Limit::MaxSharedFrameBytes - sizeof(::Mu::IPC::FrameBufferHeader);
        QCOMPARE(maxDataBytes + sizeof(::Mu::IPC::FrameBufferHeader), std::uint64_t(::Mu::Limit::MaxSharedFrameBytes));
        const std::uint32_t maxWidth = static_cast<std::uint32_t>(maxDataBytes / 4U);
        ::Mu::IPC::FrameBufferHeader maximumHeader {
            ::Mu::IPC::FRAME_SHM_MAGIC, ::Mu::IPC::FRAME_SHM_VERSION, 9, maxWidth, 1, maxWidth * 4U, 1, { }
        };
        QVERIFY(::Mu::IPC::validateFrameHeader(
            &maximumHeader, maxWidth, 1, maxWidth * 4U, 1, 9, ::Mu::Limit::MaxSharedFrameBytes));
        QVERIFY(!::Mu::IPC::validateFrameHeader(
            &maximumHeader, maxWidth, 1, maxWidth * 4U, 1, 9, std::uint64_t(::Mu::Limit::MaxSharedFrameBytes) + 1));
        // A mapping smaller than the header itself must be rejected without
        // reading the header fields beyond the mapped region.
        QVERIFY(!::Mu::IPC::validateFrameHeader(
            &header, 2, 3, 8, 1, 7, uint64_t(sizeof(::Mu::IPC::FrameBufferHeader)) - 1));
    }

    void tempDirectoryIsPerUser()
    {
        const QString path = ::Mu::Plugin::Util::tempDirectory();
        QVERIFY2(!path.isEmpty(), qPrintable(path));
        QCOMPARE(path, QDir::tempPath() + QStringLiteral("/okular-mupdf-ng-%1").arg(::getuid()));
        const QFileInfo info(path);
        QVERIFY(info.isDir());
        QVERIFY(info.permissions() & (QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QCOMPARE(info.permissions()
                     & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther
                        | QFile::ExeOther),
                 QFile::Permissions { });
    }

    void framedZppRoundTripAndMalformedLengths()
    {
        QTemporaryDir dir(::Mu::Plugin::Util::tempDirectory() + QStringLiteral("/test-XXXXXX"));
        QVERIFY(dir.isValid());
        const QString name = dir.filePath(QStringLiteral("control.sock"));
        ::Mu::IPC::CtrlChannel server;
        std::string error;
        if (!server.listen(QFile::encodeName(name).toStdString(), &error))
            QSKIP(qPrintable(QStringLiteral("Local socket unavailable: ") + QString::fromStdString(error)));
        ::Mu::Model::RequestMessage request;
        bool requestDecoded = false;
        std::jthread acceptThread([&] {
            auto peer = server.accept(::getpid(), 1000, &error);
            if (!peer.valid())
                return;
            std::vector<std::byte> frame;
            if (!::Mu::IPC::readFrame(peer, &frame, 1000, &error))
                return;
            requestDecoded = ::Mu::IPC::ZppCodec::decode(frame, &request, &error);
            const ::Mu::Model::ResponseMessage response {
                9, ::Mu::Model::PingResponse { std::string(::Mu::IPC::COMPAT), ::getpid(), { } }, std::nullopt
            };
            const auto encoded = ::Mu::IPC::ZppCodec::encode(response, &error);
            if (encoded)
                ::Mu::IPC::writeFrame(peer, *encoded, 1000, &error);
        });
        ::Mu::IPC::CtrlChannel client;
        QVERIFY(client.connect(QFile::encodeName(name).toStdString(), ::getpid(), &error));
        const ::Mu::Model::RequestMessage requestMessage {
            9, ::Mu::Model::PingRequest { std::string(::Mu::IPC::COMPAT) }
        };
        const auto encoded = ::Mu::IPC::ZppCodec::encode(requestMessage, &error);
        QVERIFY(encoded);
        QVERIFY(::Mu::IPC::writeFrame(client, *encoded, 1000, &error));
        std::vector<std::byte> response;
        QVERIFY(::Mu::IPC::readFrame(client, &response, 1000, &error));
        acceptThread.join();
        QVERIFY(requestDecoded);
        QCOMPARE(request.id, std::uint64_t(9));
        QVERIFY(std::holds_alternative<::Mu::Model::PingRequest>(request.payload));
        ::Mu::Model::ResponseMessage decodedResponse;
        QVERIFY(::Mu::IPC::ZppCodec::decode(response, &decodedResponse, &error));
        QCOMPARE(decodedResponse.id, std::uint64_t(9));
        QVERIFY(std::holds_alternative<::Mu::Model::PingResponse>(decodedResponse.payload));

        // The native writer rejects an oversized control message before narrowing its
        // length to the 32-bit control-plane header.
        const std::vector<std::byte> oversized(::Mu::Limit::MaxControlMessageBytes + 1);
        QVERIFY(!::Mu::IPC::writeFrame(client, oversized, 1000, &error));
    }

    void movingListenerPreservesSocketPathOwnership()
    {
        QTemporaryDir dir(::Mu::Plugin::Util::tempDirectory() + QStringLiteral("/test-XXXXXX"));
        QVERIFY(dir.isValid());
        const std::string path = QFile::encodeName(dir.filePath(QStringLiteral("control.sock"))).toStdString();
        ::Mu::IPC::CtrlChannel listener;
        std::string error;
        {
            ::Mu::IPC::CtrlChannel original;
            if (!original.listen(path, &error))
                QSKIP(qPrintable(QStringLiteral("Local socket unavailable: ") + QString::fromStdString(error)));
            listener = std::move(original);
            QVERIFY(listener.valid());
            QVERIFY(!original.valid());
        }

        // Verify accept timeout on idle listener without connecting peer.
        // The budget only needs to prove non-blocking behavior, not
        // scheduling precision, so it stays well above CI jitter.
        ::Mu::IPC::IoResult timeoutResult = ::Mu::IPC::IoResult::Complete;
        auto timeoutPeer = listener.accept(::getpid(), 200, &error, &timeoutResult);
        QVERIFY(!timeoutPeer.valid());
        QCOMPARE(timeoutResult, ::Mu::IPC::IoResult::Timeout);

        // Verify successful connection and IoResult::Complete status
        ::Mu::IPC::CtrlChannel client;
        QVERIFY(client.connect(path, ::getpid(), &error));
        ::Mu::IPC::IoResult connectResult = ::Mu::IPC::IoResult::Timeout;
        auto peer = listener.accept(::getpid(), 1000, &error, &connectResult);
        QVERIFY(peer.valid());
        QCOMPARE(connectResult, ::Mu::IPC::IoResult::Complete);
    }

    void nativeControlTransportRejectsMalformedAndTimedOutFrames()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);
        std::string error;
        std::vector<std::byte> frame;

        // A zero-length frame is invalid even though its fixed-size header is
        // complete. This is distinct from a peer that sends no bytes at all.
        const std::array<std::byte, 4> zeroHeader { };
        QCOMPARE(::write(writer.fd(), zeroHeader.data(), zeroHeader.size()), ssize_t(zeroHeader.size()));
        QVERIFY(!::Mu::IPC::readFrame(reader, &frame, 100, &error));
        QVERIFY(error.find("invalid length") != std::string::npos);

        error.clear();
        QVERIFY(!::Mu::IPC::readFrame(reader, &frame, 10, &error));
        QVERIFY(error.find("not connected") != std::string::npos);
    }

    void controlFrameSupportsInfiniteWaitAndRejectsNullOutput()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);
        std::string error;
        const std::array<std::byte, 5> encoded {
            std::byte(0), std::byte(0), std::byte(0), std::byte(1), std::byte(0x7f)
        };
        std::atomic<ssize_t> written { -1 };
        std::jthread delayedWriter([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            written.store(::write(writer.fd(), encoded.data(), encoded.size()));
        });
        std::vector<std::byte> frame;
        QVERIFY(::Mu::IPC::readFrame(reader, &frame, -1, &error));
        delayedWriter.join();
        QCOMPARE(written.load(), ssize_t(encoded.size()));
        QCOMPARE(frame, std::vector<std::byte> { std::byte(0x7f) });

        QVERIFY(!::Mu::IPC::readFrame(reader, nullptr, 0, &error));
        QVERIFY(error.find("output is null") != std::string::npos);
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, nullptr, &error), ::Mu::IPC::ReadStatus::Error);
        QVERIFY(error.find("output is null") != std::string::npos);
    }

    void partialFrameFailurePoisonsTheChannel()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);
        const std::array<std::byte, 2> partialHeader { std::byte(0), std::byte(0) };
        QCOMPARE(::write(writer.fd(), partialHeader.data(), partialHeader.size()), ssize_t(partialHeader.size()));

        std::vector<std::byte> frame;
        std::string error;
        QVERIFY(!::Mu::IPC::readFrame(reader, &frame, 10, &error));
        QVERIFY(error.find("timed out") != std::string::npos);
        QVERIFY(!reader.valid());
        QVERIFY(!::Mu::IPC::readFrame(reader, &frame, 0, &error));
        QVERIFY(error.find("not connected") != std::string::npos);
    }

    void nonBlockingReaderDetectsPeerCloseDuringFrame()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);
        const std::array<std::byte, 2> partialHeader { std::byte(0), std::byte(0) };
        QCOMPARE(::write(writer.fd(), partialHeader.data(), partialHeader.size()), ssize_t(partialHeader.size()));
        writer.close();

        std::vector<std::byte> frame;
        std::string error;
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, &error), ::Mu::IPC::ReadStatus::Closed);
        QVERIFY(error.find("closed during a frame") != std::string::npos);
        QVERIFY(!reader.valid());
    }

    void nonBlockingReaderDrainsFrameBeforePeerClose()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);

        const std::array<std::byte, 5> encoded {
            std::byte(0), std::byte(0), std::byte(0), std::byte(1), std::byte(0x7f)
        };
        QCOMPARE(::write(writer.fd(), encoded.data(), encoded.size()), ssize_t(encoded.size()));
        writer.close();

        std::vector<std::byte> frame;
        std::string error;
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, &error), ::Mu::IPC::ReadStatus::Complete);
        QCOMPARE(frame, std::vector<std::byte> { std::byte(0x7f) });
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, &error), ::Mu::IPC::ReadStatus::Closed);
    }

    void payloadTimeoutUsesTheWholeFrameDeadline()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);
        const std::array<std::byte, 5> partialFrame {
            std::byte(0), std::byte(0), std::byte(0), std::byte(4), std::byte('x')
        };
        QCOMPARE(::write(writer.fd(), partialFrame.data(), partialFrame.size()), ssize_t(partialFrame.size()));

        std::vector<std::byte> frame;
        std::string error;
        QVERIFY(!::Mu::IPC::readFrame(reader, &frame, 10, &error));
        QVERIFY(error.find("timed out") != std::string::npos);
        QVERIFY(!reader.valid());
    }

    void nonBlockingFrameReaderReportsStreamState()
    {
        int fds[2] { -1, -1 };
        QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        ::Mu::IPC::CtrlChannel writer(fds[0]);
        ::Mu::IPC::CtrlChannel reader(fds[1]);
        std::vector<std::byte> frame;

        // No bytes yet.
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, nullptr), ::Mu::IPC::ReadStatus::NoData);

        // A header announcing a payload that has not yet arrived must not have
        // its bytes consumed, so the stream stays synchronised.
        constexpr std::size_t PayloadSize = 16;
        const std::array<std::byte, 4> header {
            std::byte((PayloadSize >> 24) & 0xff),
            std::byte((PayloadSize >> 16) & 0xff),
            std::byte((PayloadSize >> 8) & 0xff),
            std::byte(PayloadSize & 0xff),
        };
        QCOMPARE(::write(writer.fd(), header.data(), header.size()), ssize_t(header.size()));
        std::array<std::byte, 8> partialPayload { };
        partialPayload.fill(std::byte(0xab));
        QCOMPARE(::write(writer.fd(), partialPayload.data(), partialPayload.size()), ssize_t(partialPayload.size()));
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, nullptr), ::Mu::IPC::ReadStatus::Partial);
        QVERIFY(frame.empty());

        // Once the remainder arrives the full frame becomes available.
        std::array<std::byte, 8> rest { };
        rest.fill(std::byte(0xcd));
        QCOMPARE(::write(writer.fd(), rest.data(), rest.size()), ssize_t(rest.size()));
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, nullptr), ::Mu::IPC::ReadStatus::Complete);
        QCOMPARE(frame.size(), PayloadSize);
        QCOMPARE(frame[0], std::byte(0xab));
        QCOMPARE(frame[PayloadSize - 1], std::byte(0xcd));

        // A malformed (zero-length) header is rejected without blocking.
        const std::array<std::byte, 4> zeroHeader { };
        QCOMPARE(::write(writer.fd(), zeroHeader.data(), zeroHeader.size()), ssize_t(zeroHeader.size()));
        std::string error;
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, &error), ::Mu::IPC::ReadStatus::Error);
        QVERIFY(error.find("invalid length") != std::string::npos);
        // A malformed frame poisons the channel, so it cannot be reused or
        // mistaken for a later frame.
        error.clear();
        QVERIFY(!::Mu::IPC::readFrame(reader, &frame, 100, &error));

        // The malformed frame already closed the channel.
        writer.close();
        QCOMPARE(::Mu::IPC::tryReadFrame(reader, &frame, nullptr), ::Mu::IPC::ReadStatus::Error);
    }

    void fdChannelTransfersDescriptorAndRejectsWrongId()
    {
        QTemporaryDir dir(::Mu::Plugin::Util::tempDirectory() + QStringLiteral("/test-XXXXXX"));
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("fd.sock"));
        ::Mu::IPC::FdChannel listener;
        std::string error;
        if (!listener.listen(QFile::encodeName(path).toStdString(), &error))
            QSKIP(qPrintable(QStringLiteral("FD socket unavailable: ") + QString::fromStdString(error)));

        std::atomic_bool accepted { false };
        std::jthread acceptThread([&] { accepted = listener.accept(&error); });
        ::Mu::IPC::FdChannel peer;
        QVERIFY(peer.connect(QFile::encodeName(path).toStdString(), &error));
        acceptThread.join();
        QVERIFY(accepted);

        // A control request without its descriptor must not indefinitely block
        // the owning worker event loop.
        QCOMPARE(listener.receive(999, &error, 20), -1);
        QVERIFY(QString::fromStdString(error).contains(QStringLiteral("timed out")));

        const QString filePath = dir.filePath(QStringLiteral("payload"));
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("payload");
        file.close();
        const int sourceFd = ::open(QFile::encodeName(filePath).constData(), O_RDONLY | O_CLOEXEC);
        QVERIFY(sourceFd >= 0);

        std::jthread delayedTransfer([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            peer.send(43, sourceFd, &error);
        });
        const int delayed = listener.receive(43, &error, -1);
        delayedTransfer.join();
        QVERIFY(delayed >= 0);
        ::close(delayed);

        QVERIFY(peer.send(42, sourceFd, &error));
        const int received = listener.receive(41, &error);
        QCOMPARE(received, -1);
        QVERIFY(!error.empty());
        QVERIFY(peer.send(42, sourceFd, &error));
        // SCM_RIGHTS keeps its own reference once sendmsg succeeds. Closing the
        // sender descriptor before receive must not invalidate the transfer.
        ::close(sourceFd);
        const int valid = listener.receive(42, &error);
        QVERIFY(valid >= 0);

        // A mapping also remains valid after its received descriptor closes,
        // matching the render frame lifecycle used by QImage.
        constexpr std::size_t payloadSize = 7;
        void* mapping = ::mmap(nullptr, payloadSize, PROT_READ, MAP_PRIVATE, valid, 0);
        QVERIFY(mapping != MAP_FAILED);
        ::close(valid);
        QCOMPARE(static_cast<const char*>(mapping)[0], 'p');
        QCOMPARE(::munmap(mapping, payloadSize), 0);
    }

    void formFieldRoundTripSerialization()
    {
        ::Mu::Model::FormField field;
        field.handle = "g1-f2-o42";
        field.page = 2;
        field.pdfObjectNumber = 42;
        field.fieldObjectNumber = 10;
        field.type = ::Mu::Model::FormFieldType::ComboBox;
        field.partialName = "Country";
        field.uiName = "Select Country";
        field.fullyQualifiedName = "User.Profile.Country";
        field.groupName = "User.Profile";
        field.rectangle = { 0.1, 0.2, 0.6, 0.4 };
        field.readOnly = false;
        field.visible = true;
        field.printable = true;
        field.text = "Canada";
        field.maximumLength = 100;
        field.multiline = false;
        field.password = false;
        field.checked = false;
        field.onState = "Yes";
        field.choices = { "United States", "Canada", "Mexico" };
        field.exportValues = { "US", "CA", "MX" };
        field.currentChoices = { 1 };
        field.editableCombo = true;
        field.multiSelect = false;

        QVERIFY(::Mu::Model::isValidFormField(field));

        ::Mu::Model::PageInfo pageInfo;
        pageInfo.number = 2;
        pageInfo.formFields.push_back(field);

        std::string error;
        const ::Mu::Model::ResponseMessage responseMsg { 101,
                                                         ::Mu::Model::OpenResponse { { pageInfo }, 0, { } },
                                                         std::nullopt };
        const auto encoded = ::Mu::IPC::ZppCodec::encode(responseMsg, &error);
        QVERIFY(encoded.has_value());

        ::Mu::Model::ResponseMessage decoded;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*encoded, &decoded, &error));
        const auto* openRes = std::get_if<::Mu::Model::OpenResponse>(&decoded.payload);
        QVERIFY(openRes != nullptr);
        QCOMPARE(openRes->pages.size(), std::size_t(1));
        QCOMPARE(openRes->pages[0].formFields.size(), std::size_t(1));

        const auto& decField = openRes->pages[0].formFields[0];
        QCOMPARE(decField.handle, field.handle);
        QCOMPARE(decField.page, field.page);
        QCOMPARE(decField.pdfObjectNumber, field.pdfObjectNumber);
        QCOMPARE(decField.fieldObjectNumber, field.fieldObjectNumber);
        QCOMPARE(decField.type, field.type);
        QCOMPARE(decField.partialName, field.partialName);
        QCOMPARE(decField.uiName, field.uiName);
        QCOMPARE(decField.fullyQualifiedName, field.fullyQualifiedName);
        QCOMPARE(decField.groupName, field.groupName);
        QCOMPARE(decField.rectangle.left, field.rectangle.left);
        QCOMPARE(decField.rectangle.right, field.rectangle.right);
        QCOMPARE(decField.readOnly, field.readOnly);
        QCOMPARE(decField.visible, field.visible);
        QCOMPARE(decField.printable, field.printable);
        QCOMPARE(decField.text, field.text);
        QCOMPARE(decField.maximumLength, field.maximumLength);
        QCOMPARE(decField.choices, field.choices);
        QCOMPARE(decField.exportValues, field.exportValues);
        QCOMPARE(decField.currentChoices, field.currentChoices);
        QCOMPARE(decField.editableCombo, field.editableCombo);
        QCOMPARE(decField.multiSelect, field.multiSelect);
    }

    void formFieldValidationRejectsMalformedInput()
    {
        // Malformed UTF-8 in the field name
        {
            ::Mu::Model::FormField field;
            field.handle = "g1-f0-o1";
            field.page = 0;
            field.rectangle = { 0.0, 0.0, 1.0, 1.0 };
            field.partialName = std::string("\xFF\xFE", 2); // Invalid UTF-8

            std::string_view reason;
            QVERIFY(!::Mu::Model::isValidFormField(field, &reason));
            QVERIFY(reason.find("UTF-8") != std::string_view::npos);
        }

        // Embedded NUL in the field text
        {
            ::Mu::Model::FormField field;
            field.handle = "g1-f0-o1";
            field.page = 0;
            field.rectangle = { 0.0, 0.0, 1.0, 1.0 };
            field.text = std::string("test\0injection", 14);

            std::string_view reason;
            QVERIFY(!::Mu::Model::isValidFormField(field, &reason));
            QVERIFY(reason.find("NUL") != std::string_view::npos);
        }

        // Inverted, non-finite, and out-of-range rectangles
        {
            ::Mu::Model::FormField field;
            field.handle = "g1-f0-o1";
            field.page = 0;
            field.rectangle = { 0.8, 0.1, 0.2, 0.9 }; // Inverted (left > right)
            QVERIFY(!::Mu::Model::isValidFormField(field));

            field.rectangle = { 0.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 1.0 };
            QVERIFY(!::Mu::Model::isValidFormField(field));

            field.rectangle = { -0.1, 0.0, 1.0, 1.0 }; // Out of normalized range
            QVERIFY(!::Mu::Model::isValidFormField(field));
        }

        // Choice indices: out of bounds, multi without flag, multi with flag
        {
            ::Mu::Model::FormField field;
            field.handle = "g1-f0-o1";
            field.page = 0;
            field.type = ::Mu::Model::FormFieldType::ListBox;
            field.rectangle = { 0.0, 0.0, 1.0, 1.0 };
            field.choices = { "OptionA", "OptionB" };
            field.currentChoices = { 2 }; // Out of bounds

            QVERIFY(!::Mu::Model::isValidFormField(field));

            // Multiple choices on non-multiSelect
            field.currentChoices = { 0, 1 };
            field.multiSelect = false;
            QVERIFY(!::Mu::Model::isValidFormField(field));

            // Allowed on multiSelect
            field.multiSelect = true;
            QVERIFY(::Mu::Model::isValidFormField(field));
        }
    }

    void formChoiceSelectionAndCustomTextRoundTrip()
    {
        std::string error;
        const ::Mu::Model::RequestMessage selectReq {
            10, ::Mu::Model::FormUpdateRequest { "h1", ::Mu::Model::FormChoiceSelection { { 0, 2 } } }
        };
        const auto encodedSelect = ::Mu::IPC::ZppCodec::encode(selectReq, &error);
        QVERIFY(encodedSelect.has_value());

        ::Mu::Model::RequestMessage decodedSelect;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*encodedSelect, &decodedSelect, &error));
        const auto* updateReq1 = std::get_if<::Mu::Model::FormUpdateRequest>(&decodedSelect.payload);
        QVERIFY(updateReq1 != nullptr);
        const auto* selVal = std::get_if<::Mu::Model::FormChoiceSelection>(&updateReq1->value);
        QVERIFY(selVal != nullptr);
        QCOMPARE(selVal->selectedIndices, std::vector<int>({ 0, 2 }));

        const ::Mu::Model::RequestMessage customReq {
            11, ::Mu::Model::FormUpdateRequest { "h2", ::Mu::Model::FormChoiceCustomText { "CustomString" } }
        };
        const auto encodedCustom = ::Mu::IPC::ZppCodec::encode(customReq, &error);
        QVERIFY(encodedCustom.has_value());

        ::Mu::Model::RequestMessage decodedCustom;
        QVERIFY(::Mu::IPC::ZppCodec::decode(*encodedCustom, &decodedCustom, &error));
        const auto* updateReq2 = std::get_if<::Mu::Model::FormUpdateRequest>(&decodedCustom.payload);
        QVERIFY(updateReq2 != nullptr);
        const auto* custVal = std::get_if<::Mu::Model::FormChoiceCustomText>(&updateReq2->value);
        QVERIFY(custVal != nullptr);
        QCOMPARE(custVal->text, std::string("CustomString"));
    }

#ifdef MU_DEBUG_ENABLED
    void signingPayloadDoesNotExposeSecrets()
    {
        const ::Mu::Model::SignInput payload { 1, "nonce", "cert", { 1, 2, 3 } };
        const QString formatted = QString::fromStdString(::Mu::IPC::Debug::notification({ payload }));
        QVERIFY(formatted.contains(QStringLiteral("cert")));
        QVERIFY(!formatted.contains(QStringLiteral("0x010203")));
    }

    void formPayloadDoesNotExposeSensitiveData()
    {
        ::Mu::Model::FormField field;
        field.handle = "h-secret";
        field.fullyQualifiedName = "User.Password";
        field.rectangle = { 0.1, 0.1, 0.9, 0.9 };
        field.text = "UltraSecretPassword999";

        ::Mu::Model::PageInfo pageInfo;
        pageInfo.number = 0;
        pageInfo.formFields.push_back(field);

        std::ostringstream pageOut;
        ::Mu::IPC::Debug::Detail::page(pageOut, pageInfo);
        const QString pageFormatted = QString::fromStdString(pageOut.str());
        QVERIFY(!pageFormatted.contains(QStringLiteral("UltraSecretPassword999")));
        QVERIFY(pageFormatted.contains(QStringLiteral("[redacted]")));

        const ::Mu::Model::RequestMessage updateReq {
            5, ::Mu::Model::FormUpdateRequest { "h-secret", ::Mu::Model::FormTextValue { "CreditCardNumber1234" } }
        };
        const QString updateFormatted = QString::fromStdString(::Mu::IPC::Debug::request(updateReq));
        QVERIFY(!updateFormatted.contains(QStringLiteral("CreditCardNumber1234")));
        QVERIFY(updateFormatted.contains(QStringLiteral("[redacted]")));
    }
#endif
};

QTEST_GUILESS_MAIN(TestTransport)

#include "transport.moc"
