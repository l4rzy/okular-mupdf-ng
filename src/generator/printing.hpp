// SPDX-FileCopyrightText: 2026 l4rzy <me@23ro.org>
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef MU_GENERATOR_PRINTING_HPP
#define MU_GENERATOR_PRINTING_HPP

#include "generator/okular_compat.hpp"

#if MU_OKULAR_PRINT_OPTIONS_INTERFACE
#include <okular/interfaces/printinterface.h>
#else
#include <okular/core/printoptionswidget.h>
#endif

#include <QWidget>

class QComboBox;

namespace Mu::Generator {

// Mirrors the poppler generator's print scaling options; persisted as the
// PrintScaleMode kcfg entry.
enum class PrintScaleMode {
    FitToPrintableArea,
    FitToPage,
    None,
};

// Print dialog extra options: the scale mode selection. Persistence is owned
// by the caller (wired to scaleModeChanged); the widget itself stays
// side-effect free so tests can drive it without config files.
class PrintOptionsPage
#if MU_OKULAR_PRINT_OPTIONS_INTERFACE
    : public QWidget,
      public Okular::PrintOptionsWidgetInterface {
#else
    : public Okular::PrintOptionsWidget {
#endif
    Q_OBJECT

public:
    explicit PrintOptionsPage(PrintScaleMode initialMode);

#if MU_OKULAR_PRINT_OPTIONS_INTERFACE
    QWidget* widget() override { return this; }
#endif
    bool ignorePrintMargins() const override;

    [[nodiscard]] PrintScaleMode scaleMode() const;

public Q_SLOTS:
    void setScaleMode(PrintScaleMode mode);

Q_SIGNALS:
    void scaleModeChanged(Mu::Generator::PrintScaleMode mode);

private:
    QComboBox* m_scaleMode;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_PRINTING_HPP
