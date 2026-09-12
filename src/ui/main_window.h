#pragma once

#include "ui_types.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMainWindow>

class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QResizeEvent;
class QScrollArea;
class QSpinBox;
class QTimer;

namespace TimeSync::Ui {

class ServerEditorDialog;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void refreshRequested();
    void manualSyncRequested();
    void serversSaveRequested(const QStringList &servers);
    void scheduleChangeRequested(bool enabled, int intervalMinutes);
    void scheduleRemoveRequested();
    void languageChangeRequested(const QString &languageTag);

public slots:
    void setReferenceState(const TimeSync::Ui::ReferenceState &state);
    void setBusyState(const TimeSync::Ui::BusyState &state);
    void setScheduleState(const TimeSync::Ui::ScheduleState &state);
    void setServerList(const QStringList &servers);
    void setDefaultServerList(const QStringList &servers);
    void setOperationResult(const TimeSync::Ui::OperationResult &result);
    void setLanguage(const QString &languageTag);
    void setSystemClockReadSuspended(bool suspended);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildUi();
    void applyStyle();
    void retranslateUi();
    void updateClockDisplay();
    void updateReferenceDisplay();
    void updateBusyDisplay();
    void updateScheduleDisplay();
    void updateServerDisplay();
    void updateResponsiveLayout();
    void showServerEditor();
    void showOperationBanner(const OperationResult &result);
    QString operationMessage(const OperationResult &result) const;
    QString referenceDetail() const;
    QString calibrationAgeText(qint64 totalSeconds) const;
    QString formattedDate(const QDateTime &dateTime) const;
    QString systemTimeZoneText() const;
    QString timeDifferenceText(qint64 differenceMilliseconds) const;
    void captureTimeZoneSnapshot();
    QDateTime displayedSystemWallClock(qint64 utcUnixMilliseconds) const;
    QString scheduleStatusText() const;
    void setTone(QWidget *widget, const QString &tone);

    QFrame *headerFrame_ = nullptr;
    QLabel *appTitleLabel_ = nullptr;
    QLabel *languageLabel_ = nullptr;
    QComboBox *languageCombo_ = nullptr;

    QFrame *operationBanner_ = nullptr;
    QLabel *operationIconLabel_ = nullptr;
    QLabel *operationTextLabel_ = nullptr;
    QPushButton *dismissBannerButton_ = nullptr;

    QFrame *referencePanel_ = nullptr;
    QGridLayout *clockGrid_ = nullptr;
    QFrame *standardClockFrame_ = nullptr;
    QFrame *systemClockFrame_ = nullptr;
    QLabel *referenceHeadingLabel_ = nullptr;
    QLabel *timeLabel_ = nullptr;
    QLabel *dateLabel_ = nullptr;
    QLabel *timezoneLabel_ = nullptr;
    QLabel *systemHeadingLabel_ = nullptr;
    QLabel *systemTimeLabel_ = nullptr;
    QLabel *systemDateLabel_ = nullptr;
    QLabel *systemTimezoneLabel_ = nullptr;
    QLabel *timeDifferenceLabel_ = nullptr;
    QLabel *statusBadge_ = nullptr;
    QLabel *referenceDetailLabel_ = nullptr;
    QLabel *sourceCaptionLabel_ = nullptr;
    QLabel *sourceValueLabel_ = nullptr;
    QLabel *statusCaptionLabel_ = nullptr;
    QLabel *statusValueLabel_ = nullptr;
    QLabel *calibrationCaptionLabel_ = nullptr;
    QLabel *calibrationValueLabel_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *syncButton_ = nullptr;

    QFrame *schedulePanel_ = nullptr;
    QLabel *scheduleHeadingLabel_ = nullptr;
    QCheckBox *scheduleEnabledCheck_ = nullptr;
    QLabel *intervalLabel_ = nullptr;
    QSpinBox *intervalSpinBox_ = nullptr;
    QLabel *scheduleStatusCaptionLabel_ = nullptr;
    QLabel *scheduleStatusValueLabel_ = nullptr;
    QPushButton *applyScheduleButton_ = nullptr;
    QPushButton *removeScheduleButton_ = nullptr;

    QFrame *serversPanel_ = nullptr;
    QLabel *serversHeadingLabel_ = nullptr;
    QLabel *serverCountLabel_ = nullptr;
    QListWidget *serverList_ = nullptr;
    QPushButton *editServersButton_ = nullptr;
    QGridLayout *lowerGrid_ = nullptr;
    QScrollArea *contentScrollArea_ = nullptr;
    QWidget *contentWidget_ = nullptr;

    QTimer *clockTimer_ = nullptr;
    QElapsedTimer referenceElapsed_;
    QString cachedZoneAbbreviation_;
    int cachedOffsetSeconds_ = 0;
    bool systemClockReadSuspended_ = false;
    ReferenceState referenceState_;
    BusyState busyState_;
    ScheduleState scheduleState_;
    QStringList servers_;
    QStringList defaultServers_;
    OperationResult lastOperationResult_;
    bool hasOperationResult_ = false;
    ServerEditorDialog *serverEditor_ = nullptr;
};

} // namespace TimeSync::Ui
