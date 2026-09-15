#include "app_controller.h"

#include "../ui/main_window.h"
#include "../ui/ui_strings.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QThread>

namespace TimeSync {
namespace {

Ui::OperationFailure mapBackendFailure(const OperationFailure failure)
{
    switch (failure) {
    case OperationFailure::None:
        return Ui::OperationFailure::None;
    case OperationFailure::Busy:
        return Ui::OperationFailure::SystemTimeConfirming;
    case OperationFailure::RemoteTimeUnavailable:
        return Ui::OperationFailure::RemoteTimeUnavailable;
    case OperationFailure::ServerUnavailable:
        return Ui::OperationFailure::ServerUnavailable;
    case OperationFailure::Cancelled:
        return Ui::OperationFailure::Cancelled;
    case OperationFailure::InvalidConfiguration:
        return Ui::OperationFailure::InvalidConfiguration;
    case OperationFailure::PrivilegeMissing:
        return Ui::OperationFailure::InsufficientPrivilege;
    case OperationFailure::AuthorizationDenied:
        return Ui::OperationFailure::AuthorizationDenied;
    case OperationFailure::SystemTimeRejected:
        return Ui::OperationFailure::SystemTimeRejected;
    case OperationFailure::VerificationFailed:
        return Ui::OperationFailure::VerificationFailed;
    case OperationFailure::UncertainState:
        return Ui::OperationFailure::UncertainState;
    case OperationFailure::ScheduleUpdateFailed:
        return Ui::OperationFailure::ScheduleUpdateFailed;
    case OperationFailure::ScheduleRemoveFailed:
        return Ui::OperationFailure::ScheduleRemoveFailed;
    case OperationFailure::UnsupportedPlatform:
    case OperationFailure::Other:
        return Ui::OperationFailure::Unknown;
    }
    return Ui::OperationFailure::Unknown;
}

} // namespace

AppController::AppController(Ui::MainWindow *window,
                             const ParsedCommand &command,
                             QObject *parent)
    : QObject(parent)
    , window_(window)
    , command_(command)
    , coordinator_(this)
    , powerMonitor_(this)
{
    clockTimer_ = new QTimer(this);
    clockTimer_->setInterval(500);
    connect(clockTimer_, &QTimer::timeout, this, &AppController::onClockTimer);

    connect(&coordinator_,
            &SyncCoordinator::referenceChanged,
            this,
            &AppController::onReferenceChanged);
    connect(&coordinator_,
            &SyncCoordinator::operationFinished,
            this,
            &AppController::onOperationFinished);
    connect(&coordinator_, &SyncCoordinator::systemClockMutationStarted, this, [this] {
        if (window_ != nullptr) {
            window_->setSystemClockReadSuspended(true);
            window_->setSystemClockFollowsTrusted(true);
        }
    });
    connect(&coordinator_, &SyncCoordinator::systemClockMutationEnded, this, [this] {
        if (window_ != nullptr) {
            window_->setSystemClockReadSuspended(false);
            window_->setSystemClockFollowsTrusted(false);
        }
        activeCoordinatorGeneration_ = 0;
    });
    coordinator_.attachPowerResumeMonitor(&powerMonitor_);
    connect(&powerMonitor_, &PowerResumeMonitor::resumed, this, &AppController::onPowerResumed);
}

AppController::~AppController()
{
    shutdown();
}

void AppController::start(QCoreApplication *application)
{
    if (started_ || window_ == nullptr) {
        return;
    }
    started_ = true;
    application_ = application;
    connectUi();
    powerMonitor_.install(application_);
    clockTimer_->start();
    loadConfiguration();
}

void AppController::shutdown()
{
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    if (application_ != nullptr) {
        powerMonitor_.uninstall(application_);
    }
    if (elevationCancel_) {
        elevationCancel_->store(true);
    }
    taskJobActive_ = false;
    taskStateKnown_ = false;
    pendingScheduleChange_ = false;
    pendingScheduleRemove_ = false;
    for (const QString &path : pendingHandoffPaths_) {
        QFile::remove(path);
    }
    pendingHandoffPaths_.clear();
    coordinator_.cancel();
    referenceLoading_ = false;
    busyState_ = {};
    if (window_ != nullptr) {
        window_->setBusyState(busyState_);
    }
    if (backgroundThread_ != nullptr) {
        QThread *thread = backgroundThread_;
        backgroundThread_ = nullptr;
        thread->requestInterruption();
        thread->wait();
        delete thread;
    }
    if (clockTimer_ != nullptr) {
        clockTimer_->stop();
    }
}

void AppController::connectUi()
{
    connect(window_, &Ui::MainWindow::refreshRequested, this, &AppController::onRefreshRequested);
    connect(window_, &Ui::MainWindow::manualSyncRequested, this, &AppController::onManualSyncRequested);
    connect(window_,
            &Ui::MainWindow::serversSaveRequested,
            this,
            &AppController::onServersSaveRequested);
    connect(window_,
            &Ui::MainWindow::scheduleChangeRequested,
            this,
            &AppController::onScheduleChangeRequested);
    connect(window_,
            &Ui::MainWindow::scheduleRemoveRequested,
            this,
            &AppController::onScheduleRemoveRequested);
    connect(window_,
            &Ui::MainWindow::languageChangeRequested,
            this,
            &AppController::onLanguageChangeRequested);
}

void AppController::loadConfiguration()
{
    const Result<ConfigLoadResult> loaded = configRepository_.load(command_.configPath);
    if (!loaded) {
        configReady_ = false;
        configPath_ = ConfigRepository::resolvePath(command_.configPath);
        window_->setDefaultServerList(ConfigRepository::defaultServers());
        window_->setServerList({});
        window_->setLanguage(QStringLiteral("zh-CN"));
        scheduleState_ = {};
        window_->setScheduleState(scheduleState_);
        emitUiErrorOperation(Ui::OperationKind::Refresh, loaded.error());
        applyReferenceState();
        return;
    }

    configReady_ = true;
    config_ = loaded.value().config;
    configPath_ = loaded.value().path;
    scheduleState_.intervalMinutes = config_.scheduleIntervalMinutes;
    initializeUiFromConfiguration();
    queryTaskState();
    startInitialRefresh();
}

void AppController::initializeUiFromConfiguration()
{
    window_->setDefaultServerList(ConfigRepository::defaultServers());
    window_->setServerList(config_.servers);
    window_->setLanguage(normalizeLanguageTag(config_.language));
    window_->setScheduleState(scheduleState_);
    applyReferenceState();
}

void AppController::startInitialRefresh()
{
    startRefresh();
}

bool AppController::canStartOperation() const
{
    return configReady_ && !shuttingDown_ && !coordinator_.isBusy() && !elevationActive_
        && !scheduleMutationActive_;
}

void AppController::setLoadingState(const bool loading)
{
    referenceLoading_ = loading;
    if (loading) {
        referenceFailure_ = Ui::ReferenceFailure::None;
    }
    busyState_.refreshing = loading;
    applyReferenceState();
    window_->setBusyState(busyState_);
}

void AppController::startRefresh()
{
    if (!configReady_ || shuttingDown_ || elevationActive_ || scheduleMutationActive_) {
        return;
    }
    if (coordinator_.isSystemWriteInProgress()) {
        busyState_.refreshing = false;
        window_->setBusyState(busyState_);
        emitUiOperation(Ui::OperationKind::Refresh,
                        false,
                        Ui::OperationFailure::SystemTimeConfirming);
        return;
    }
    if (coordinator_.isBusy()) {
        busyState_.refreshing = false;
        window_->setBusyState(busyState_);
        return;
    }

    refreshPending_ = false;
    setLoadingState(true);
    const Result<quint64> started = coordinator_.startRefresh(config_);
    if (!started) {
        referenceLoading_ = false;
        applyReferenceState();
        busyState_.refreshing = false;
        window_->setBusyState(busyState_);
        if (started.error().code == ErrorCode::Busy) {
            emitUiOperation(Ui::OperationKind::Refresh,
                            false,
                            Ui::OperationFailure::SystemTimeConfirming);
            return;
        }
        emitUiErrorOperation(Ui::OperationKind::Refresh, started.error());
        return;
    }
    activeCoordinatorGeneration_ = started.value();
}

void AppController::onRefreshRequested()
{
    startRefresh();
}

void AppController::onManualSyncRequested()
{
    if (!configReady_ || shuttingDown_ || elevationActive_ || scheduleMutationActive_) {
        emitUiOperation(Ui::OperationKind::ManualSync,
                        false,
                        Ui::OperationFailure::Unknown);
        return;
    }
    if (coordinator_.isSystemWriteInProgress()) {
        return;
    }

    const Result<quint64> started =
        coordinator_.startSync(config_, SyncMode::AuthorizedDirect, false);
    if (!started) {
        emitUiErrorOperation(Ui::OperationKind::ManualSync, started.error());
        return;
    }
    activeCoordinatorGeneration_ = started.value();
    busyState_.syncing = coordinator_.isBusy();
    window_->setBusyState(busyState_);
}

void AppController::onReferenceChanged(const TrustedClockState &state)
{
    Q_UNUSED(state)
    if (!shuttingDown_) {
        applyReferenceState();
    }
}

void AppController::onOperationFinished(const OperationResult &result)
{
    if (shuttingDown_) {
        return;
    }
    if (activeCoordinatorGeneration_ != 0 && result.generation != activeCoordinatorGeneration_) {
        return;
    }
    if (result.operation != OperationKind::Refresh
        && result.operation != OperationKind::ManualSync) {
        return;
    }

    referenceLoading_ = false;
    if (result.succeeded) {
        referenceFailure_ = Ui::ReferenceFailure::None;
    } else {
        referenceFailure_ = result.failure == OperationFailure::RemoteTimeUnavailable
            ? Ui::ReferenceFailure::ServerUnavailable
            : Ui::ReferenceFailure::NetworkUnavailable;
    }
    applyReferenceState();

    if (result.operation == OperationKind::ManualSync) {
        busyState_.syncing = false;
        busyState_.refreshing = false;
        window_->setBusyState(busyState_);
        window_->setOperationResult(toUiOperationResult(result));
        if (!coordinator_.isSystemWriteInProgress()) {
            activeCoordinatorGeneration_ = 0;
        }
        return;
    }

    busyState_.refreshing = false;
    window_->setBusyState(busyState_);
    window_->setOperationResult(toUiOperationResult(result));
    activeCoordinatorGeneration_ = 0;
    maybeStartPendingRefresh();
}

void AppController::applyReferenceState()
{
    if (window_ == nullptr) {
        return;
    }
    window_->setReferenceState(toUiReferenceState(coordinator_.trustedClock()->state(),
                                                  referenceLoading_,
                                                  referenceFailure_));
}

void AppController::onClockTimer()
{
    applyReferenceState();
}

void AppController::maybeStartPendingRefresh()
{
    if (!refreshPending_ || shuttingDown_ || !configReady_ || referenceLoading_
        || coordinator_.isBusy() || elevationActive_ || scheduleMutationActive_ || taskJobActive_) {
        return;
    }
    refreshPending_ = false;
    startRefresh();
}

Ui::ReferenceState AppController::toUiReferenceState(const TrustedClockState &state,
                                                     const bool loading,
                                                     const Ui::ReferenceFailure failure)
{
    Ui::ReferenceState result;
    result.failure = failure;
    result.hasReferenceTime = state.available && state.shanghai.isValid();
    result.beijingTime = state.shanghai;
    result.source = state.source;
    result.calibrationAgeSeconds = state.calibrationAgeSeconds;
    if (loading) {
        result.status = Ui::ReferenceStatus::Loading;
    } else {
        switch (state.status) {
        case TrustedClockStatus::Unavailable:
            result.status = Ui::ReferenceStatus::NoData;
            break;
        case TrustedClockStatus::Current:
            result.status = Ui::ReferenceStatus::Current;
            break;
        case TrustedClockStatus::Stale:
            result.status = Ui::ReferenceStatus::Stale;
            break;
        case TrustedClockStatus::Error:
            result.status = Ui::ReferenceStatus::Error;
            break;
        }
    }
    if (!result.hasReferenceTime) {
        result.beijingTime = {};
        result.calibrationAgeSeconds = -1;
    }
    return result;
}

Ui::OperationResult AppController::toUiOperationResult(const OperationResult &result)
{
    Ui::OperationResult mapped;
    mapped.operation = mapOperationKind(result.operation);
    mapped.success = result.succeeded;
    mapped.failure = mapBackendFailure(result.failure);
    if (result.succeeded) {
        mapped.failure = Ui::OperationFailure::None;
    }
    return mapped;
}

void AppController::emitUiOperation(const Ui::OperationKind operation,
                                     const bool success,
                                     const Ui::OperationFailure failure,
                                     const bool uncertain,
                                     const QString &source)
{
    Q_UNUSED(uncertain)
    Ui::OperationResult result;
    result.operation = operation;
    result.success = success;
    result.failure = success ? Ui::OperationFailure::None : failure;
    window_->setOperationResult(result);
    Q_UNUSED(source)
}

void AppController::emitUiErrorOperation(const Ui::OperationKind operation,
                                          const Error &error,
                                          const bool uncertain)
{
    emitUiOperation(operation, false, mapErrorToUiFailure(error.code, operation), uncertain);
}

Ui::OperationKind AppController::mapOperationKind(const OperationKind operation)
{
    switch (operation) {
    case OperationKind::Refresh:
        return Ui::OperationKind::Refresh;
    case OperationKind::ManualSync:
    case OperationKind::ScheduledSync:
        return Ui::OperationKind::ManualSync;
    case OperationKind::InstallTask:
        return Ui::OperationKind::ChangeSchedule;
    case OperationKind::RemoveTask:
        return Ui::OperationKind::RemoveSchedule;
    }
    return Ui::OperationKind::Refresh;
}

Ui::OperationFailure AppController::mapErrorToUiFailure(const ErrorCode code,
                                                         const Ui::OperationKind operation)
{
    switch (code) {
    case ErrorCode::InvalidArgument:
    case ErrorCode::InvalidConfiguration:
    case ErrorCode::ConfigurationReadFailed:
        return Ui::OperationFailure::InvalidConfiguration;
    case ErrorCode::ConfigurationWriteFailed:
        return operation == Ui::OperationKind::SaveServers
            ? Ui::OperationFailure::SaveFailed
            : Ui::OperationFailure::InvalidConfiguration;
    case ErrorCode::NetworkUnavailable:
    case ErrorCode::SourceTimeout:
    case ErrorCode::NoValidSource:
    case ErrorCode::ProtocolRejected:
        return Ui::OperationFailure::RemoteTimeUnavailable;
    case ErrorCode::PrivilegeMissing:
    case ErrorCode::PrivilegeAdjustmentFailed:
        return Ui::OperationFailure::InsufficientPrivilege;
    case ErrorCode::ElevationCancelled:
        return Ui::OperationFailure::AuthorizationDenied;
    case ErrorCode::VerificationFailed:
        return Ui::OperationFailure::VerificationFailed;
    case ErrorCode::Cancelled:
        return Ui::OperationFailure::Cancelled;
    case ErrorCode::SchedulerAccessDenied:
        return Ui::OperationFailure::ScheduleAuthorizationDenied;
    case ErrorCode::SchedulerMutationFailed:
    case ErrorCode::TaskNotFound:
        return operation == Ui::OperationKind::RemoveSchedule
            ? Ui::OperationFailure::ScheduleRemoveFailed
            : Ui::OperationFailure::ScheduleUpdateFailed;
    case ErrorCode::SystemTimeRejected:
        return Ui::OperationFailure::SystemTimeRejected;
    case ErrorCode::UncertainState:
        return Ui::OperationFailure::UncertainState;
    case ErrorCode::Busy:
        return Ui::OperationFailure::SystemTimeConfirming;
    case ErrorCode::ElevationFailed:
    case ErrorCode::UnsupportedPlatform:
    case ErrorCode::InternalError:
    case ErrorCode::None:
        return Ui::OperationFailure::Unknown;
    }
    return Ui::OperationFailure::Unknown;
}

Ui::ReferenceFailure AppController::mapReferenceFailure(const NtpFailureKind kind)
{
    return kind == NtpFailureKind::NoValidSource || kind == NtpFailureKind::ProtocolRejected
        ? Ui::ReferenceFailure::ServerUnavailable
        : Ui::ReferenceFailure::NetworkUnavailable;
}

Ui::ScheduleStatus AppController::mapScheduleStatus(const ScheduledTaskInfo &info)
{
    if (!info.exists) {
        return Ui::ScheduleStatus::NotConfigured;
    }
    return info.enabled ? Ui::ScheduleStatus::Active : Ui::ScheduleStatus::Paused;
}

QString AppController::normalizeLanguageTag(const QString &tag)
{
    const QString normalized = tag.trimmed().replace(QLatin1Char('_'), QLatin1Char('-'));
    if (normalized.compare(QStringLiteral("zh"), Qt::CaseInsensitive) == 0
        || normalized.startsWith(QStringLiteral("zh-"), Qt::CaseInsensitive)) {
        return QStringLiteral("zh-CN");
    }
    if (normalized.compare(QStringLiteral("en"), Qt::CaseInsensitive) == 0
        || normalized.startsWith(QStringLiteral("en-"), Qt::CaseInsensitive)) {
        return QStringLiteral("en-US");
    }
    return normalized;
}

void AppController::queryTaskState()
{
    if (taskJobActive_ || elevationActive_ || shuttingDown_) {
        return;
    }
    taskStateKnown_ = false;
    startTaskJob(
        [] { return TaskSchedulerBackend().query(); },
        [this](const Result<ScheduledTaskInfo> &result) { applyTaskState(result); });
}

void AppController::startTaskJob(std::function<Result<ScheduledTaskInfo>()> operation,
                                 std::function<void(const Result<ScheduledTaskInfo> &)> completed)
{
    if (taskJobActive_ || elevationActive_ || shuttingDown_) {
        return;
    }
    taskJobActive_ = true;
    QThread *thread = QThread::create(
        [this, operation = std::move(operation), completed = std::move(completed)]() mutable {
            const Result<ScheduledTaskInfo> result = operation();
            QMetaObject::invokeMethod(
                this,
                [this, result, completed]() mutable {
                    taskJobActive_ = false;
                    if (!shuttingDown_) {
                        completed(result);
                        maybeStartPendingRefresh();
                    }
                },
                Qt::QueuedConnection);
        });
    backgroundThread_ = thread;
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (backgroundThread_ == thread) {
            backgroundThread_ = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}

void AppController::applyTaskState(const Result<ScheduledTaskInfo> &result)
{
    if (!result) {
        taskStateKnown_ = false;
        scheduleState_.status = Ui::ScheduleStatus::Error;
        window_->setScheduleState(scheduleState_);
        const bool hadPendingChange = pendingScheduleChange_;
        const bool hadPendingRemove = pendingScheduleRemove_;
        pendingScheduleChange_ = false;
        pendingScheduleRemove_ = false;
        if (hadPendingChange) {
            busyState_.changingSchedule = false;
            window_->setBusyState(busyState_);
            emitUiErrorOperation(Ui::OperationKind::ChangeSchedule, result.error());
        } else if (hadPendingRemove) {
            busyState_.removingSchedule = false;
            window_->setBusyState(busyState_);
            emitUiErrorOperation(Ui::OperationKind::RemoveSchedule, result.error());
        }
        return;
    }
    taskStateKnown_ = true;
    const ScheduledTaskInfo &info = result.value();
    scheduleState_.exists = info.exists;
    scheduleState_.enabled = info.enabled;
    if (info.intervalMinutes > 0) {
        scheduleState_.intervalMinutes = info.intervalMinutes;
    }
    scheduleState_.status = mapScheduleStatus(info);
    window_->setScheduleState(scheduleState_);
    if (pendingScheduleChange_) {
        const bool enabled = pendingScheduleEnabled_;
        const int interval = pendingScheduleInterval_;
        pendingScheduleChange_ = false;
        onScheduleChangeRequested(enabled, interval);
    } else if (pendingScheduleRemove_) {
        pendingScheduleRemove_ = false;
        onScheduleRemoveRequested();
    }
}

void AppController::startElevation(const ElevatedPurpose purpose,
                                   const QStringList &arguments,
                                   const HandoffFile &handoff)
{
    if (shuttingDown_) {
        QFile::remove(handoff.path);
        pendingHandoffPaths_.remove(handoff.path);
        return;
    }
    if (taskJobActive_ || (backgroundThread_ != nullptr && !elevationActive_)) {
        if (pendingHandoffPaths_.contains(handoff.path)) {
            return;
        }
        pendingHandoffPaths_.insert(handoff.path);
        QTimer::singleShot(50, this, [this, purpose, arguments, handoff] {
            startElevation(purpose, arguments, handoff);
        });
        return;
    }
    if (backgroundThread_ != nullptr || elevationActive_) {
        pendingHandoffPaths_.remove(handoff.path);
        QFile::remove(handoff.path);
        const Error error{ErrorCode::Busy, QStringLiteral("background operation is busy")};
        finishScheduleMutationFailure(error);
        return;
    }

    pendingHandoffPaths_.remove(handoff.path);
    elevationActive_ = true;
    elevationCancel_ = std::make_shared<std::atomic_bool>(false);
    const std::shared_ptr<std::atomic_bool> cancel = elevationCancel_;
    const QString executablePath = QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath();
    QThread *thread = QThread::create(
        [this, purpose, arguments, handoff, executablePath, cancel] {
            const Result<int> launched = ElevationBroker::runAsAdministrator(
                executablePath,
                arguments,
                120000,
                [cancel] { return cancel->load(); });
            const Result<HandoffPayload> result = [&]() {
                if (!launched) {
                    QFile::remove(handoff.path);
                    return Result<HandoffPayload>::failure(launched.error());
                }
                Result<HandoffPayload> handoffResult = ResultHandoff::readAndRemove(handoff);
                if (handoffResult && static_cast<int>(handoffResult.value().exitCode) != launched.value()) {
                    return Result<HandoffPayload>::failure(
                        {ErrorCode::UncertainState,
                         QStringLiteral("elevated result exit mismatch"),
                         0,
                         true});
                }
                return handoffResult;
            }();
            QMetaObject::invokeMethod(
                this,
                [this, purpose, result]() { handleElevationResult(purpose, result); },
                Qt::QueuedConnection);
        });
    backgroundThread_ = thread;
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread] {
        if (backgroundThread_ == thread) {
            backgroundThread_ = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}

void AppController::handleElevationResult(const ElevatedPurpose purpose,
                                           const Result<HandoffPayload> &result)
{
    elevationActive_ = false;
    elevationCancel_.reset();
    if (!result) {
        finishScheduleMutationFailure(result.error(), result.error().uncertain);
        return;
    }
    const HandoffPayload &payload = result.value();
    if (payload.exitCode != ExitCode::Success || payload.errorCode != ErrorCode::None) {
        const Error error{payload.errorCode == ErrorCode::None ? ErrorCode::InternalError
                                                               : payload.errorCode,
                          QStringLiteral("elevated operation failed"),
                          0,
                          payload.uncertain};
        finishScheduleMutationFailure(error, payload.uncertain);
        return;
    }
    finishScheduleMutationSuccess();
}

void AppController::onServersSaveRequested(const QStringList &servers)
{
    if (!configReady_ || elevationActive_ || scheduleMutationActive_) {
        emitUiOperation(Ui::OperationKind::SaveServers, false, Ui::OperationFailure::SaveFailed);
        return;
    }
    AppConfig candidate = config_;
    candidate.servers = servers;
    const Result<void> validation = ConfigRepository::validate(candidate);
    if (!validation) {
        emitUiErrorOperation(Ui::OperationKind::SaveServers, validation.error());
        return;
    }
    const Result<void> saved = configRepository_.save(configPath_, candidate);
    if (!saved) {
        emitUiErrorOperation(Ui::OperationKind::SaveServers, saved.error());
        return;
    }
    config_ = candidate;
    window_->setServerList(config_.servers);
    emitUiOperation(Ui::OperationKind::SaveServers, true);
    refreshPending_ = true;
    maybeStartPendingRefresh();
}

void AppController::onLanguageChangeRequested(const QString &languageTag)
{
    if (!configReady_) {
        return;
    }
    const QString normalized = normalizeLanguageTag(languageTag);
    if (normalized != QStringLiteral("zh-CN") && normalized != QStringLiteral("en-US")) {
        window_->setLanguage(config_.language);
        emitUiErrorOperation(Ui::OperationKind::Refresh,
                             {ErrorCode::InvalidConfiguration, QStringLiteral("unsupported language")});
        return;
    }
    languageBeforeChange_ = config_.language;
    AppConfig candidate = config_;
    candidate.language = normalized;
    const Result<void> saved = configRepository_.save(configPath_, candidate);
    if (!saved) {
        window_->setLanguage(languageBeforeChange_);
        emitUiErrorOperation(Ui::OperationKind::Refresh, saved.error());
        return;
    }
    config_ = candidate;
}

void AppController::onScheduleChangeRequested(const bool enabled, const int intervalMinutes)
{
    if (!configReady_ || elevationActive_ || scheduleMutationActive_) {
        emitUiOperation(Ui::OperationKind::ChangeSchedule,
                        false,
                        Ui::OperationFailure::ScheduleUpdateFailed);
        return;
    }
    if (!taskStateKnown_) {
        pendingScheduleChange_ = true;
        pendingScheduleEnabled_ = enabled;
        pendingScheduleInterval_ = intervalMinutes;
        pendingScheduleRemove_ = false;
        scheduleState_.status = Ui::ScheduleStatus::Updating;
        busyState_.changingSchedule = true;
        window_->setScheduleState(scheduleState_);
        window_->setBusyState(busyState_);
        queryTaskState();
        return;
    }
    AppConfig candidate = config_;
    candidate.scheduleIntervalMinutes = intervalMinutes;
    const Result<void> validation = ConfigRepository::validate(candidate);
    if (!validation) {
        emitUiErrorOperation(Ui::OperationKind::ChangeSchedule, validation.error());
        return;
    }
    const Result<void> saved = configRepository_.save(configPath_, candidate);
    if (!saved) {
        emitUiErrorOperation(Ui::OperationKind::ChangeSchedule, saved.error());
        return;
    }
    scheduleRollbackConfig_ = config_;
    scheduleRollbackState_ = scheduleState_;
    config_ = candidate;
    scheduleDesiredEnabled_ = enabled;
    scheduleDesiredInterval_ = intervalMinutes;
    scheduleMutationActive_ = true;
    scheduleState_.status = Ui::ScheduleStatus::Updating;
    busyState_.changingSchedule = true;
    window_->setScheduleState(scheduleState_);
    window_->setBusyState(busyState_);

    if (!enabled && !scheduleState_.exists) {
        scheduleMutation_ = ScheduleMutation::None;
        scheduleState_.enabled = false;
        scheduleState_.status = Ui::ScheduleStatus::NotConfigured;
        scheduleMutationActive_ = false;
        busyState_.changingSchedule = false;
        window_->setScheduleState(scheduleState_);
        window_->setBusyState(busyState_);
        emitUiOperation(Ui::OperationKind::ChangeSchedule, true);
        return;
    }

    const Result<HandoffFile> handoff = ResultHandoff::create();
    if (!handoff) {
        finishScheduleMutationFailure(handoff.error());
        return;
    }
    QStringList arguments;
    ElevatedPurpose purpose;
    if (enabled) {
        scheduleMutation_ = ScheduleMutation::Install;
        purpose = ElevatedPurpose::InstallTask;
        arguments = {QStringLiteral("--elevated-install-task"),
                     QStringLiteral("--config"),
                     QFileInfo(configPath_).absoluteFilePath(),
                     QStringLiteral("--result"),
                     handoff.value().path,
                     QStringLiteral("--nonce"),
                     handoff.value().nonce};
    } else {
        scheduleMutation_ = ScheduleMutation::Disable;
        purpose = ElevatedPurpose::DisableTask;
        arguments = {QStringLiteral("--elevated-set-task-enabled"),
                     QStringLiteral("--enabled"),
                     QStringLiteral("false"),
                     QStringLiteral("--result"),
                     handoff.value().path,
                     QStringLiteral("--nonce"),
                     handoff.value().nonce};
    }
    startElevation(purpose, arguments, handoff.value());
}

void AppController::onScheduleRemoveRequested()
{
    if (!configReady_ || elevationActive_ || scheduleMutationActive_) {
        emitUiOperation(Ui::OperationKind::RemoveSchedule,
                        false,
                        Ui::OperationFailure::ScheduleRemoveFailed);
        return;
    }
    if (!taskStateKnown_) {
        pendingScheduleChange_ = false;
        pendingScheduleRemove_ = true;
        scheduleState_.status = Ui::ScheduleStatus::Updating;
        busyState_.removingSchedule = true;
        window_->setScheduleState(scheduleState_);
        window_->setBusyState(busyState_);
        queryTaskState();
        return;
    }
    if (!scheduleState_.exists) {
        scheduleState_.enabled = false;
        scheduleState_.status = Ui::ScheduleStatus::NotConfigured;
        window_->setScheduleState(scheduleState_);
        emitUiOperation(Ui::OperationKind::RemoveSchedule, true);
        return;
    }
    const Result<HandoffFile> handoff = ResultHandoff::create();
    if (!handoff) {
        emitUiErrorOperation(Ui::OperationKind::RemoveSchedule, handoff.error());
        return;
    }
    scheduleMutation_ = ScheduleMutation::Remove;
    scheduleRollbackState_ = scheduleState_;
    scheduleMutationActive_ = true;
    busyState_.removingSchedule = true;
    scheduleState_.status = Ui::ScheduleStatus::Updating;
    window_->setScheduleState(scheduleState_);
    window_->setBusyState(busyState_);
    const QStringList arguments = {QStringLiteral("--elevated-remove-task"),
                                   QStringLiteral("--result"),
                                   handoff.value().path,
                                   QStringLiteral("--nonce"),
                                   handoff.value().nonce};
    startElevation(ElevatedPurpose::RemoveTask, arguments, handoff.value());
}

void AppController::finishScheduleMutationSuccess()
{
    const bool remove = scheduleMutation_ == ScheduleMutation::Remove;
    const bool exists = !remove && (scheduleMutation_ != ScheduleMutation::Disable || scheduleState_.exists);
    scheduleState_.exists = exists;
    scheduleState_.enabled = !remove && scheduleDesiredEnabled_;
    scheduleState_.intervalMinutes = scheduleDesiredInterval_;
    scheduleState_.status = mapScheduleStatus({exists,
                                                scheduleState_.enabled,
                                                scheduleState_.intervalMinutes,
                                                {},
                                                {}});
    busyState_.changingSchedule = false;
    busyState_.removingSchedule = false;
    scheduleMutationActive_ = false;
    scheduleMutation_ = ScheduleMutation::None;
    window_->setScheduleState(scheduleState_);
    window_->setBusyState(busyState_);
    emitUiOperation(remove ? Ui::OperationKind::RemoveSchedule
                           : Ui::OperationKind::ChangeSchedule,
                    true);
    maybeStartPendingRefresh();
}

void AppController::rollbackScheduleConfiguration(const bool uncertainOnFailure,
                                                   const Error &operationError)
{
    config_ = scheduleRollbackConfig_;
    const Result<void> rollback = configRepository_.save(configPath_, config_);
    if (!rollback) {
        scheduleState_.status = Ui::ScheduleStatus::Error;
        scheduleMutationActive_ = false;
        finishScheduleMutationFailure(
            {ErrorCode::UncertainState, QStringLiteral("schedule and configuration rollback failed"), 0, true},
            true);
        return;
    }
    scheduleMutationActive_ = false;
    finishScheduleMutationFailure(operationError, uncertainOnFailure);
}

void AppController::finishScheduleMutationFailure(const Error &error, const bool uncertain)
{
    if (scheduleMutation_ != ScheduleMutation::Remove && scheduleMutationActive_) {
        rollbackScheduleConfiguration(uncertain || error.uncertain, error);
        return;
    }
    const bool remove = scheduleMutation_ == ScheduleMutation::Remove;
    busyState_.changingSchedule = false;
    busyState_.removingSchedule = false;
    scheduleMutationActive_ = false;
    scheduleMutation_ = ScheduleMutation::None;
    if (!(uncertain || error.uncertain)) {
        scheduleState_ = scheduleRollbackState_;
    } else {
        scheduleState_.status = Ui::ScheduleStatus::Error;
    }
    window_->setScheduleState(scheduleState_);
    window_->setBusyState(busyState_);
    emitUiErrorOperation(remove
                             ? Ui::OperationKind::RemoveSchedule
                             : Ui::OperationKind::ChangeSchedule,
                         error,
                         uncertain);
    maybeStartPendingRefresh();
}

void AppController::onPowerResumed()
{
    if (shuttingDown_ || !configReady_) {
        return;
    }
    referenceFailure_ = Ui::ReferenceFailure::None;
    refreshPending_ = true;
    if (coordinator_.isBusy()) {
        coordinator_.cancel();
    }
    applyReferenceState();
    maybeStartPendingRefresh();
}

} // namespace TimeSync
