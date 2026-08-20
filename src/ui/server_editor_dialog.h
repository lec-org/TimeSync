#pragma once

#include <QDialog>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QToolButton;

namespace TimeSync::Ui {

class ServerEditorDialog final : public QDialog {
    Q_OBJECT

public:
    explicit ServerEditorDialog(const QStringList &servers,
                                const QStringList &defaultServers,
                                QWidget *parent = nullptr);

    QStringList servers() const;
    void retranslateUi();
    void setSaving(bool saving);
    void showSaveError(const QString &message);

signals:
    void saveRequested(const QStringList &servers);

private:
    struct ValidationResult {
        bool valid = true;
        int row = -1;
        QString messageKey;
    };

    void buildUi();
    void addServer();
    void deleteSelectedServer();
    void moveSelectedServer(int offset);
    void restoreDefaults();
    void requestSave();
    void updateControlState();
    void clearValidation();
    void showValidation(const ValidationResult &result);
    ValidationResult validateServers() const;
    static bool isValidServerAddress(const QString &address);

    QStringList defaultServers_;
    QString validationMessageKey_;
    bool saving_ = false;

    QLabel *addressLabel_ = nullptr;
    QLineEdit *addressEdit_ = nullptr;
    QPushButton *addButton_ = nullptr;
    QListWidget *serverList_ = nullptr;
    QToolButton *deleteButton_ = nullptr;
    QToolButton *moveUpButton_ = nullptr;
    QToolButton *moveDownButton_ = nullptr;
    QLabel *validationLabel_ = nullptr;
    QPushButton *restoreButton_ = nullptr;
    QPushButton *cancelButton_ = nullptr;
    QPushButton *saveButton_ = nullptr;
};

} // namespace TimeSync::Ui
