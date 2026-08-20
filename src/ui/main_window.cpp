#include "main_window.h"

#include "server_editor_dialog.h"
#include "ui_strings.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStyle>
#include <QTimer>
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

    setMinimumSize(760, 560);
    resize(980, 720);
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
    hasOperationResult_ = true;
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
    showOperationBanner(result);
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
    headerLayout->setContentsMargins(24, 12, 24, 12);
    headerLayout->setSpacing(12);
    appTitleLabel_ = new QLabel(headerFrame_);
    appTitleLabel_->setObjectName(QStringLiteral("appTitle"));
    languageLabel_ = new QLabel(headerFrame_);
    languageLabel_->setObjectName(QStringLiteral("fieldLabel"));
    languageCombo_ = new QComboBox(headerFrame_);
    languageCombo_->setMinimumSize(132, 40);
    languageLabel_->setBuddy(languageCombo_);
    headerLayout->addWidget(appTitleLabel_);
    headerLayout->addStretch(1);
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
    shellLayout->setContentsMargins(16, 0, 16, 0);
    shellLayout->setSpacing(0);
    contentWidget_ = new QWidget(scrollContent);
    contentWidget_->setMaximumWidth(1240);
    contentWidget_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *contentLayout = new QVBoxLayout(contentWidget_);
    contentLayout->setContentsMargins(8, 20, 8, 24);
    contentLayout->setSpacing(16);

    operationBanner_ = new QFrame(contentWidget_);
    operationBanner_->setObjectName(QStringLiteral("operationBanner"));
    operationBanner_->setVisible(false);
    auto *bannerLayout = new QHBoxLayout(operationBanner_);
    bannerLayout->setContentsMargins(12, 8, 8, 8);
    bannerLayout->setSpacing(8);
    operationIconLabel_ = new QLabel(operationBanner_);
    operationIconLabel_->setFixedSize(20, 20);
    operationIconLabel_->setAlignment(Qt::AlignCenter);
    operationTextLabel_ = new QLabel(operationBanner_);
    operationTextLabel_->setWordWrap(true);
    dismissBannerButton_ = new QPushButton(operationBanner_);
    dismissBannerButton_->setObjectName(QStringLiteral("bannerDismissButton"));
    dismissBannerButton_->setFlat(true);
    dismissBannerButton_->setIcon(style()->standardIcon(QStyle::SP_DialogCloseButton));
    dismissBannerButton_->setIconSize(QSize(16, 16));
    dismissBannerButton_->setFixedSize(40, 40);
    bannerLayout->addWidget(operationIconLabel_);
    bannerLayout->addWidget(operationTextLabel_, 1);
    bannerLayout->addWidget(dismissBannerButton_);
    contentLayout->addWidget(operationBanner_);

    referencePanel_ = new QFrame(contentWidget_);
    referencePanel_->setObjectName(QStringLiteral("surfacePanel"));
    auto *referenceLayout = new QVBoxLayout(referencePanel_);
    referenceLayout->setContentsMargins(24, 14, 24, 16);
    referenceLayout->setSpacing(5);
    referenceHeadingLabel_ = new QLabel(referencePanel_);
    referenceHeadingLabel_->setObjectName(QStringLiteral("sectionHeading"));
    referenceHeadingLabel_->setAlignment(Qt::AlignHCenter);
    timeLabel_ = new QLabel(referencePanel_);
    timeLabel_->setObjectName(QStringLiteral("timeLabel"));
    timeLabel_->setAlignment(Qt::AlignCenter);
    timeLabel_->setMinimumHeight(58);
    dateLabel_ = new QLabel(referencePanel_);
    dateLabel_->setObjectName(QStringLiteral("dateLabel"));
    dateLabel_->setAlignment(Qt::AlignCenter);
    timezoneLabel_ = new QLabel(referencePanel_);
    timezoneLabel_->setObjectName(QStringLiteral("secondaryText"));
    timezoneLabel_->setAlignment(Qt::AlignCenter);
    statusBadge_ = new QLabel(referencePanel_);
    statusBadge_->setObjectName(QStringLiteral("statusBadge"));
    statusBadge_->setAlignment(Qt::AlignCenter);
    statusBadge_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    referenceDetailLabel_ = new QLabel(referencePanel_);
    referenceDetailLabel_->setObjectName(QStringLiteral("secondaryText"));
    referenceDetailLabel_->setAlignment(Qt::AlignCenter);
    referenceDetailLabel_->setWordWrap(true);

    referenceLayout->addWidget(referenceHeadingLabel_);
    referenceLayout->addWidget(timeLabel_);
    referenceLayout->addWidget(dateLabel_);
    referenceLayout->addWidget(timezoneLabel_);
    referenceLayout->addSpacing(2);
    referenceLayout->addWidget(statusBadge_, 0, Qt::AlignHCenter);
    referenceLayout->addWidget(referenceDetailLabel_);

    auto *metadataLayout = new QGridLayout;
    metadataLayout->setContentsMargins(0, 6, 0, 2);
    metadataLayout->setHorizontalSpacing(28);
    metadataLayout->setVerticalSpacing(3);
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
    refreshButton_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refreshButton_->setIconSize(QSize(18, 18));
    syncButton_ = new QPushButton(referencePanel_);
    syncButton_->setObjectName(QStringLiteral("primaryButton"));
    syncButton_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    syncButton_->setIconSize(QSize(18, 18));
    referenceButtonLayout->addWidget(refreshButton_);
    referenceButtonLayout->addWidget(syncButton_);
    referenceButtonLayout->addStretch(1);
    referenceLayout->addLayout(referenceButtonLayout);
    contentLayout->addWidget(referencePanel_);

    lowerGrid_ = new QGridLayout;
    lowerGrid_->setContentsMargins(0, 0, 0, 0);
    lowerGrid_->setHorizontalSpacing(16);
    lowerGrid_->setVerticalSpacing(16);

    schedulePanel_ = new QFrame(contentWidget_);
    schedulePanel_->setObjectName(QStringLiteral("surfacePanel"));
    auto *scheduleLayout = new QGridLayout(schedulePanel_);
    scheduleLayout->setContentsMargins(20, 18, 20, 20);
    scheduleLayout->setHorizontalSpacing(12);
    scheduleLayout->setVerticalSpacing(8);
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
    intervalSpinBox_->setMinimumWidth(172);
    intervalSpinBox_->setMaximumWidth(224);
    intervalLabel_->setBuddy(intervalSpinBox_);
    scheduleStatusCaptionLabel_ = new QLabel(schedulePanel_);
    scheduleStatusCaptionLabel_->setObjectName(QStringLiteral("fieldLabel"));
    scheduleStatusValueLabel_ = new QLabel(schedulePanel_);
    scheduleStatusValueLabel_->setObjectName(QStringLiteral("scheduleStatus"));
    scheduleStatusValueLabel_->setWordWrap(true);
    applyScheduleButton_ = new QPushButton(schedulePanel_);
    applyScheduleButton_->setObjectName(QStringLiteral("primaryButton"));
    applyScheduleButton_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    applyScheduleButton_->setIconSize(QSize(18, 18));
    removeScheduleButton_ = new QPushButton(schedulePanel_);
    removeScheduleButton_->setObjectName(QStringLiteral("dangerButton"));
    removeScheduleButton_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    removeScheduleButton_->setIconSize(QSize(18, 18));
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
    serversLayout->setContentsMargins(20, 18, 20, 20);
    serversLayout->setSpacing(8);
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
    serverList_->setMinimumHeight(96);
    serverList_->setUniformItemSizes(true);
    serverList_->setTextElideMode(Qt::ElideMiddle);
    serverList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    serversLayout->addWidget(serverList_, 1);
    editServersButton_ = new QPushButton(serversPanel_);
    editServersButton_->setObjectName(QStringLiteral("secondaryButton"));
    editServersButton_->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    editServersButton_->setIconSize(QSize(18, 18));
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

    connect(dismissBannerButton_, &QPushButton::clicked, operationBanner_, &QWidget::hide);
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
            background: #F4F6F8;
        }
        QScrollArea#contentScrollArea { border: 0; }
        QFrame#headerFrame {
            background: #FFFFFF;
            border: 0;
            border-bottom: 1px solid #DDE2E8;
        }
        QLabel#appTitle {
            color: #172033;
            font-size: 18px;
            font-weight: 600;
        }
        QFrame#surfacePanel {
            background: #FFFFFF;
            border: 1px solid #DDE2E8;
            border-radius: 8px;
        }
        QLabel#sectionHeading {
            color: #172033;
            font-size: 16px;
            font-weight: 600;
        }
        QLabel#timeLabel {
            color: #111827;
            font-family: "Cascadia Mono", "Consolas", monospace;
            font-size: 48px;
            font-weight: 600;
        }
        QLabel#dateLabel {
            color: #344054;
            font-size: 16px;
            font-weight: 500;
        }
        QLabel#secondaryText { color: #667085; }
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
        QLabel#statusBadge[tone="info"] { color: #2457B8; background: #EAF1FF; }
        QLabel#statusBadge[tone="success"] { color: #176B43; background: #E8F6EE; }
        QLabel#statusBadge[tone="warning"] { color: #8A4B08; background: #FFF2D8; }
        QLabel#statusBadge[tone="error"] { color: #A61B12; background: #FDEBEA; }
        QFrame#operationBanner {
            border: 1px solid;
            border-radius: 6px;
        }
        QFrame#operationBanner[tone="success"] { background: #ECF8F1; border-color: #A8DDBE; }
        QFrame#operationBanner[tone="warning"] { background: #FFF6E5; border-color: #EBCB8B; }
        QFrame#operationBanner[tone="error"] { background: #FDEEEE; border-color: #E9B4B0; }
        QPushButton, QToolButton {
            min-height: 40px;
            padding: 0 14px;
            border-radius: 6px;
            font-weight: 600;
        }
        QPushButton#primaryButton {
            color: #FFFFFF;
            background: #2F6FED;
            border: 1px solid #2F6FED;
        }
        QPushButton#primaryButton:hover { background: #245DCE; border-color: #245DCE; }
        QPushButton#primaryButton:pressed { background: #1D4FAF; border-color: #1D4FAF; }
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
        QPushButton#bannerDismissButton:focus { border: 2px solid #2F6FED; }
        QPushButton:disabled, QToolButton:disabled {
            color: #98A2B3;
            background: #EEF1F4;
            border-color: #DDE2E8;
        }
        QPushButton:focus, QToolButton:focus, QComboBox:focus, QSpinBox:focus,
        QLineEdit:focus, QListWidget:focus {
            border: 2px solid #2F6FED;
        }
        QLineEdit, QSpinBox, QComboBox, QListWidget {
            color: #172033;
            background: #FFFFFF;
            border: 1px solid #C9D1DB;
            border-radius: 6px;
            selection-background-color: #DCE8FF;
            selection-color: #172033;
        }
        QLineEdit, QSpinBox, QComboBox { padding: 0 10px; }
        QSpinBox { min-width: 172px; }
        QLineEdit[error="true"] { border: 2px solid #B42318; }
        QListWidget { padding: 4px; outline: 0; }
        QListWidget::item {
            min-height: 30px;
            padding: 2px 8px;
            border-radius: 4px;
        }
        QListWidget::item:selected { background: #DCE8FF; color: #172033; }
        QCheckBox { spacing: 8px; }
        QCheckBox::indicator { width: 18px; height: 18px; }
        QCheckBox:focus { color: #1F56C2; }
        QLabel#validationLabel { color: transparent; font-size: 13px; }
        QLabel#validationLabel[visibleError="true"] { color: #B42318; }
        QFrame#divider { color: #DDE2E8; }
        QDialog#serverEditorDialog { background: #FFFFFF; }
        QScrollBar:vertical {
            width: 12px;
            background: #F4F6F8;
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
    sourceCaptionLabel_->setText(UiStrings::text(QStringLiteral("reference.source")));
    statusCaptionLabel_->setText(UiStrings::text(QStringLiteral("reference.status")));
    calibrationCaptionLabel_->setText(UiStrings::text(QStringLiteral("reference.lastCalibration")));

    scheduleHeadingLabel_->setText(UiStrings::text(QStringLiteral("schedule.heading")));
    scheduleEnabledCheck_->setText(UiStrings::text(QStringLiteral("schedule.enabled")));
    intervalLabel_->setText(UiStrings::text(QStringLiteral("schedule.interval")));
    intervalSpinBox_->setSuffix(UiStrings::text(QStringLiteral("schedule.minutesSuffix")));
    scheduleStatusCaptionLabel_->setText(UiStrings::text(QStringLiteral("schedule.status")));
    removeScheduleButton_->setText(busyState_.removingSchedule
                                       ? UiStrings::text(QStringLiteral("busy.removing"))
                                       : UiStrings::text(QStringLiteral("action.removeSchedule")));
    applyScheduleButton_->setText(busyState_.changingSchedule
                                      ? UiStrings::text(QStringLiteral("busy.updatingSchedule"))
                                      : UiStrings::text(QStringLiteral("action.apply")));

    serversHeadingLabel_->setText(UiStrings::text(QStringLiteral("servers.heading")));
    serverList_->setAccessibleName(UiStrings::text(QStringLiteral("servers.heading")));
    editServersButton_->setText(UiStrings::text(QStringLiteral("action.editServers")));
    refreshButton_->setText(busyState_.refreshing
                                    || referenceState_.status == ReferenceStatus::Loading
                                ? UiStrings::text(QStringLiteral("busy.refreshing"))
                                : UiStrings::text(QStringLiteral("action.refresh")));
    syncButton_->setText(busyState_.syncing
                             ? UiStrings::text(QStringLiteral("busy.syncing"))
                             : UiStrings::text(QStringLiteral("action.syncNow")));

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
}

void MainWindow::updateClockDisplay()
{
    if (!referenceState_.hasReferenceTime || !referenceState_.beijingTime.isValid()) {
        timeLabel_->setText(QStringLiteral("--:--:--"));
        dateLabel_->setText(UiStrings::text(QStringLiteral("reference.dateUnavailable")));
        calibrationValueLabel_->setText(UiStrings::text(QStringLiteral("reference.never")));
        return;
    }

    const qint64 elapsedMilliseconds = referenceElapsed_.isValid() ? referenceElapsed_.elapsed() : 0;
    const QDateTime beijingTime = referenceState_.beijingTime
                                      .addMSecs(elapsedMilliseconds)
                                      .toOffsetFromUtc(BeijingUtcOffsetSeconds);
    timeLabel_->setText(beijingTime.toString(QStringLiteral("HH:mm:ss")));

    if (UiStrings::languageTag() == QStringLiteral("zh-CN")) {
        const QLocale locale(QLocale::Chinese, QLocale::China);
        dateLabel_->setText(locale.toString(beijingTime.date(), QStringLiteral("yyyy年M月d日 dddd")));
    } else {
        const QLocale locale(QLocale::English, QLocale::UnitedStates);
        dateLabel_->setText(locale.toString(beijingTime.date(), QStringLiteral("dddd, MMMM d, yyyy")));
    }

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
    refreshButton_->setEnabled(!timeBusy);
    syncButton_->setEnabled(!timeBusy);
    editServersButton_->setEnabled(!busyState_.savingServers && !busyState_.syncing);

    const bool scheduleBusy = busyState_.changingSchedule || busyState_.removingSchedule
        || scheduleState_.status == ScheduleStatus::Updating;
    scheduleEnabledCheck_->setEnabled(!scheduleBusy);
    intervalSpinBox_->setEnabled(!scheduleBusy && scheduleEnabledCheck_->isChecked());
    applyScheduleButton_->setEnabled(!scheduleBusy);
    removeScheduleButton_->setEnabled(scheduleState_.exists && !scheduleBusy);

    retranslateUi();
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
    const int usableWidth = qMax(0, viewportWidth - 32);
    contentWidget_->setMinimumWidth(qMin(1240, usableWidth));

    languageLabel_->setVisible(width() >= 820);
    const bool compact = width() < 920;
    if (lowerGrid_->property("compact").toBool() == compact
        && lowerGrid_->property("initialized").toBool()) {
        return;
    }

    lowerGrid_->removeWidget(schedulePanel_);
    lowerGrid_->removeWidget(serversPanel_);
    if (compact) {
        lowerGrid_->addWidget(schedulePanel_, 0, 0);
        lowerGrid_->addWidget(serversPanel_, 1, 0);
        lowerGrid_->setColumnStretch(0, 1);
        lowerGrid_->setColumnStretch(1, 0);
        serverList_->setMinimumHeight(88);
    } else {
        lowerGrid_->addWidget(schedulePanel_, 0, 0);
        lowerGrid_->addWidget(serversPanel_, 0, 1);
        lowerGrid_->setColumnStretch(0, 1);
        lowerGrid_->setColumnStretch(1, 1);
        serverList_->setMinimumHeight(96);
    }
    lowerGrid_->setProperty("compact", compact);
    lowerGrid_->setProperty("initialized", true);
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
    operationTextLabel_->setText(operationMessage(result));
    const QStyle::StandardPixmap icon = result.success
        ? QStyle::SP_DialogApplyButton
        : warning ? QStyle::SP_MessageBoxWarning : QStyle::SP_MessageBoxCritical;
    operationIconLabel_->setPixmap(style()->standardIcon(icon).pixmap(18, 18));
    operationBanner_->setVisible(true);
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
