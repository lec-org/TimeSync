#include "main_window.h"

#include "about_dialog.h"
#include "brand_assets.h"
#include "server_editor_dialog.h"
#include "ui_strings.h"

#include "../platform/windows_system_clock.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <QWidget>

namespace TimeSync::Ui {
namespace {

constexpr int BeijingUtcOffsetSeconds = 8 * 60 * 60;

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QStringList builtInDefaultServers()
{
    return {
        QStringLiteral("ntp1.aliyun.com"),
        QStringLiteral("ntp2.aliyun.com"),
        QStringLiteral("ntp3.aliyun.com"),
        QStringLiteral("ntp4.aliyun.com"),
        QStringLiteral("ntp5.aliyun.com"),
        QStringLiteral("ntp6.aliyun.com"),
        QStringLiteral("ntp7.aliyun.com"),
        QStringLiteral("s1a.time.edu.cn"),
        QStringLiteral("s1b.time.edu.cn"),
        QStringLiteral("s1c.time.edu.cn"),
        QStringLiteral("s1d.time.edu.cn"),
        QStringLiteral("s1e.time.edu.cn"),
        QStringLiteral("s2a.time.edu.cn"),
        QStringLiteral("s2b.time.edu.cn"),
        QStringLiteral("s2c.time.edu.cn"),
        QStringLiteral("s2d.time.edu.cn"),
        QStringLiteral("s2e.time.edu.cn"),
        QStringLiteral("s2f.time.edu.cn"),
        QStringLiteral("s2g.time.edu.cn"),
        QStringLiteral("s2h.time.edu.cn"),
        QStringLiteral("s2j.time.edu.cn"),
        QStringLiteral("s2k.time.edu.cn"),
        QStringLiteral("s2m.time.edu.cn"),
    };
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , defaultServers_(builtInDefaultServers())
{
    qRegisterMetaType<ReferenceState>();
    qRegisterMetaType<BusyState>();
    qRegisterMetaType<ScheduleState>();
    qRegisterMetaType<OperationResult>();

    setMinimumSize(600, 440);
    resize(820, 600);
    setWindowIcon(QIcon(QStringLiteral(":/branding/lec-logo.png")));
    buildUi();
    applyStyle();
    retranslateUi();
    updateReferenceDisplay();
    updateScheduleDisplay();
    updateServerDisplay();
    updateBusyDisplay();
    updateResponsiveLayout();

    clockTimer_ = new QTimer(this);
    clockTimer_->setTimerType(Qt::PreciseTimer);
    clockTimer_->setInterval(250);
    connect(clockTimer_, &QTimer::timeout, this, &MainWindow::updateClockDisplay);
    captureTimeZoneSnapshot();
    clockTimer_->start();
    QTimer::singleShot(0, this, &MainWindow::updateResponsiveLayout);
}

MainWindow::~MainWindow() = default;

void MainWindow::setReferenceState(const ReferenceState &state)
{
    ReferenceState nextState = state;
    const bool incomingHasValidReference = state.hasReferenceTime && state.beijingTime.isValid();
    const bool shouldRetainReference = referenceState_.hasReferenceTime
        && referenceState_.beijingTime.isValid()
        && !incomingHasValidReference
        && (state.status == ReferenceStatus::Loading
            || state.status == ReferenceStatus::Stale
            || state.status == ReferenceStatus::Error);

    if (shouldRetainReference) {
        const qint64 elapsedMilliseconds = referenceElapsed_.isValid() ? referenceElapsed_.elapsed() : 0;
        nextState.hasReferenceTime = true;
        nextState.beijingTime = referenceState_.beijingTime.addMSecs(elapsedMilliseconds);
        nextState.source = state.source.trimmed().isEmpty() ? referenceState_.source : state.source;
        if (referenceState_.calibrationAgeSeconds >= 0) {
            nextState.calibrationAgeSeconds = referenceState_.calibrationAgeSeconds
                + elapsedMilliseconds / 1000;
        }
    }

    if (!nextState.hasReferenceTime || !nextState.beijingTime.isValid()) {
        nextState.hasReferenceTime = false;
        if (nextState.status == ReferenceStatus::Current
            || nextState.status == ReferenceStatus::Stale) {
            nextState.status = ReferenceStatus::NoData;
            nextState.failure = ReferenceFailure::None;
        }
    }

    referenceState_ = nextState;
    if (referenceState_.hasReferenceTime && referenceState_.beijingTime.isValid()) {
        referenceElapsed_.start();
    } else {
        referenceElapsed_.invalidate();
    }
    updateReferenceDisplay();
    updateBusyDisplay();
}

void MainWindow::setBusyState(const BusyState &state)
{
    busyState_ = state;
    updateBusyDisplay();
    if (serverEditor_ != nullptr) {
        serverEditor_->setSaving(busyState_.savingServers);
    }
}

void MainWindow::setScheduleState(const ScheduleState &state)
{
    scheduleState_ = state;
    {
        const QSignalBlocker checkBlocker(scheduleEnabledCheck_);
        const QSignalBlocker intervalBlocker(intervalSpinBox_);
        scheduleEnabledCheck_->setChecked(scheduleState_.enabled);
        intervalSpinBox_->setValue(qMax(1, scheduleState_.intervalMinutes));
    }
    updateScheduleDisplay();
    updateBusyDisplay();
}

void MainWindow::setServerList(const QStringList &servers)
{
    servers_ = servers;
    updateServerDisplay();
}

void MainWindow::setDefaultServerList(const QStringList &servers)
{
    if (!servers.isEmpty()) {
        defaultServers_ = servers;
    }
}

void MainWindow::setOperationResult(const OperationResult &result)
{
    lastOperationResult_ = result;
    const bool showBanner = shouldShowOperationBanner(result);
    hasOperationResult_ = showBanner;
    switch (result.operation) {
    case OperationKind::Refresh:
        busyState_.refreshing = false;
        break;
    case OperationKind::ManualSync:
        busyState_.syncing = false;
        break;
    case OperationKind::SaveServers:
        busyState_.savingServers = false;
        if (serverEditor_ != nullptr) {
            if (result.success) {
                servers_ = serverEditor_->servers();
                updateServerDisplay();
                serverEditor_->accept();
            } else {
                serverEditor_->showSaveError(operationMessage(result));
            }
        }
        break;
    case OperationKind::ChangeSchedule:
        busyState_.changingSchedule = false;
        break;
    case OperationKind::RemoveSchedule:
        busyState_.removingSchedule = false;
        break;
    }

    updateBusyDisplay();
    if (showBanner) {
        showOperationBanner(result);
    } else {
        ++bannerGeneration_;
        operationBanner_->hide();
    }
}

void MainWindow::setLanguage(const QString &languageTag)
{
    if (!UiStrings::setLanguageTag(languageTag)) {
        return;
    }
    retranslateUi();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateResponsiveLayout();
}

void MainWindow::buildUi()
{
    setObjectName(QStringLiteral("mainWindow"));

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    headerFrame_ = new QFrame(central);
    headerFrame_->setObjectName(QStringLiteral("headerFrame"));
    headerFrame_->setMinimumHeight(64);
    auto *headerLayout = new QHBoxLayout(headerFrame_);
    headerLayout->setContentsMargins(20, 10, 20, 10);
    headerLayout->setSpacing(12);
    logoLabel_ = new QLabel(headerFrame_);
    logoLabel_->setObjectName(QStringLiteral("brandLogo"));
    logoLabel_->setAlignment(Qt::AlignCenter);
    applyHeaderLogo();
    appTitleLabel_ = new QLabel(headerFrame_);
    appTitleLabel_->setObjectName(QStringLiteral("appTitle"));
    aboutButton_ = new QPushButton(headerFrame_);
    aboutButton_->setObjectName(QStringLiteral("secondaryButton"));
    aboutButton_->setCursor(Qt::PointingHandCursor);
    aboutButton_->setMinimumSize(72, 40);
    languageLabel_ = new QLabel(headerFrame_);
    languageLabel_->setObjectName(QStringLiteral("fieldLabel"));
    languageCombo_ = new QComboBox(headerFrame_);
    languageCombo_->setMinimumSize(124, 40);
    languageLabel_->setBuddy(languageCombo_);
    headerLayout->addWidget(logoLabel_);
    headerLayout->addWidget(appTitleLabel_);
    headerLayout->addStretch(1);
    headerLayout->addWidget(aboutButton_);
    headerLayout->addWidget(languageLabel_);
    headerLayout->addWidget(languageCombo_);
    rootLayout->addWidget(headerFrame_);

    contentScrollArea_ = new QScrollArea(central);
    contentScrollArea_->setObjectName(QStringLiteral("contentScrollArea"));
    contentScrollArea_->setWidgetResizable(true);
    contentScrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *scrollContent = new QWidget(contentScrollArea_);
    scrollContent->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *shellLayout = new QHBoxLayout(scrollContent);
    shellLayout->setContentsMargins(12, 0, 12, 0);
    shellLayout->setSpacing(0);
    contentWidget_ = new QWidget(scrollContent);
    contentWidget_->setMaximumWidth(1040);
    contentWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *contentLayout = new QVBoxLayout(contentWidget_);
    contentLayout->setContentsMargins(6, 12, 6, 16);
    contentLayout->setSpacing(12);

    referencePanel_ = new QFrame(contentWidget_);
    referencePanel_->setObjectName(QStringLiteral("surfacePanel"));
    auto *referenceLayout = new QVBoxLayout(referencePanel_);
    referenceLayout->setContentsMargins(18, 12, 18, 12);
    referenceLayout->setSpacing(4);

    clockGrid_ = new QGridLayout;
    clockGrid_->setContentsMargins(0, 0, 0, 0);
    clockGrid_->setHorizontalSpacing(10);
    clockGrid_->setVerticalSpacing(8);

    standardClockFrame_ = new QFrame(referencePanel_);
    standardClockFrame_->setObjectName(QStringLiteral("standardClock"));
    auto *standardClockLayout = new QVBoxLayout(standardClockFrame_);
    standardClockLayout->setContentsMargins(14, 9, 14, 10);
    standardClockLayout->setSpacing(1);
    referenceHeadingLabel_ = new QLabel(standardClockFrame_);
    referenceHeadingLabel_->setObjectName(QStringLiteral("sectionHeading"));
    referenceHeadingLabel_->setAlignment(Qt::AlignHCenter);
    timeLabel_ = new QLabel(standardClockFrame_);
    timeLabel_->setObjectName(QStringLiteral("timeLabel"));
    timeLabel_->setAlignment(Qt::AlignCenter);
    timeLabel_->setMinimumHeight(48);
    dateLabel_ = new QLabel(standardClockFrame_);
    dateLabel_->setObjectName(QStringLiteral("dateLabel"));
    dateLabel_->setAlignment(Qt::AlignCenter);
    dateLabel_->setWordWrap(true);
    timezoneLabel_ = new QLabel(standardClockFrame_);
    timezoneLabel_->setObjectName(QStringLiteral("secondaryText"));
    timezoneLabel_->setAlignment(Qt::AlignCenter);
    timezoneLabel_->setWordWrap(true);
    standardClockLayout->addWidget(referenceHeadingLabel_);
    standardClockLayout->addWidget(timeLabel_);
    standardClockLayout->addWidget(dateLabel_);
    standardClockLayout->addWidget(timezoneLabel_);

    systemClockFrame_ = new QFrame(referencePanel_);
    systemClockFrame_->setObjectName(QStringLiteral("systemClock"));
    auto *systemClockLayout = new QVBoxLayout(systemClockFrame_);
    systemClockLayout->setContentsMargins(14, 9, 14, 10);
    systemClockLayout->setSpacing(1);
    systemHeadingLabel_ = new QLabel(systemClockFrame_);
    systemHeadingLabel_->setObjectName(QStringLiteral("sectionHeading"));
    systemHeadingLabel_->setAlignment(Qt::AlignHCenter);
    systemTimeLabel_ = new QLabel(systemClockFrame_);
    systemTimeLabel_->setObjectName(QStringLiteral("systemTimeLabel"));
    systemTimeLabel_->setAlignment(Qt::AlignCenter);
    systemTimeLabel_->setMinimumHeight(44);
    systemDateLabel_ = new QLabel(systemClockFrame_);
    systemDateLabel_->setObjectName(QStringLiteral("dateLabel"));
    systemDateLabel_->setAlignment(Qt::AlignCenter);
    systemDateLabel_->setWordWrap(true);
    systemTimezoneLabel_ = new QLabel(systemClockFrame_);
    systemTimezoneLabel_->setObjectName(QStringLiteral("secondaryText"));
    systemTimezoneLabel_->setAlignment(Qt::AlignCenter);
    systemTimezoneLabel_->setWordWrap(true);
    systemClockLayout->addWidget(systemHeadingLabel_);
    systemClockLayout->addWidget(systemTimeLabel_);
    systemClockLayout->addWidget(systemDateLabel_);
    systemClockLayout->addWidget(systemTimezoneLabel_);

    clockGrid_->addWidget(standardClockFrame_, 0, 0);
    clockGrid_->addWidget(systemClockFrame_, 0, 1);
    clockGrid_->setColumnStretch(0, 1);
    clockGrid_->setColumnStretch(1, 1);
    referenceLayout->addLayout(clockGrid_);

    timeDifferenceLabel_ = new QLabel(referencePanel_);
    timeDifferenceLabel_->setObjectName(QStringLiteral("comparisonText"));
    timeDifferenceLabel_->setAlignment(Qt::AlignCenter);
    timeDifferenceLabel_->setWordWrap(true);
    timeDifferenceLabel_->setVisible(false);
    referenceLayout->addWidget(timeDifferenceLabel_);

    statusBadge_ = new QLabel(referencePanel_);
    statusBadge_->setObjectName(QStringLiteral("statusBadge"));
    statusBadge_->setAlignment(Qt::AlignCenter);
    statusBadge_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    referenceDetailLabel_ = new QLabel(referencePanel_);
    referenceDetailLabel_->setObjectName(QStringLiteral("secondaryText"));
    referenceDetailLabel_->setAlignment(Qt::AlignCenter);
    referenceDetailLabel_->setWordWrap(true);

    referenceLayout->addWidget(statusBadge_, 0, Qt::AlignHCenter);
    referenceLayout->addWidget(referenceDetailLabel_);

    auto *metadataLayout = new QGridLayout;
    metadataLayout->setContentsMargins(0, 4, 0, 1);
    metadataLayout->setHorizontalSpacing(16);
    metadataLayout->setVerticalSpacing(2);
    sourceCaptionLabel_ = new QLabel(referencePanel_);
    sourceCaptionLabel_->setObjectName(QStringLiteral("metaCaption"));
    sourceValueLabel_ = new QLabel(referencePanel_);
    sourceValueLabel_->setObjectName(QStringLiteral("metaValue"));
    sourceValueLabel_->setAlignment(Qt::AlignCenter);
    sourceValueLabel_->setWordWrap(true);
    statusCaptionLabel_ = new QLabel(referencePanel_);
    statusCaptionLabel_->setObjectName(QStringLiteral("metaCaption"));
    statusValueLabel_ = new QLabel(referencePanel_);
    statusValueLabel_->setObjectName(QStringLiteral("metaValue"));
    statusValueLabel_->setAlignment(Qt::AlignCenter);
    statusValueLabel_->setWordWrap(true);
    calibrationCaptionLabel_ = new QLabel(referencePanel_);
    calibrationCaptionLabel_->setObjectName(QStringLiteral("metaCaption"));
    calibrationValueLabel_ = new QLabel(referencePanel_);
    calibrationValueLabel_->setObjectName(QStringLiteral("metaValue"));
    calibrationValueLabel_->setAlignment(Qt::AlignCenter);
    calibrationValueLabel_->setWordWrap(true);

    metadataLayout->addWidget(sourceCaptionLabel_, 0, 0, Qt::AlignHCenter);
    metadataLayout->addWidget(statusCaptionLabel_, 0, 1, Qt::AlignHCenter);
    metadataLayout->addWidget(calibrationCaptionLabel_, 0, 2, Qt::AlignHCenter);
    metadataLayout->addWidget(sourceValueLabel_, 1, 0);
    metadataLayout->addWidget(statusValueLabel_, 1, 1);
    metadataLayout->addWidget(calibrationValueLabel_, 1, 2);
    metadataLayout->setColumnStretch(0, 1);
    metadataLayout->setColumnStretch(1, 1);
    metadataLayout->setColumnStretch(2, 1);
    referenceLayout->addLayout(metadataLayout);

    auto *referenceButtonLayout = new QHBoxLayout;
    referenceButtonLayout->setContentsMargins(0, 2, 0, 0);
    referenceButtonLayout->setSpacing(8);
    referenceButtonLayout->addStretch(1);
    refreshButton_ = new QPushButton(referencePanel_);
    refreshButton_->setObjectName(QStringLiteral("secondaryButton"));
    refreshButton_->setCursor(Qt::PointingHandCursor);
    syncButton_ = new QPushButton(referencePanel_);
    syncButton_->setObjectName(QStringLiteral("primaryButton"));
    syncButton_->setCursor(Qt::PointingHandCursor);
    syncButton_->setMinimumWidth(220);
    referenceButtonLayout->addWidget(refreshButton_);
    referenceButtonLayout->addWidget(syncButton_);
    referenceButtonLayout->addStretch(1);
    referenceLayout->addLayout(referenceButtonLayout);

    operationBanner_ = new QFrame(referencePanel_);
    operationBanner_->setObjectName(QStringLiteral("operationBanner"));
    operationBanner_->setVisible(false);
    auto *bannerLayout = new QHBoxLayout(operationBanner_);
    bannerLayout->setContentsMargins(12, 8, 4, 8);
    bannerLayout->setSpacing(8);
    operationIconLabel_ = new QLabel(operationBanner_);
    operationIconLabel_->setFixedSize(18, 18);
    operationIconLabel_->setAlignment(Qt::AlignCenter);
    operationTextLabel_ = new QLabel(operationBanner_);
    operationTextLabel_->setObjectName(QStringLiteral("noticeText"));
    operationTextLabel_->setWordWrap(true);
    dismissBannerButton_ = new QPushButton(operationBanner_);
    dismissBannerButton_->setObjectName(QStringLiteral("bannerDismissButton"));
    dismissBannerButton_->setFlat(true);
    dismissBannerButton_->setText(QStringLiteral("×"));
    dismissBannerButton_->setFixedSize(40, 40);
    bannerLayout->addWidget(operationIconLabel_);
    bannerLayout->addWidget(operationTextLabel_, 1);
    bannerLayout->addWidget(dismissBannerButton_);
    referenceLayout->addWidget(operationBanner_);
    contentLayout->addWidget(referencePanel_);

    lowerGrid_ = new QGridLayout;
    lowerGrid_->setContentsMargins(0, 0, 0, 0);
    lowerGrid_->setHorizontalSpacing(12);
    lowerGrid_->setVerticalSpacing(12);

    schedulePanel_ = new QFrame(contentWidget_);
    schedulePanel_->setObjectName(QStringLiteral("surfacePanel"));
    auto *scheduleLayout = new QGridLayout(schedulePanel_);
    scheduleLayout->setContentsMargins(16, 13, 16, 14);
    scheduleLayout->setHorizontalSpacing(10);
    scheduleLayout->setVerticalSpacing(6);
    scheduleHeadingLabel_ = new QLabel(schedulePanel_);
    scheduleHeadingLabel_->setObjectName(QStringLiteral("sectionHeading"));
    scheduleEnabledCheck_ = new QCheckBox(schedulePanel_);
    scheduleEnabledCheck_->setMinimumHeight(40);
    intervalLabel_ = new QLabel(schedulePanel_);
    intervalLabel_->setObjectName(QStringLiteral("fieldLabel"));
    intervalSpinBox_ = new QSpinBox(schedulePanel_);
    intervalSpinBox_->setRange(1, 10080); // Must match ConfigRepository::MaximumScheduleIntervalMinutes.
    intervalSpinBox_->setAccelerated(true);
    intervalSpinBox_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    intervalSpinBox_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    intervalSpinBox_->setMinimumHeight(40);
    intervalSpinBox_->setMinimumWidth(144);
    intervalSpinBox_->setMaximumWidth(200);
    intervalLabel_->setBuddy(intervalSpinBox_);
    scheduleStatusCaptionLabel_ = new QLabel(schedulePanel_);
    scheduleStatusCaptionLabel_->setObjectName(QStringLiteral("fieldLabel"));
    scheduleStatusValueLabel_ = new QLabel(schedulePanel_);
    scheduleStatusValueLabel_->setObjectName(QStringLiteral("scheduleStatus"));
    scheduleStatusValueLabel_->setWordWrap(true);
    applyScheduleButton_ = new QPushButton(schedulePanel_);
    applyScheduleButton_->setObjectName(QStringLiteral("primaryButton"));
    applyScheduleButton_->setCursor(Qt::PointingHandCursor);
    removeScheduleButton_ = new QPushButton(schedulePanel_);
    removeScheduleButton_->setObjectName(QStringLiteral("dangerButton"));
    removeScheduleButton_->setCursor(Qt::PointingHandCursor);
    scheduleLayout->addWidget(scheduleHeadingLabel_, 0, 0, 1, 2);
    scheduleLayout->addWidget(scheduleEnabledCheck_, 1, 0, 1, 2);
    scheduleLayout->addWidget(intervalLabel_, 2, 0);
    scheduleLayout->addWidget(intervalSpinBox_, 2, 1);
    scheduleLayout->addWidget(scheduleStatusCaptionLabel_, 3, 0);
    scheduleLayout->addWidget(scheduleStatusValueLabel_, 3, 1);
    scheduleLayout->setRowStretch(4, 1);
    auto *scheduleButtons = new QHBoxLayout;
    scheduleButtons->setSpacing(8);
    scheduleButtons->addWidget(removeScheduleButton_);
    scheduleButtons->addStretch(1);
    scheduleButtons->addWidget(applyScheduleButton_);
    scheduleLayout->addLayout(scheduleButtons, 5, 0, 1, 2);

    serversPanel_ = new QFrame(contentWidget_);
    serversPanel_->setObjectName(QStringLiteral("surfacePanel"));
    auto *serversLayout = new QVBoxLayout(serversPanel_);
    serversLayout->setContentsMargins(16, 13, 16, 14);
    serversLayout->setSpacing(6);
    auto *serverHeadingLayout = new QHBoxLayout;
    serverHeadingLayout->setSpacing(8);
    serversHeadingLabel_ = new QLabel(serversPanel_);
    serversHeadingLabel_->setObjectName(QStringLiteral("sectionHeading"));
    serverCountLabel_ = new QLabel(serversPanel_);
    serverCountLabel_->setObjectName(QStringLiteral("secondaryText"));
    serverHeadingLayout->addWidget(serversHeadingLabel_);
    serverHeadingLayout->addStretch(1);
    serverHeadingLayout->addWidget(serverCountLabel_);
    serversLayout->addLayout(serverHeadingLayout);
    serverList_ = new QListWidget(serversPanel_);
    serverList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    serverList_->setSelectionMode(QAbstractItemView::NoSelection);
    serverList_->setFocusPolicy(Qt::StrongFocus);
    serverList_->setMinimumHeight(82);
    serverList_->setUniformItemSizes(true);
    serverList_->setTextElideMode(Qt::ElideMiddle);
    serverList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    serversLayout->addWidget(serverList_, 1);
    editServersButton_ = new QPushButton(serversPanel_);
    editServersButton_->setObjectName(QStringLiteral("secondaryButton"));
    editServersButton_->setCursor(Qt::PointingHandCursor);
    serversLayout->addWidget(editServersButton_, 0, Qt::AlignRight);

    lowerGrid_->addWidget(schedulePanel_, 0, 0);
    lowerGrid_->addWidget(serversPanel_, 0, 1);
    lowerGrid_->setColumnStretch(0, 1);
    lowerGrid_->setColumnStretch(1, 1);
    contentLayout->addLayout(lowerGrid_);
    contentLayout->addStretch(1);

    shellLayout->addStretch(1);
    shellLayout->addWidget(contentWidget_, 1);
    shellLayout->addStretch(1);
    contentScrollArea_->setWidget(scrollContent);
    rootLayout->addWidget(contentScrollArea_, 1);
    setCentralWidget(central);

    connect(dismissBannerButton_, &QPushButton::clicked, this, [this] {
        ++bannerGeneration_;
        hasOperationResult_ = false;
        operationBanner_->hide();
    });
    connect(languageCombo_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        const QString languageTag = languageCombo_->itemData(index).toString();
        if (languageTag == UiStrings::languageTag() || !UiStrings::setLanguageTag(languageTag)) {
            return;
        }
        retranslateUi();
        emit languageChangeRequested(languageTag);
    });
    connect(refreshButton_, &QPushButton::clicked, this, [this] {
        hasOperationResult_ = false;
        operationBanner_->hide();
        busyState_.refreshing = true;
        updateBusyDisplay();
        emit refreshRequested();
    });
    connect(syncButton_, &QPushButton::clicked, this, [this] {
        hasOperationResult_ = false;
        operationBanner_->hide();
        busyState_.syncing = true;
        updateBusyDisplay();
        emit manualSyncRequested();
    });
    connect(scheduleEnabledCheck_, &QCheckBox::toggled, this, [this](bool enabled) {
        intervalSpinBox_->setEnabled(enabled && !busyState_.changingSchedule
                                     && !busyState_.removingSchedule);
    });
    connect(applyScheduleButton_, &QPushButton::clicked, this, [this] {
        hasOperationResult_ = false;
        operationBanner_->hide();
        busyState_.changingSchedule = true;
        updateBusyDisplay();
        emit scheduleChangeRequested(scheduleEnabledCheck_->isChecked(), intervalSpinBox_->value());
    });
    connect(removeScheduleButton_, &QPushButton::clicked, this, [this] {
        QMessageBox confirmation(this);
        confirmation.setIcon(QMessageBox::Warning);
        confirmation.setWindowTitle(UiStrings::text(QStringLiteral("schedule.remove.title")));
        confirmation.setText(UiStrings::text(QStringLiteral("schedule.remove.message")));
        QPushButton *removeButton = confirmation.addButton(
            UiStrings::text(QStringLiteral("action.remove")), QMessageBox::DestructiveRole);
        QPushButton *cancelButton = confirmation.addButton(
            UiStrings::text(QStringLiteral("action.cancel")), QMessageBox::RejectRole);
        removeButton->setObjectName(QStringLiteral("dangerButton"));
        cancelButton->setObjectName(QStringLiteral("secondaryButton"));
        confirmation.setDefaultButton(cancelButton);
        confirmation.exec();
        if (confirmation.clickedButton() != removeButton) {
            return;
        }
        hasOperationResult_ = false;
        operationBanner_->hide();
        busyState_.removingSchedule = true;
        updateBusyDisplay();
        emit scheduleRemoveRequested();
    });
    connect(editServersButton_, &QPushButton::clicked, this, &MainWindow::showServerEditor);
    connect(aboutButton_, &QPushButton::clicked, this, &MainWindow::showAboutDialog);

    QWidget::setTabOrder(aboutButton_, languageCombo_);
    QWidget::setTabOrder(languageCombo_, refreshButton_);
    QWidget::setTabOrder(refreshButton_, syncButton_);
    QWidget::setTabOrder(syncButton_, scheduleEnabledCheck_);
    QWidget::setTabOrder(scheduleEnabledCheck_, intervalSpinBox_);
    QWidget::setTabOrder(intervalSpinBox_, removeScheduleButton_);
    QWidget::setTabOrder(removeScheduleButton_, applyScheduleButton_);
    QWidget::setTabOrder(applyScheduleButton_, serverList_);
    QWidget::setTabOrder(serverList_, editServersButton_);
}

void MainWindow::applyStyle()
{
    setStyleSheet(QStringLiteral(R"(
        * {
            color: #172033;
            font-family: "Segoe UI", "Microsoft YaHei UI", sans-serif;
            font-size: 14px;
        }
        QMainWindow#mainWindow, QScrollArea#contentScrollArea, QScrollArea#contentScrollArea > QWidget > QWidget {
            background: #F3F6FA;
        }
        QScrollArea#contentScrollArea { border: 0; }
        QFrame#headerFrame {
            background: #FFFFFF;
            border: 0;
            border-bottom: 2px solid #005AAE;
        }
        QLabel#appTitle {
            color: #172033;
            font-size: 20px;
            font-weight: 600;
        }
        QFrame#surfacePanel {
            background: #FFFFFF;
            border: 1px solid #D7E2EE;
            border-radius: 10px;
        }
        QFrame#standardClock {
            background: #EEF5FC;
            border: 1px solid #C5DCF0;
            border-radius: 10px;
        }
        QFrame#systemClock {
            background: #F8FAFC;
            border: 1px solid #E6E9ED;
            border-radius: 10px;
        }
        QLabel#sectionHeading {
            color: #172033;
            font-size: 15px;
            font-weight: 600;
        }
        QLabel#timeLabel, QLabel#systemTimeLabel {
            color: #005AAE;
            font-family: "Cascadia Mono", "Consolas", monospace;
            font-size: 36px;
            font-weight: 600;
        }
        QLabel#systemTimeLabel { color: #475467; font-size: 32px; }
        QLabel#dateLabel {
            color: #344054;
            font-size: 14px;
            font-weight: 500;
        }
        QLabel#secondaryText { color: #667085; }
        QLabel#comparisonText {
            color: #667085;
            font-size: 12px;
        }
        QLabel#metaCaption {
            color: #667085;
            font-size: 12px;
        }
        QLabel#metaValue {
            color: #344054;
            font-size: 13px;
            font-weight: 600;
        }
        QLabel#fieldLabel {
            color: #475467;
            font-size: 13px;
            font-weight: 500;
        }
        QLabel#scheduleStatus {
            color: #344054;
            font-weight: 600;
        }
        QLabel#statusBadge {
            padding: 4px 10px;
            border-radius: 6px;
            font-size: 13px;
            font-weight: 600;
        }
        QLabel#statusBadge[tone="neutral"] { color: #475467; background: #EEF1F4; }
        QLabel#statusBadge[tone="info"] { color: #005AAE; background: #EAF4FC; }
        QLabel#statusBadge[tone="success"] { color: #176B43; background: #E8F6EE; }
        QLabel#statusBadge[tone="warning"] { color: #8A4B08; background: #FFF2D8; }
        QLabel#statusBadge[tone="error"] { color: #A61B12; background: #FDEBEA; }
        QFrame#operationBanner {
            background: #EEF1F4;
            border: 0;
            border-radius: 8px;
        }
        QFrame#operationBanner[tone="success"] { background: #E8F6EE; }
        QFrame#operationBanner[tone="warning"] { background: #FFF2D8; }
        QFrame#operationBanner[tone="error"] { background: #FDEBEA; }
        QLabel#noticeText { color: #344054; font-size: 13px; font-weight: 600; }
        QLabel#noticeText[tone="success"] { color: #176B43; }
        QLabel#noticeText[tone="warning"] { color: #8A4B08; }
        QLabel#noticeText[tone="error"] { color: #A61B12; }
        QPushButton, QToolButton {
            min-height: 40px;
            padding: 0 14px;
            border-radius: 6px;
            font-weight: 600;
        }
        QPushButton#primaryButton {
            color: #FFFFFF;
            background: #005AAE;
            border: 1px solid #005AAE;
        }
        QPushButton#primaryButton:hover { background: #004E98; border-color: #004E98; }
        QPushButton#primaryButton:pressed { background: #003F7A; border-color: #003F7A; }
        QPushButton#secondaryButton, QToolButton#iconButton {
            color: #344054;
            background: #FFFFFF;
            border: 1px solid #C9D1DB;
        }
        QPushButton#secondaryButton:hover, QToolButton#iconButton:hover { background: #F6F8FA; }
        QPushButton#secondaryButton:pressed, QToolButton#iconButton:pressed { background: #E9EDF2; }
        QPushButton#dangerButton {
            color: #A61B12;
            background: #FFFFFF;
            border: 1px solid #D7A19D;
        }
        QPushButton#dangerButton:hover { background: #FDF2F1; }
        QPushButton#bannerDismissButton {
            padding: 0;
            background: transparent;
            border: 0;
        }
        QPushButton#bannerDismissButton:hover { background: rgba(23, 32, 51, 0.08); }
        QPushButton#bannerDismissButton:focus { border: 2px solid #005AAE; }
        QPushButton#bannerDismissButton { font-size: 18px; color: #667085; }
        QPushButton:disabled, QToolButton:disabled {
            color: #98A2B3;
            background: #EEF1F4;
            border-color: #DDE2E8;
        }
        QPushButton:focus, QToolButton:focus, QComboBox:focus, QSpinBox:focus,
        QLineEdit:focus, QListWidget:focus {
            border: 2px solid #005AAE;
        }
        QLineEdit, QSpinBox, QComboBox, QListWidget {
            color: #172033;
            background: #FFFFFF;
            border: 1px solid #C9D1DB;
            border-radius: 6px;
            selection-background-color: #D6E8F8;
            selection-color: #172033;
        }
        QLineEdit, QSpinBox, QComboBox { padding: 0 10px; }
        QSpinBox { min-width: 144px; }
        QLineEdit[error="true"] { border: 2px solid #B42318; }
        QListWidget { padding: 4px; outline: 0; }
        QListWidget::item {
            min-height: 30px;
            padding: 2px 8px;
            border-radius: 4px;
        }
        QListWidget::item:selected { background: #D6E8F8; color: #172033; }
        QCheckBox { spacing: 8px; }
        QCheckBox::indicator { width: 18px; height: 18px; }
        QCheckBox:focus { color: #005AAE; }
        QLabel#validationLabel { color: transparent; font-size: 13px; }
        QLabel#validationLabel[visibleError="true"] { color: #B42318; }
        QFrame#divider { color: #DDE2E8; }
        QDialog#serverEditorDialog, QDialog#aboutDialog { background: #FFFFFF; }
        QLabel#aboutStudio {
            color: #172033;
            font-size: 18px;
            font-weight: 600;
        }
        QLabel#aboutCaption {
            color: #667085;
            font-size: 13px;
            font-weight: 500;
        }
        QLabel#aboutLink { font-size: 14px; font-weight: 600; }
        QLabel#aboutLink a { color: #005AAE; text-decoration: none; }
        QFrame#aboutLinksFrame {
            background: #F3F7FB;
            border: 1px solid #D7E2EE;
            border-radius: 10px;
        }
        QScrollBar:vertical {
            width: 12px;
            background: #F3F6FA;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            min-height: 32px;
            background: #C9D1DB;
            border-radius: 5px;
            margin: 2px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    )"));
}

