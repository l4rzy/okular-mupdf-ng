#ifndef MU_GENERATOR_CONFIG_SETTINGSWIDGET_HPP
#define MU_GENERATOR_CONFIG_SETTINGSWIDGET_HPP

#include <QWidget>

class Ui_MuPDFNGSettingsWidgetBase;

namespace Mu::Generator {

class MuPDFNGSettingsWidget : public QWidget {
    Q_OBJECT

public:
    explicit MuPDFNGSettingsWidget(QWidget* parent = nullptr);
    ~MuPDFNGSettingsWidget() override;

    void updateCustomCssButtonText();

private:
    void updateManageCertificatesButton();
    /// Refreshes the signature preview from the widgets' pending state so it
    /// tracks unapplied changes.
    void updateSignaturePreview();

    Ui_MuPDFNGSettingsWidgetBase* m_mupdfsw;
};

} // namespace Mu::Generator

#endif // MU_GENERATOR_CONFIG_SETTINGSWIDGET_HPP
