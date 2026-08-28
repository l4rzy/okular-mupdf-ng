#ifndef MUPDFSETTINGSWIDGET_HPP
#define MUPDFSETTINGSWIDGET_HPP

#include <QWidget>

#include "shared/model/types.hpp"

class Ui_MuPDFSettingsWidgetBase;

namespace Mu::Generator {

class MuPDFSettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit MuPDFSettingsWidget(QWidget* parent = nullptr, const Model::SandboxStatus& sandboxStatus = { });
    ~MuPDFSettingsWidget() override;

    void setSandboxStatus(const Model::SandboxStatus& status);
    void updateCustomCssButtonText();

private:
    void updateManageCertificatesButton();

    Ui_MuPDFSettingsWidgetBase* m_mupdfsw;
};

} // namespace Mu::Generator

#endif