void MainWindow::retranslateUi()
{
    setWindowTitle(UiStrings::text(QStringLiteral("app.windowTitle")));
    appTitleLabel_->setText(UiStrings::text(QStringLiteral("app.title")));
    logoLabel_->setAccessibleName(UiStrings::text(QStringLiteral("about.logoAlt")));
    aboutButton_->setText(UiStrings::text(QStringLiteral("action.about")));
    languageLabel_->setText(UiStrings::text(QStringLiteral("language.label")));

    {
        const QSignalBlocker blocker(languageCombo_);
        languageCombo_->clear();
        languageCombo_->addItem(UiStrings::text(QStringLiteral("language.zhCN")), QStringLiteral("zh-CN"));
        languageCombo_->addItem(UiStrings::text(QStringLiteral("language.english")), QStringLiteral("en-US"));
        const int currentIndex = languageCombo_->findData(UiStrings::languageTag());
        languageCombo_->setCurrentIndex(qMax(0, currentIndex));
    }

    dismissBannerButton_->setToolTip(UiStrings::text(QStringLiteral("action.dismiss")));
    dismissBannerButton_->setAccessibleName(UiStrings::text(QStringLiteral("action.dismiss")));
    referenceHeadingLabel_->setText(UiStrings::text(QStringLiteral("reference.heading")));
    timezoneLabel_->setText(UiStrings::text(QStringLiteral("reference.timezone")));
    systemHeadingLabel_->setText(UiStrings::text(QStringLiteral("system.heading")));
    sourceCaptionLabel_->setText(UiStrings::text(QStringLiteral("reference.source")));
    statusCaptionLabel_->setText(UiStrings::text(QStringLiteral("reference.status")));
    calibrationCaptionLabel_->setText(UiStrings::text(QStringLiteral("reference.lastCalibration")));

    scheduleHeadingLabel_->setText(UiStrings::text(QStringLiteral("schedule.heading")));
    scheduleEnabledCheck_->setText(UiStrings::text(QStringLiteral("schedule.enabled")));
    intervalLabel_->setText(UiStrings::text(QStringLiteral("schedule.interval")));
    intervalSpinBox_->setSuffix(UiStrings::text(QStringLiteral("schedule.minutesSuffix")));
    scheduleStatusCaptionLabel_->setText(UiStrings::text(QStringLiteral("schedule.status")));
    serversHeadingLabel_->setText(UiStrings::text(QStringLiteral("servers.heading")));
    serverList_->setAccessibleName(UiStrings::text(QStringLiteral("servers.heading")));
    editServersButton_->setText(UiStrings::text(QStringLiteral("action.editServers")));

    if (aboutDialog_ != nullptr) {
        aboutDialog_->retranslateUi();
    }
    if (serverEditor_ != nullptr) {
        serverEditor_->retranslateUi();
        if (!busyState_.savingServers && hasOperationResult_ && !lastOperationResult_.success
            && lastOperationResult_.operation == OperationKind::SaveServers) {
            serverEditor_->showSaveError(operationMessage(lastOperationResult_));
        }
    }
    if (hasOperationResult_ && operationBanner_->isVisible()) {
        showOperationBanner(lastOperationResult_);
    }
    updateReferenceDisplay();
    updateScheduleDisplay();
    updateServerDisplay();
    updateBusyDisplay();
}

