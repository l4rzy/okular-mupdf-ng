// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/conversion/document.hpp"

#include "generator/proxy/embedded_file.hpp"

#include <QByteArray>
#include <QDateTime>
#include <QTimeZone>
#include <QUrl>

#include <array>
#include <functional>
#include <limits>

namespace Mu::Generator::Conversion {

Okular::FontInfo fromModel(const Model::Font& model)
{
    static const Okular::FontInfo::FontType fontTypes[] = {
        Okular::FontInfo::Unknown,     Okular::FontInfo::Type1,       Okular::FontInfo::Type1C,
        Okular::FontInfo::Type1COT,    Okular::FontInfo::Type3,       Okular::FontInfo::TrueType,
        Okular::FontInfo::TrueTypeOT,  Okular::FontInfo::CIDType0,    Okular::FontInfo::CIDType0C,
        Okular::FontInfo::CIDType0COT, Okular::FontInfo::CIDTrueType, Okular::FontInfo::CIDTrueTypeOT,
    };
    static const Okular::FontInfo::EmbedType embedTypes[] = {
        Okular::FontInfo::NotEmbedded,
        Okular::FontInfo::EmbeddedSubset,
        Okular::FontInfo::FullyEmbedded,
    };

    Okular::FontInfo result;
    result.setName(QString::fromStdString(model.name));
    result.setFile(QString::fromStdString(model.file));
    const auto typeIndex = static_cast<std::int32_t>(model.type);
    if (typeIndex >= 0 && typeIndex < static_cast<int>(std::size(fontTypes)))
        result.setType(fontTypes[typeIndex]);
    const auto embedIndex = static_cast<std::int32_t>(model.embedType);
    if (embedIndex >= 0 && embedIndex < static_cast<int>(std::size(embedTypes)))
        result.setEmbedType(embedTypes[embedIndex]);
    return result;
}

std::unique_ptr<Okular::EmbeddedFile> embeddedFile(const Model::EmbeddedFile& file)
{
    const auto toBytes = [](const std::vector<std::uint8_t>& bytes) {
        return QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    };
    const auto date = [](const Model::Timestamp& value) {
        return value.valid ? QDateTime::fromMSecsSinceEpoch(value.unixMilliseconds, QTimeZone::UTC) : QDateTime { };
    };
    const int size =
        file.size > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(file.size);
    return std::make_unique<Proxy::EmbeddedFile>(QString::fromStdString(file.name),
                                                 QString::fromStdString(file.description),
                                                 size,
                                                 date(file.creationDate),
                                                 date(file.modificationDate),
                                                 toBytes(file.data));
}

namespace {

Okular::DocumentViewport viewportFromModel(const Model::Viewport& modelViewport)
{
    Okular::DocumentViewport viewport(modelViewport.page);
    viewport.rePos.enabled = true;
    viewport.rePos.pos = Okular::DocumentViewport::TopLeft;
    viewport.rePos.normalizedX =
        (modelViewport.coordinateMask & Model::Viewport::CoordinateX) != 0 ? modelViewport.normalizedX : 0.0;
    viewport.rePos.normalizedY =
        (modelViewport.coordinateMask & Model::Viewport::CoordinateY) != 0 ? modelViewport.normalizedY : 0.0;
    return viewport;
}

Okular::ObjectRect* objectRect(const Model::Link& link)
{
    std::unique_ptr<Okular::Action> action;
    if (link.target.external) {
        action = std::make_unique<Okular::BrowseAction>(QUrl(QString::fromStdString(link.target.uri)));
    } else if (link.target.valid && link.target.viewport.page >= 0) {
        action = std::make_unique<Okular::GotoAction>(QString(), viewportFromModel(link.target.viewport));
    }
    if (!action)
        return nullptr;
    return new Okular::ObjectRect(
        link.left, link.top, link.right, link.bottom, false, Okular::ObjectRect::Action, action.release());
}

} // namespace

QList<Okular::ObjectRect*> objectRects(const std::vector<Model::Link>& links)
{
    QList<Okular::ObjectRect*> result;
    result.reserve(static_cast<qsizetype>(links.size()));
    for (const auto& link : links) {
        if (auto* rect = objectRect(link))
            result.append(rect);
    }
    return result;
}

std::unique_ptr<Okular::DocumentSynopsis> documentSynopsis(const std::vector<Model::OutlineNode>& nodes)
{
    if (nodes.empty())
        return nullptr;

    auto result = std::make_unique<Okular::DocumentSynopsis>();
    std::function<void(const std::vector<Model::OutlineNode>&, QDomNode&)> addNodes;
    addNodes = [&](const std::vector<Model::OutlineNode>& children, QDomNode& parent) {
        for (const auto& node : children) {
            const QString title = node.title.empty() ? QStringLiteral("Item") : QString::fromStdString(node.title);
            QDomElement element = result->createElement(title);
            if (element.isNull())
                continue;
            parent.appendChild(element);
            if (node.open)
                element.setAttribute(QStringLiteral("Open"), QStringLiteral("true"));
            if (node.link.valid) {
                if (node.link.external) {
                    element.setAttribute(QStringLiteral("URL"), QString::fromStdString(node.link.uri));
                } else {
                    element.setAttribute(QStringLiteral("Viewport"), viewportFromModel(node.link.viewport).toString());
                }
            }
            addNodes(node.children, element);
        }
    };

    addNodes(nodes, *result);
    return result;
}

} // namespace Mu::Generator::Conversion
