// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "runtime/frame_pool.hpp"
#include "shared/protocol/limits.hpp"

#include <QTest>
#include <array>
#include <fcntl.h>

using Mu::Worker::Runtime::FramePool;

class TestFramePool : public QObject {
    Q_OBJECT

private slots:

    void reuseSmallestIdleSlot_data()
    {
        QTest::addColumn<quint64>("bytes");
        QTest::addColumn<int>("expectedSlot");
        QTest::newRow("exact-small") << quint64(128) << 0;
        QTest::newRow("between-small-and-medium") << quint64(129) << 1;
        QTest::newRow("exact-medium") << quint64(256) << 1;
        QTest::newRow("between-medium-and-large") << quint64(257) << 2;
        QTest::newRow("exact-large") << quint64(512) << 2;
    }

    void reuseSmallestIdleSlot()
    {
        QFETCH(quint64, bytes);
        QFETCH(int, expectedSlot);
        FramePool pool;
        std::array<FramePool::Lease, 3> leases;
        const std::array<std::size_t, 3> sizes { 128, 256, 512 };
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            auto frame = pool.acquireFrame(sizes[i]);
            QVERIFY(frame);
            QVERIFY(frame->needsTransfer());
            *static_cast<unsigned char*>(frame->data()) = static_cast<unsigned char>(i);
            leases[i] = pool.publishFrame(*frame);
        }
        for (const auto& lease : leases)
            pool.releaseFrame(lease.slotId, lease.leaseId);

        auto reused = pool.acquireFrame(static_cast<std::size_t>(bytes));
        QVERIFY(reused);
        QVERIFY(!reused->needsTransfer());
        QCOMPARE(*static_cast<unsigned char*>(reused->data()), expectedSlot);
        const auto lease = pool.publishFrame(*reused);
        const auto previous = leases[static_cast<std::size_t>(expectedSlot)];
        QCOMPARE(lease.slotId, previous.slotId);
        QCOMPARE(lease.leaseId, previous.leaseId + 1);
    }

    void releaseHonorsLease_data()
    {
        QTest::addColumn<bool>("unknownSlot");
        QTest::addColumn<int>("leaseOffset");
        QTest::addColumn<bool>("reusable");
        QTest::newRow("current") << false << 0 << true;
        QTest::newRow("previous") << false << -1 << false;
        QTest::newRow("future") << false << 1 << false;
        QTest::newRow("unknown-slot") << true << 0 << false;
    }

    void releaseHonorsLease()
    {
        QFETCH(bool, unknownSlot);
        QFETCH(int, leaseOffset);
        QFETCH(bool, reusable);
        FramePool pool;
        FramePool::Lease first;
        {
            auto frame = pool.acquireFrame(128);
            QVERIFY(frame);
            first = pool.publishFrame(*frame);
        }
        pool.releaseFrame(first.slotId, first.leaseId);
        FramePool::Lease current;
        {
            auto frame = pool.acquireFrame(128);
            QVERIFY(frame);
            current = pool.publishFrame(*frame);
        }
        const auto releaseId = static_cast<std::uint64_t>(static_cast<std::int64_t>(current.leaseId) + leaseOffset);
        pool.releaseFrame(current.slotId + (unknownSlot ? 1 : 0), releaseId);
        auto next = pool.acquireFrame(128);
        QVERIFY(next);
        QCOMPARE(next->needsTransfer(), !reusable);
    }

    void fullPoolUsesTransientFrame_data()
    {
        QTest::addColumn<quint64>("bytes");
        QTest::addColumn<int>("count");
        QTest::newRow("slot-limit") << quint64(128) << static_cast<int>(Mu::Limit::MaxFrameSlotCount);
        QTest::newRow("combined-byte-limit") << quint64(Mu::Limit::MaxSharedFrameBytes / 2) << 2;
    }

    void fullPoolUsesTransientFrame()
    {
        QFETCH(quint64, bytes);
        QFETCH(int, count);
        FramePool pool;
        FramePool::Lease first;
        for (int i = 0; i < count; ++i) {
            auto frame = pool.acquireFrame(static_cast<std::size_t>(bytes));
            QVERIFY(frame);
            const auto lease = pool.publishFrame(*frame);
            QVERIFY(lease.slotId);
            if (i == 0)
                first = lease;
        }
        {
            auto transient = pool.acquireFrame(128);
            QVERIFY(transient);
            QVERIFY(transient->needsTransfer());
            const auto lease = pool.publishFrame(*transient);
            QCOMPARE(lease.slotId, std::uint64_t(0));
            QCOMPARE(lease.leaseId, std::uint64_t(0));
        }
        pool.releaseFrame(first.slotId, first.leaseId);
        auto reused = pool.acquireFrame(128);
        QVERIFY(reused);
        QVERIFY(!reused->needsTransfer());
        QCOMPARE(pool.publishFrame(*reused).slotId, first.slotId);
    }

    void discardedAllocationReleasesResources_data()
    {
        QTest::addColumn<quint64>("bytes");
        QTest::newRow("small") << quint64(128);
        QTest::newRow("whole-budget") << quint64(Mu::Limit::MaxSharedFrameBytes);
    }

    void discardedAllocationReleasesResources()
    {
        QFETCH(quint64, bytes);
        FramePool pool;
        int descriptor = -1;
        {
            auto discarded = pool.acquireFrame(static_cast<std::size_t>(bytes));
            QVERIFY(discarded);
            descriptor = discarded->descriptor();
            QVERIFY(::fcntl(descriptor, F_GETFD) >= 0);
        }
        QCOMPARE(::fcntl(descriptor, F_GETFD), -1);
        auto replacement = pool.acquireFrame(Mu::Limit::MaxSharedFrameBytes);
        QVERIFY(replacement);
        QVERIFY(pool.publishFrame(*replacement).slotId);
    }

    void clearRejectsOldDocumentLeases()
    {
        FramePool pool;
        FramePool::Lease old;
        {
            auto frame = pool.acquireFrame(Mu::Limit::MaxSharedFrameBytes);
            QVERIFY(frame);
            old = pool.publishFrame(*frame);
        }
        pool.clearFrames();
        FramePool::Lease current;
        {
            auto frame = pool.acquireFrame(Mu::Limit::MaxSharedFrameBytes);
            QVERIFY(frame);
            QVERIFY(frame->needsTransfer());
            current = pool.publishFrame(*frame);
            QVERIFY(current.slotId > old.slotId);
        }
        pool.releaseFrame(old.slotId, old.leaseId);
        auto next = pool.acquireFrame(128);
        QVERIFY(next);
        QCOMPARE(pool.publishFrame(*next).slotId, std::uint64_t(0));
    }
};

QTEST_GUILESS_MAIN(TestFramePool)
#include "frame_pool.moc"