void MainWindow::setSystemClockReadSuspended(const bool suspended)
{
    if (suspended == systemClockReadSuspended_) {
        return;
    }
    if (suspended) {
        captureTimeZoneSnapshot();
    }
    systemClockReadSuspended_ = suspended;
    if (!suspended) {
        captureTimeZoneSnapshot();
    }
    updateClockDisplay();
}

void MainWindow::setSystemClockFollowsTrusted(const bool follow)
{
    if (follow == systemClockFollowsTrusted_) {
        return;
    }
    systemClockFollowsTrusted_ = follow;
    updateClockDisplay();
}

void MainWindow::captureTimeZoneSnapshot()
{
    const TimeSync::TimeZoneSnapshot snapshot = TimeSync::WindowsSystemClock::queryTimeZone();
    cachedZoneAbbreviation_ = snapshot.abbreviation;
    cachedOffsetSeconds_ = snapshot.offsetSeconds;
}

QDateTime MainWindow::displayedSystemWallClock(const qint64 utcUnixMilliseconds) const
{
    return QDateTime::fromMSecsSinceEpoch(
        utcUnixMilliseconds + static_cast<qint64>(cachedOffsetSeconds_) * 1000,
        QTimeZone(QTimeZone::UTC));
}

void MainWindow::updateClockDisplay()
{
    if (!systemClockReadSuspended_) {
        captureTimeZoneSnapshot();
    }
    qint64 systemUtcMs = TimeSync::WindowsSystemClock::utcUnixMilliseconds();
    systemTimezoneLabel_->setText(systemTimeZoneText());

    if (!referenceState_.hasReferenceTime || !referenceState_.beijingTime.isValid()) {
        const QDateTime systemTime = displayedSystemWallClock(systemUtcMs);
        systemTimeLabel_->setText(systemTime.toString(QStringLiteral("HH:mm:ss")));
        systemDateLabel_->setText(formattedDate(systemTime));
        timeLabel_->setText(QStringLiteral("--:--:--"));
        dateLabel_->setText(UiStrings::text(QStringLiteral("reference.dateUnavailable")));
        calibrationValueLabel_->setText(UiStrings::text(QStringLiteral("reference.never")));
        timeDifferenceLabel_->setVisible(false);
        return;
    }

    const qint64 elapsedMilliseconds = referenceElapsed_.isValid() ? referenceElapsed_.elapsed() : 0;
    const QDateTime beijingTime = referenceState_.beijingTime
                                      .addMSecs(elapsedMilliseconds)
                                      .toOffsetFromUtc(BeijingUtcOffsetSeconds);
    const qint64 beijingUtcMs = beijingTime.toMSecsSinceEpoch();
    if (systemClockFollowsTrusted_) {
        systemUtcMs = beijingUtcMs;
    }
    const QDateTime systemTime = displayedSystemWallClock(systemUtcMs);
    systemTimeLabel_->setText(systemTime.toString(QStringLiteral("HH:mm:ss")));
    systemDateLabel_->setText(formattedDate(systemTime));
    timeLabel_->setText(beijingTime.toString(QStringLiteral("HH:mm:ss")));
    dateLabel_->setText(formattedDate(beijingTime));
    timeDifferenceLabel_->setText(timeDifferenceText(systemUtcMs - beijingUtcMs));
    timeDifferenceLabel_->setVisible(true);

    const qint64 ageSeconds = referenceState_.calibrationAgeSeconds < 0
        ? -1
        : referenceState_.calibrationAgeSeconds + elapsedMilliseconds / 1000;
    calibrationValueLabel_->setText(ageSeconds < 0
                                        ? UiStrings::text(QStringLiteral("reference.ageUnavailable"))
                                        : calibrationAgeText(ageSeconds));
}

