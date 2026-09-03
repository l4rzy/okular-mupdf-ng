// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "generator/printing.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QVBoxLayout>

#include <KLocalizedString>

namespace Mu::Generator {

PrintOptionsPage::PrintOptionsPage(PrintScaleMode initialMode)
{
    setWindowTitle(i18n("MuPDF-NG Options"));
    auto* layout = new QVBoxLayout(this);

    auto* formWidget = new QWidget(this);
    auto* printBackendLayout = new QFormLayout(formWidget);

    m_scaleMode = new QComboBox(this);
    m_scaleMode->insertItem(static_cast<int>(PrintScaleMode::FitToPrintableArea),
                            i18n("Fit to printable area"),
                            static_cast<int>(PrintScaleMode::FitToPrintableArea));
    m_scaleMode->insertItem(static_cast<int>(PrintScaleMode::FitToPage),
                            i18n("Fit to full page"),
                            static_cast<int>(PrintScaleMode::FitToPage));
    m_scaleMode->insertItem(static_cast<int>(PrintScaleMode::None),
                            i18n("None; print original size"),
                            static_cast<int>(PrintScaleMode::None));
    m_scaleMode->setToolTip(i18n("Scaling mode for the printed pages"));
    printBackendLayout->addRow(i18n("Scale mode:"), m_scaleMode);

    layout->addWidget(formWidget);
    layout->addStretch(1);

    setScaleMode(initialMode);
    connect(m_scaleMode, &QComboBox::currentIndexChanged, this, [this] { Q_EMIT scaleModeChanged(scaleMode()); });
}

bool PrintOptionsPage::ignorePrintMargins() const
{
    // Full-page printing must not clip against the printer's margins.
    return scaleMode() == PrintScaleMode::FitToPage;
}

PrintScaleMode PrintOptionsPage::scaleMode() const
{
    return m_scaleMode->currentData().value<PrintScaleMode>();
}

void PrintOptionsPage::setScaleMode(PrintScaleMode mode)
{
    const int index = m_scaleMode->findData(static_cast<int>(mode));
    if (index < 0)
        return;
    m_scaleMode->setCurrentIndex(index);
}

} // namespace Mu::Generator

#include "printing.moc"
