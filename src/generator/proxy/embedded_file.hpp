// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PROXY_EMBEDDED_FILE_HPP
#define MU_GENERATOR_PROXY_EMBEDDED_FILE_HPP

#include <okular/core/document.h>

#include <QByteArray>
#include <QDateTime>
#include <QString>

namespace Mu::Generator::Proxy {

class EmbeddedFile final : public Okular::EmbeddedFile {
public:
    EmbeddedFile(const QString& name,
                 const QString& description,
                 int size,
                 const QDateTime& creationDate,
                 const QDateTime& modDate,
                 const QByteArray& data)
        : m_name(name)
        , m_description(description)
        , m_size(size)
        , m_creationDate(creationDate)
        , m_modDate(modDate)
        , m_data(data)
    {
    }

    QString name() const override { return m_name; }

    QString description() const override { return m_description; }

    QByteArray data() const override { return m_data; }

    int size() const override { return m_size <= 0 ? -1 : m_size; }

    QDateTime modificationDate() const override { return m_modDate; }

    QDateTime creationDate() const override { return m_creationDate; }

private:
    QString m_name;
    QString m_description;
    int m_size;
    QDateTime m_creationDate;
    QDateTime m_modDate;
    QByteArray m_data;
};

} // namespace Mu::Generator::Proxy

#endif // MU_GENERATOR_PROXY_EMBEDDED_FILE_HPP