void MainWindow::updateReferenceDisplay()
{
    QString statusKey;
    QString tone;
    switch (referenceState_.status) {
    case ReferenceStatus::NoData:
        statusKey = QStringLiteral("reference.state.noData");
        tone = QStringLiteral("neutral");
        break;
    case ReferenceStatus::Loading:
        statusKey = QStringLiteral("reference.state.loading");
        tone = QStringLiteral("info");
        break;
    case ReferenceStatus::Current:
        statusKey = QStringLiteral("reference.state.current");
        tone = QStringLiteral("success");
        break;
    case ReferenceStatus::Stale:
        statusKey = QStringLiteral("reference.state.stale");
        tone = QStringLiteral("warning");
        break;
    case ReferenceStatus::Error:
        statusKey = QStringLiteral("reference.state.error");
        tone = QStringLiteral("error");
        break;
    }

    const QString statusText = UiStrings::text(statusKey);
    statusBadge_->setText(statusText);
    statusValueLabel_->setText(statusText);
    setTone(statusBadge_, tone);
    referenceDetailLabel_->setText(referenceDetail());
    sourceValueLabel_->setText(referenceState_.source.trimmed().isEmpty()
                                   ? UiStrings::text(QStringLiteral("reference.sourceUnavailable"))
                                   : referenceState_.source.trimmed());
    updateClockDisplay();
}

