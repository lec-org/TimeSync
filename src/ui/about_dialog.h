#pragma once

#include <QDialog>

class QLabel;
class QPushButton;
class QShowEvent;

namespace TimeSync::Ui {

class AboutDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AboutDialog(QWidget *parent = nullptr);

    void retranslateUi();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void buildUi();
    void applyLogo();

    QLabel *logoLabel_ = nullptr;
    QLabel *studioLabel_ = nullptr;
    QLabel *websiteCaptionLabel_ = nullptr;
    QLabel *websiteLinkLabel_ = nullptr;
    QLabel *githubCaptionLabel_ = nullptr;
    QLabel *githubLinkLabel_ = nullptr;
    QPushButton *closeButton_ = nullptr;
};

} // namespace TimeSync::Ui
