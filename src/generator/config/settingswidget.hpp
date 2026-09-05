#ifndef MU_GENERATOR_CONFIG_SETTINGSWIDGET_HPP
#define MU_GENERATOR_CONFIG_SETTINGSWIDGET_HPP

#include <QWidget>

class Ui_MuPDFSettingsWidgetBase;

namespace Mu::Generator {

class MuPDFSettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit MuPDFSettingsWidget(QWidget* parent = nullptr);
    ~MuPDFSettingsWidget() override;

    void updateCustomCssButtonText();

private:
    void updateManageCertificatesButton();

    Ui_MuPDFSettingsWidgetBase* m_mupdfsw;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_CONFIG_SETTINGSWIDGET_HPP