void MainWindow::updateBusyDisplay()
{
    const bool timeBusy = busyState_.refreshing || busyState_.syncing
        || busyState_.savingServers || referenceState_.status == ReferenceStatus::Loading;
    refreshButton_->setText(busyState_.refreshing
                                    || referenceState_.status == ReferenceStatus::Loading
                                ? UiStrings::text(QStringLiteral("busy.refreshing"))
                                : UiStrings::text(QStringLiteral("action.refresh")));
    syncButton_->setText(busyState_.syncing
                             ? UiStrings::text(QStringLiteral("busy.syncing"))
                             : UiStrings::text(QStringLiteral("action.syncNow")));
    removeScheduleButton_->setText(busyState_.removingSchedule
                                       ? UiStrings::text(QStringLiteral("busy.removing"))
                                       : UiStrings::text(QStringLiteral("action.removeSchedule")));
    applyScheduleButton_->setText(busyState_.changingSchedule
                                      ? UiStrings::text(QStringLiteral("busy.updatingSchedule"))
                                      : UiStrings::text(QStringLiteral("action.apply")));
    refreshButton_->setEnabled(!timeBusy);
    syncButton_->setEnabled(!timeBusy);
    editServersButton_->setEnabled(!busyState_.savingServers && !busyState_.syncing);

    const bool scheduleBusy = busyState_.changingSchedule || busyState_.removingSchedule
        || scheduleState_.status == ScheduleStatus::Updating;
    scheduleEnabledCheck_->setEnabled(!scheduleBusy);
    intervalSpinBox_->setEnabled(!scheduleBusy && scheduleEnabledCheck_->isChecked());
    applyScheduleButton_->setEnabled(!scheduleBusy);
    removeScheduleButton_->setEnabled(scheduleState_.exists && !scheduleBusy);
}

