// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "plugin/caching/cache_file.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <limits>

namespace Mu::Plugin::Caching {

namespace {

std::optional<QString> s_root;

QString root()
{
    return s_root ? *s_root : QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

} // namespace

void setRootForTesting(const QString& rootPath)
{
    s_root = rootPath;
}

void clearRootForTesting()
{
    s_root.reset();
}

QString directory(const QString& name)
{
    return root() + QLatin1Char('/') + name;
}

std::optional<QByteArray> readBounded(const QString& path, qint64 maxBytes)
{
    if (maxBytes < 0)
        return std::nullopt;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;

    const qint64 initialSize = file.size();
    if (initialSize < 0 || initialSize > maxBytes)
        return std::nullopt;

    const qint64 readLimit = maxBytes == std::numeric_limits<qint64>::max() ? maxBytes : maxBytes + 1;
    const QByteArray data = file.read(readLimit);
    return data.size() == initialSize && data.size() <= maxBytes ? std::optional<QByteArray>(data) : std::nullopt;
}

bool writeAtomically(const QString& path, const QByteArray& data)
{
    if (!QFileInfo(path).dir().mkpath(QStringLiteral(".")))
        return false;

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(data) != data.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace Mu::Plugin::Caching