void MainWindow::updateScheduleDisplay()
{
    scheduleStatusValueLabel_->setText(scheduleStatusText());
}

void MainWindow::updateServerDisplay()
{
    serverList_->clear();
    if (servers_.isEmpty()) {
        auto *placeholder = new QListWidgetItem(
            UiStrings::text(QStringLiteral("servers.noneConfigured")), serverList_);
        placeholder->setFlags(Qt::NoItemFlags);
        placeholder->setForeground(QColor(QStringLiteral("#667085")));
        serverCountLabel_->setText(UiStrings::text(QStringLiteral("servers.count.none")));
        return;
    }

    for (int index = 0; index < servers_.size(); ++index) {
        serverList_->addItem(QStringLiteral("%1   %2").arg(index + 1).arg(servers_.at(index)));
    }
    const QString countKey = servers_.size() == 1
        ? QStringLiteral("servers.count.one")
        : QStringLiteral("servers.count.many");
    serverCountLabel_->setText(UiStrings::text(countKey).arg(servers_.size()));
}

void MainWindow::updateResponsiveLayout()
{
    if (lowerGrid_ == nullptr || contentScrollArea_ == nullptr || contentWidget_ == nullptr) {
        return;
    }

    const int viewportWidth = contentScrollArea_->viewport()->width();
    const int usableWidth = qMax(0, viewportWidth - 24);
    contentWidget_->setMinimumWidth(qMin(1040, usableWidth));

    languageLabel_->setVisible(width() >= 720);
    const bool clocksStacked = width() < 700;
    const bool compact = width() < 780;
    if (lowerGrid_->property("compact").toBool() == compact
        && clockGrid_->property("stacked").toBool() == clocksStacked
        && lowerGrid_->property("initialized").toBool()) {
        return;
    }

    clockGrid_->removeWidget(standardClockFrame_);
    clockGrid_->removeWidget(systemClockFrame_);
    if (clocksStacked) {
        clockGrid_->addWidget(standardClockFrame_, 0, 0);
        clockGrid_->addWidget(systemClockFrame_, 1, 0);
        clockGrid_->setColumnStretch(0, 1);
        clockGrid_->setColumnStretch(1, 0);
    } else {
        clockGrid_->addWidget(standardClockFrame_, 0, 0);
        clockGrid_->addWidget(systemClockFrame_, 0, 1);
        clockGrid_->setColumnStretch(0, 1);
        clockGrid_->setColumnStretch(1, 1);
    }

    lowerGrid_->removeWidget(schedulePanel_);
    lowerGrid_->removeWidget(serversPanel_);
    if (compact) {
        lowerGrid_->addWidget(schedulePanel_, 0, 0);
        lowerGrid_->addWidget(serversPanel_, 1, 0);
        lowerGrid_->setColumnStretch(0, 1);
        lowerGrid_->setColumnStretch(1, 0);
        serverList_->setMinimumHeight(78);
    } else {
        lowerGrid_->addWidget(schedulePanel_, 0, 0);
        lowerGrid_->addWidget(serversPanel_, 0, 1);
        lowerGrid_->setColumnStretch(0, 1);
        lowerGrid_->setColumnStretch(1, 1);
        serverList_->setMinimumHeight(82);
    }
    clockGrid_->setProperty("stacked", clocksStacked);
    lowerGrid_->setProperty("compact", compact);
    lowerGrid_->setProperty("initialized", true);
}

void MainWindow::applyHeaderLogo()
{
    const QPixmap logo = brandLogoPixmap(44);
    logoLabel_->setPixmap(logo);
    if (logo.isNull()) {
        logoLabel_->hide();
        return;
    }
    logoLabel_->setFixedSize(logo.deviceIndependentSize().toSize());
    logoLabel_->show();
}

void MainWindow::showAboutDialog()
{
    if (aboutDialog_ == nullptr) {
        aboutDialog_ = new AboutDialog(this);
    } else {
        aboutDialog_->retranslateUi();
    }
    aboutDialog_->exec();
}

void MainWindow::showServerEditor()
{
    if (serverEditor_ != nullptr) {
        serverEditor_->raise();
        serverEditor_->activateWindow();
        return;
    }

    serverEditor_ = new ServerEditorDialog(servers_, defaultServers_, this);
    connect(serverEditor_, &ServerEditorDialog::saveRequested, this, [this](const QStringList &servers) {
        hasOperationResult_ = false;
        operationBanner_->hide();
        busyState_.savingServers = true;
        updateBusyDisplay();
        emit serversSaveRequested(servers);
    });
    connect(serverEditor_, &QDialog::finished, this, [this] {
        serverEditor_->deleteLater();
        serverEditor_ = nullptr;
    });
    serverEditor_->show();
}

void MainWindow::showOperationBanner(const OperationResult &result)
{
    const bool warning = !result.success && result.failure == OperationFailure::Cancelled;
    const QString tone = result.success ? QStringLiteral("success")
                                       : warning ? QStringLiteral("warning") : QStringLiteral("error");
    setTone(operationBanner_, tone);
    setTone(operationTextLabel_, tone);
    operationTextLabel_->setText(operationMessage(result));
    operationIconLabel_->hide();
    operationBanner_->setVisible(true);
    ++bannerGeneration_;
    if (!result.success) {
        return;
    }
    const int generation = bannerGeneration_;
    QTimer::singleShot(8000, this, [this, generation] {
        if (generation != bannerGeneration_ || !lastOperationResult_.success) {
            return;
        }
        hasOperationResult_ = false;
        operationBanner_->hide();
    });
}

bool MainWindow::shouldShowOperationBanner(const OperationResult &result) const
{
    if (!result.success) {
        return true;
    }
    switch (result.operation) {
    case OperationKind::ManualSync:
    case OperationKind::ChangeSchedule:
    case OperationKind::RemoveSchedule:
        return true;
    case OperationKind::Refresh:
    case OperationKind::SaveServers:
        return false;
    }
    return false;
}

QString MainWindow::operationMessage(const OperationResult &result) const
{
    if (result.success) {
        switch (result.operation) {
        case OperationKind::Refresh:
            return UiStrings::text(QStringLiteral("operation.refresh.success"));
        case OperationKind::ManualSync:
            return UiStrings::text(QStringLiteral("operation.sync.success"));
        case OperationKind::SaveServers:
            return UiStrings::text(QStringLiteral("operation.servers.success"));
        case OperationKind::ChangeSchedule:
            return UiStrings::text(QStringLiteral("operation.schedule.success"));
        case OperationKind::RemoveSchedule:
            return UiStrings::text(QStringLiteral("operation.removeSchedule.success"));
        }
    }

    switch (result.failure) {
    case OperationFailure::RemoteTimeUnavailable:
        return UiStrings::text(QStringLiteral("operation.failure.remoteTime"));
    case OperationFailure::ServerUnavailable:
        return UiStrings::text(QStringLiteral("operation.failure.server"));
    case OperationFailure::AuthorizationDenied:
        return UiStrings::text(QStringLiteral("operation.failure.authorizationDenied"));
    case OperationFailure::InsufficientPrivilege:
        return UiStrings::text(QStringLiteral("operation.failure.privilege"));
    case OperationFailure::SystemTimeRejected:
        return UiStrings::text(QStringLiteral("operation.failure.systemRejected"));
    case OperationFailure::VerificationFailed:
        return UiStrings::text(QStringLiteral("operation.failure.verification"));
    case OperationFailure::UncertainState:
        return UiStrings::text(QStringLiteral("operation.failure.uncertain"));
    case OperationFailure::Cancelled:
        return UiStrings::text(QStringLiteral("operation.failure.cancelled"));
    case OperationFailure::InvalidConfiguration:
        return UiStrings::text(QStringLiteral("operation.failure.invalidConfiguration"));
    case OperationFailure::SaveFailed:
        return UiStrings::text(QStringLiteral("operation.failure.save"));
    case OperationFailure::ScheduleAuthorizationDenied:
        return UiStrings::text(QStringLiteral("operation.failure.scheduleAuthorization"));
    case OperationFailure::ScheduleUpdateFailed:
        return UiStrings::text(QStringLiteral("operation.failure.scheduleUpdate"));
    case OperationFailure::ScheduleRemoveFailed:
        return UiStrings::text(QStringLiteral("operation.failure.scheduleRemove"));
    case OperationFailure::None:
    case OperationFailure::Unknown:
        break;
    }

    switch (result.operation) {
    case OperationKind::Refresh:
        return UiStrings::text(QStringLiteral("operation.refresh.failure"));
    case OperationKind::ManualSync:
        return UiStrings::text(QStringLiteral("operation.sync.failure"));
    case OperationKind::SaveServers:
        return UiStrings::text(QStringLiteral("operation.servers.failure"));
    case OperationKind::ChangeSchedule:
        return UiStrings::text(QStringLiteral("operation.schedule.failure"));
    case OperationKind::RemoveSchedule:
        return UiStrings::text(QStringLiteral("operation.removeSchedule.failure"));
    }
    return UiStrings::text(QStringLiteral("operation.failure.unknown"));
}

QString MainWindow::referenceDetail() const
{
    switch (referenceState_.status) {
    case ReferenceStatus::NoData:
        return UiStrings::text(QStringLiteral("reference.detail.noData"));
    case ReferenceStatus::Loading:
        return referenceState_.hasReferenceTime
            ? UiStrings::text(QStringLiteral("reference.detail.refreshingExisting"))
            : UiStrings::text(QStringLiteral("reference.detail.loading"));
    case ReferenceStatus::Current:
        return UiStrings::text(QStringLiteral("reference.detail.current"));
    case ReferenceStatus::Stale:
        return UiStrings::text(QStringLiteral("reference.detail.stale"));
    case ReferenceStatus::Error:
        switch (referenceState_.failure) {
        case ReferenceFailure::NetworkUnavailable:
            return UiStrings::text(QStringLiteral("reference.detail.networkError"));
        case ReferenceFailure::ServerUnavailable:
            return UiStrings::text(QStringLiteral("reference.detail.serverError"));
        case ReferenceFailure::None:
        case ReferenceFailure::Unknown:
            return UiStrings::text(QStringLiteral("reference.detail.error"));
        }
    }
    return {};
}

QString MainWindow::calibrationAgeText(qint64 totalSeconds) const
{
    totalSeconds = qMax<qint64>(0, totalSeconds);
    if (totalSeconds < 5) {
        return UiStrings::text(QStringLiteral("age.justNow"));
    }
    if (totalSeconds < 60) {
        return UiStrings::text(QStringLiteral("age.seconds")).arg(totalSeconds);
    }

    const qint64 totalMinutes = totalSeconds / 60;
    if (totalMinutes < 60) {
        return UiStrings::text(QStringLiteral("age.minutes")).arg(totalMinutes);
    }

    const qint64 totalHours = totalMinutes / 60;
    const qint64 remainingMinutes = totalMinutes % 60;
    if (totalHours < 24) {
        return UiStrings::text(QStringLiteral("age.hours"))
            .arg(totalHours)
            .arg(remainingMinutes);
    }

    const qint64 days = totalHours / 24;
    const qint64 remainingHours = totalHours % 24;
    return UiStrings::text(QStringLiteral("age.days")).arg(days).arg(remainingHours);
}

QString MainWindow::formattedDate(const QDateTime &dateTime) const
{
    if (UiStrings::languageTag() == QStringLiteral("zh-CN")) {
        const QLocale locale(QLocale::Chinese, QLocale::China);
        return locale.toString(dateTime.date(), QStringLiteral("yyyy年M月d日 dddd"));
    }

    const QLocale locale(QLocale::English, QLocale::UnitedStates);
    return locale.toString(dateTime.date(), QStringLiteral("ddd, MMM d, yyyy"));
}

QString MainWindow::systemTimeZoneText() const
{
    QString zoneName = cachedZoneAbbreviation_;
    if (zoneName.isEmpty()) {
        zoneName = UiStrings::text(QStringLiteral("system.localZone"));
    }

    const int offsetSeconds = cachedOffsetSeconds_;
    const QChar sign = offsetSeconds < 0 ? QLatin1Char('-') : QLatin1Char('+');
    const int absoluteOffset = qAbs(offsetSeconds);
    const QString offsetText = QStringLiteral("UTC%1%2:%3")
                                   .arg(sign)
                                   .arg(absoluteOffset / 3600, 2, 10, QLatin1Char('0'))
                                   .arg((absoluteOffset % 3600) / 60, 2, 10, QLatin1Char('0'));
    return UiStrings::text(QStringLiteral("system.timezone"))
        .arg(zoneName, offsetText);
}

QString MainWindow::timeDifferenceText(qint64 differenceMilliseconds) const
{
    const qint64 roundedSeconds = differenceMilliseconds >= 0
        ? (differenceMilliseconds + 500) / 1000
        : -((-differenceMilliseconds + 500) / 1000);
    if (roundedSeconds == 0) {
        return UiStrings::text(QStringLiteral("comparison.aligned"));
    }

    const QString key = roundedSeconds > 0
        ? QStringLiteral("comparison.systemAhead")
        : QStringLiteral("comparison.systemBehind");
    return UiStrings::text(key).arg(qAbs(roundedSeconds));
}

QString MainWindow::scheduleStatusText() const
{
    switch (scheduleState_.status) {
    case ScheduleStatus::NotConfigured:
        return UiStrings::text(QStringLiteral("schedule.state.notConfigured"));
    case ScheduleStatus::Active:
        return UiStrings::text(scheduleState_.intervalMinutes == 1
                                   ? QStringLiteral("schedule.state.active.one")
                                   : QStringLiteral("schedule.state.active.many"))
            .arg(scheduleState_.intervalMinutes);
    case ScheduleStatus::Paused:
        return UiStrings::text(QStringLiteral("schedule.state.paused"));
    case ScheduleStatus::Updating:
        return UiStrings::text(QStringLiteral("schedule.state.updating"));
    case ScheduleStatus::Error:
        return UiStrings::text(QStringLiteral("schedule.state.error"));
    }
    return {};
}

void MainWindow::setTone(QWidget *widget, const QString &tone)
{
    widget->setProperty("tone", tone);
    repolish(widget);
}

} // namespace TimeSync::Ui
